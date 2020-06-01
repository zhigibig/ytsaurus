#include "clickhouse_server.h"

#include "clickhouse_config.h"
#include "config_repository.h"
#include "logger.h"
#include "http_handler.h"
#include "tcp_handler.h"
#include "poco_config.h"
#include "host.h"
#include "helpers.h"

#include <yt/core/misc/fs.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/coroutine.h>

#include <Server/IServer.h>
#include <Interpreters/AsynchronousMetrics.h>
#include <Interpreters/Context.h>
#include <Interpreters/ProcessList.h>
#include <Interpreters/ExternalDictionariesLoader.h>
#include <Storages/System/attachSystemTables.h>
#include <Storages/StorageFactory.h>
#include <Storages/StorageMemory.h>
#include <TableFunctions/registerTableFunctions.h>
#include <Dictionaries/registerDictionaries.h>
#include <Functions/registerFunctions.h>
#include <AggregateFunctions/registerAggregateFunctions.h>
#include <Common/ClickHouseRevision.h>
#include <Databases/DatabaseMemory.h>
#include <Access/AccessControlManager.h>
#include <Access/MemoryAccessStorage.h>

#include <Storages/System/StorageSystemProcesses.h>
#include <Storages/System/StorageSystemAsynchronousMetrics.h>
#include <Storages/System/StorageSystemDictionaries.h>
#include <Storages/System/StorageSystemMetrics.h>


#include <Poco/DirectoryIterator.h>
#include <Poco/File.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/TCPServer.h>
#include <Poco/ThreadPool.h>
#include <Poco/Util/LayeredConfiguration.h>


namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

using namespace NConcurrency;

static const auto& Logger = ClickHouseYtLogger;
static const auto& ProfilingPeriod = TDuration::Seconds(1);

////////////////////////////////////////////////////////////////////////////////

class TClickHouseServer
    : public DB::IServer
    , public IClickHouseServer
{
public:
    TClickHouseServer(
        THost* host,
        TClickHouseConfigPtr config)
        : Host_(std::move(host))
        , Config_(config)
        , SharedContext_(DB::Context::createShared())
        , ServerContext_(std::make_unique<DB::Context>(DB::Context::createGlobal(SharedContext_.get())))
        , LayeredConfig_(ConvertToLayeredConfig(ConvertToNode(Config_)))
        , ProfilingQueue_(New<TActionQueue>("NativeProfiling"))
        , ProfilingExecutor_(New<TPeriodicExecutor>(
            ProfilingQueue_->GetInvoker(),
            BIND(&TClickHouseServer::OnProfiling, MakeWeak(this)),
            ProfilingPeriod))
    {
        SetupLogger();
        SetupContext();
        WarmupDictionaries();
    }

    // IClickHouseServer overrides:

    virtual void Start() override
    {
        SetupServers();
        ProfilingExecutor_->Start();

        for (auto& server : Servers_) {
            server->start();
        }
    }

    virtual void Stop() override
    {
        Cancelled_ = true;

        for (auto& server : Servers_) {
            if (auto httpPtr = dynamic_cast<Poco::Net::HTTPServer*>(server.get())) {
                // Special method of HTTP Server, will break all active connections.
                httpPtr->stopAll(true);
            } else {
                server->stop();
            }
        }
    }

    DB::Context* GetContext() override
    {
        return ServerContext_.get();
    }

    // DB::Server overrides:

    Poco::Logger& logger() const override
    {
        return Poco::Logger::root();
    }

    Poco::Util::LayeredConfiguration& config() const override
    {
        return *const_cast<Poco::Util::LayeredConfiguration*>(LayeredConfig_.get());
    }

    DB::Context& context() const override
    {
        return *ServerContext_;
    }

    bool isCancelled() const override
    {
        return Cancelled_;
    }

private:
    THost* Host_;
    const TClickHouseConfigPtr Config_;
    DB::SharedContextHolder SharedContext_;
    std::unique_ptr<DB::Context> ServerContext_;

    // Poco representation of Config_.
    Poco::AutoPtr<Poco::Util::LayeredConfiguration> LayeredConfig_;

    Poco::AutoPtr<Poco::Channel> LogChannel;

    std::unique_ptr<DB::AsynchronousMetrics> AsynchronousMetrics_;

    std::unique_ptr<Poco::ThreadPool> ServerPool_;
    std::vector<std::unique_ptr<Poco::Net::TCPServer>> Servers_;

    std::atomic<bool> Cancelled_ { false };

    TActionQueuePtr ProfilingQueue_;
    TPeriodicExecutorPtr ProfilingExecutor_;

    std::shared_ptr<DB::IDatabase> SystemDatabase_;

    ext::scope_guard DictionaryGuard_;

    void SetupLogger()
    {
        LogChannel = CreateLogChannel(ClickHouseNativeLogger);

        auto& rootLogger = Poco::Logger::root();
        rootLogger.close();
        rootLogger.setChannel(LogChannel);
        rootLogger.setLevel(Config_->LogLevel);
    }

    void SetupContext()
    {
        YT_LOG_INFO("Setting up context");

        ServerContext_->makeGlobalContext();
        ServerContext_->setApplicationType(DB::Context::ApplicationType::SERVER);
        ServerContext_->setConfig(LayeredConfig_);
        ServerContext_->setUsersConfig(ConvertToPocoConfig(ConvertToNode(Config_->Users)));

        DB::registerFunctions();
        DB::registerAggregateFunctions();
        DB::registerTableFunctions();
        DB::registerStorageMemory(DB::StorageFactory::instance());
        DB::registerDictionaries();

        CurrentMetrics::set(CurrentMetrics::Revision, ClickHouseRevision::get());
        CurrentMetrics::set(CurrentMetrics::VersionInteger, ClickHouseRevision::getVersionInteger());

        // Initialize DateLUT early, to not interfere with running time of first query.
        YT_LOG_DEBUG("Initializing DateLUT");
        DateLUT::setDefaultTimezone(Config_->Timezone.value());
        DateLUT::instance();
        YT_LOG_DEBUG("DateLUT initialized (TimeZone: %v)", DateLUT::instance().getTimeZone());

        // Limit on total number of concurrently executed queries.
        ServerContext_->getProcessList().setMaxSize(Config_->MaxConcurrentQueries);

        ServerContext_->setDefaultProfiles(*LayeredConfig_);

        YT_LOG_DEBUG("Profiles, processes & uncompressed cache set up");

        NFS::MakeDirRecursive(Config_->DataPath);
        ServerContext_->setPath(Config_->DataPath);

        // This object will periodically calculate asynchronous metrics.
        AsynchronousMetrics_ = std::make_unique<DB::AsynchronousMetrics>(*ServerContext_);

        YT_LOG_DEBUG("Asynchronous metrics set up");

        // Database for system tables.

        YT_LOG_DEBUG("Setting up databases");

        SystemDatabase_ = std::make_shared<DB::DatabaseMemory>(DB::DatabaseCatalog::SYSTEM_DATABASE, *ServerContext_);

        DB::DatabaseCatalog::instance().attachDatabase(DB::DatabaseCatalog::SYSTEM_DATABASE, SystemDatabase_);

        SystemDatabase_->attachTable("processes", DB::StorageSystemProcesses::create("processes"));
        SystemDatabase_->attachTable("metrics", DB::StorageSystemMetrics::create("metrics"));
        SystemDatabase_->attachTable("dictionaries", DB::StorageSystemDictionaries::create("dictionaries"));
        SystemDatabase_->attachTable("asynchronous_metrics", DB::StorageSystemAsynchronousMetrics::create("asynchronous_metrics", *AsynchronousMetrics_));

        DB::attachSystemTablesLocal(*SystemDatabase_);
        Host_->PopulateSystemDatabase(SystemDatabase_.get());

        DB::DatabaseCatalog::instance().attachDatabase("YT", Host_->CreateYtDatabase());
        ServerContext_->setCurrentDatabase("YT");

        auto DatabaseForTemporaryAndExternalTables = std::make_shared<DB::DatabaseMemory>(DB::DatabaseCatalog::TEMPORARY_DATABASE, *ServerContext_);
        DB::DatabaseCatalog::instance().attachDatabase(DB::DatabaseCatalog::TEMPORARY_DATABASE, DatabaseForTemporaryAndExternalTables);

        YT_LOG_DEBUG("Initializing system logs");
        // XXX(max42): fill link :)
        // NB: under debug build this method does not fit in regular fiber stack
        // due to https://...
        TCoroutine<void()> coroutine(BIND([&] (TCoroutine<void()>& /* self */) { ServerContext_->initializeSystemLogs(); }), EExecutionStackKind::Large);
        coroutine.Run();
        YT_VERIFY(coroutine.IsCompleted());
        YT_LOG_DEBUG("System logs initialized");

        YT_LOG_DEBUG("Setting up access manager");

        ServerContext_->getAccessControlManager().addStorage(std::make_unique<DB::MemoryAccessStorage>());
        RegisterNewUser(ServerContext_->getAccessControlManager(), InternalRemoteUserName);

        YT_LOG_DEBUG("Adding external dictionaries from config");

        DictionaryGuard_ = ServerContext_->getExternalDictionariesLoader().addConfigRepository(CreateDictionaryConfigRepository(Config_->Dictionaries));

        YT_LOG_INFO("Finished setting up context");
    }

    void WarmupDictionaries()
    {
        YT_LOG_INFO("Warming up dictionaries");
        ServerContext_->getEmbeddedDictionaries();
        YT_LOG_INFO("Finished warming up");
    }

    void SetupServers()
    {
#ifdef _linux_
        YT_LOG_INFO("Setting up servers");

        const auto& settings = ServerContext_->getSettingsRef();

        ServerPool_ = std::make_unique<Poco::ThreadPool>(3, Config_->MaxConnections);

        auto setupSocket = [&] (const std::string& host, UInt16 port) {
            Poco::Net::SocketAddress socketAddress;
            socketAddress = Poco::Net::SocketAddress(host, port);
            Poco::Net::ServerSocket socket(socketAddress);
            socket.setReceiveTimeout(settings.receive_timeout);
            socket.setSendTimeout(settings.send_timeout);

            return socket;
        };


        {
            YT_LOG_INFO("Setting up HTTP server");
            auto socket = setupSocket("::", Config_->HttpPort);

            Poco::Timespan keepAliveTimeout(Config_->KeepAliveTimeout, 0);

            Poco::Net::HTTPServerParams::Ptr httpParams = new Poco::Net::HTTPServerParams();
            httpParams->setTimeout(settings.receive_timeout);
            httpParams->setKeepAliveTimeout(keepAliveTimeout);

            Servers_.emplace_back(std::make_unique<Poco::Net::HTTPServer>(
                CreateHttpHandlerFactory(Host_, *this),
                *ServerPool_,
                socket,
                httpParams));
        }

        {
            YT_LOG_INFO("Setting up TCP server");
            auto socket = setupSocket("::", Config_->TcpPort);

            Servers_.emplace_back(std::make_unique<Poco::Net::TCPServer>(
                CreateTcpHandlerFactory(Host_, *this),
                *ServerPool_,
                socket,
                new Poco::Net::TCPServerParams()));
        }

        YT_LOG_INFO("Servers set up");
#endif
    }

    void OnProfiling()
    {
        for (int index = 0; index < static_cast<int>(CurrentMetrics::end()); ++index) {
            const auto* name = CurrentMetrics::getName(index);
            auto value = CurrentMetrics::values[index].load(std::memory_order_relaxed);
            ClickHouseNativeProfiler.Enqueue(
                "/current_metrics/" + CamelCaseToUnderscoreCase(TString(name)),
                value,
                NProfiling::EMetricType::Gauge);
        }

        for (const auto& [name, value] : AsynchronousMetrics_->getValues()) {
            ClickHouseNativeProfiler.Enqueue(
                "/asynchronous_metrics/" + CamelCaseToUnderscoreCase(TString(name)),
                value,
                NProfiling::EMetricType::Gauge);
        }

        for (int index = 0; index < static_cast<int>(ProfileEvents::end()); ++index) {
            const auto* name = ProfileEvents::getName(index);
            auto value = ProfileEvents::global_counters[index].load(std::memory_order_relaxed);
            ClickHouseNativeProfiler.Enqueue(
                "/global_profile_events/" + CamelCaseToUnderscoreCase(TString(name)),
                value,
                NProfiling::EMetricType::Counter);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

IClickHouseServerPtr CreateClickHouseServer(
    THost* host,
    TClickHouseConfigPtr config)
{
    return New<TClickHouseServer>(std::move(host), std::move(config));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NClickHouseServer
