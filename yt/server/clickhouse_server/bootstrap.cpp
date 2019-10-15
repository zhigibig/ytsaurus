#include "bootstrap.h"

#include "private.h"

#include "host.h"

#include "clickhouse_service.h"
#include "config.h"
#include "query_context.h"
#include "query_registry.h"
#include "security_manager.h"

#include <yt/server/clickhouse_server/protos/clickhouse_service.pb.h>

#include <yt/server/lib/admin/admin_service.h>
#include <yt/server/lib/core_dump/core_dumper.h>

#include <yt/ytlib/monitoring/http_integration.h>
#include <yt/ytlib/monitoring/monitoring_manager.h>

#include <yt/ytlib/program/build_attributes.h>
#include <yt/ytlib/program/configure_singletons.h>
#include <yt/ytlib/api/connection.h>
#include <yt/ytlib/api/native/client.h>
#include <yt/ytlib/api/native/connection.h>
#include <yt/ytlib/api/native/client_cache.h>
#include <yt/ytlib/orchid/orchid_service.h>

#include <yt/client/api/client.h>
#include <yt/client/api/client_cache.h>

#include <yt/client/misc/discovery.h>

#include <yt/core/bus/tcp/server.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/throughput_throttler.h>
#include <yt/core/concurrency/thread_pool_poller.h>
#include <yt/core/concurrency/thread_pool.h>

#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/core_dumper.h>
#include <yt/core/misc/ref_counted_tracker_statistics_producer.h>
#include <yt/core/misc/signal_registry.h>
#include <yt/core/misc/crash_handler.h>

#include <yt/core/ytalloc/statistics_producer.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/http/server.h>

#include <yt/core/rpc/bus/server.h>

#include <yt/core/ytree/ephemeral_node_factory.h>
#include <yt/core/ytree/virtual.h>
#include <yt/core/ytree/ypath_client.h>

#include <util/datetime/base.h>

namespace NYT::NClickHouseServer {

using namespace NAdmin;
using namespace NApi;
using namespace NApi::NNative;
using namespace NBus;
using namespace NConcurrency;
using namespace NMonitoring;
using namespace NOrchid;
using namespace NProfiling;
using namespace NRpc;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ServerLogger;

const TString CacheUser = "yt-clickhouse-cache";

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(
    TClickHouseServerBootstrapConfigPtr config,
    INodePtr configNode,
    TString instanceId,
    TString cliqueId,
    ui16 rpcPort,
    ui16 monitoringPort,
    ui16 tcpPort,
    ui16 httpPort)
    : Config_(std::move(config))
    , CliqueId_(std::move(cliqueId))
    , ConfigNode_(std::move(configNode))
    , InstanceId_(std::move(instanceId))
    , RpcPort_(rpcPort)
    , MonitoringPort_(monitoringPort)
    , TcpPort_(tcpPort)
    , HttpPort_(httpPort)
{
    WarnForUnrecognizedOptions(Logger, Config_);
}

void TBootstrap::Run()
{
    ControlQueue_ = New<TActionQueue>("Control");

    BIND(&TBootstrap::DoRun, this)
        .AsyncVia(GetControlInvoker())
        .Run()
        .Get()
        .ThrowOnError();

    Sleep(TDuration::Max());
}

void TBootstrap::DoRun()
{
    YT_LOG_INFO("Starting ClickHouse server");

    // Make RSS predictable.
    NYTAlloc::SetEnableEagerMemoryRelease(true);

    Config_->MonitoringServer->Port = MonitoringPort_;
    HttpServer_ = NHttp::CreateServer(Config_->MonitoringServer);

    NYTree::IMapNodePtr orchidRoot;
    NMonitoring::Initialize(HttpServer_, &MonitoringManager_, &orchidRoot);

    QueryRegistry_ = New<TQueryRegistry>(this);

    // Set up crash handlers.
    QueryRegistry_->SetupStateWritingCrashSignalHandler();
    InstallCrashSignalHandler();

    SetNodeByYPath(
        orchidRoot,
        "/config",
        ConfigNode_);
    SetNodeByYPath(
        orchidRoot,
        "/queries",
        CreateVirtualNode(QueryRegistry_->GetOrchidService()->Via(GetControlInvoker())));
    SetBuildAttributes(orchidRoot, "clickhouse_server");

    // TODO(max42): make configurable.
    WorkerThreadPool_ = New<TThreadPool>(4, "Worker");
    WorkerInvoker_ = WorkerThreadPool_->GetInvoker();
    SerializedWorkerInvoker_ = CreateSerializedInvoker(WorkerInvoker_);

    if (Config_->CoreDumper) {
        CoreDumper_ = NCoreDump::CreateCoreDumper(Config_->CoreDumper);
    }

    Config_->BusServer->Port = RpcPort_;
    BusServer_ = CreateTcpBusServer(Config_->BusServer);

    RpcServer_ = NRpc::NBus::CreateBusServer(BusServer_);

    RpcServer_->RegisterService(CreateAdminService(
        GetControlInvoker(),
        CoreDumper_));

    RpcServer_->RegisterService(CreateOrchidService(
        orchidRoot,
        GetControlInvoker()));

    RpcServer_->RegisterService(CreateClickHouseService(
        this, InstanceId_));

    RpcServer_->Configure(Config_->RpcServer);

    NApi::NNative::TConnectionOptions connectionOptions;
    connectionOptions.RetryRequestQueueSizeLimitExceeded = true;

    Connection_ = NApi::NNative::CreateConnection(
        Config_->ClusterConnection,
        connectionOptions);

    ClientCache_ = New<NApi::NNative::TClientCache>(Config_->ClientCache, Connection_);

    RootClient_ = ClientCache_->GetClient(Config_->User);
    CacheClient_ = ClientCache_->GetClient(CacheUser);

    // Configure clique's directory.
    Config_->Discovery->Directory += "/" + CliqueId_;
    TCreateNodeOptions createCliqueNodeOptions {
        .Recursive = true,
        .IgnoreExisting = true,
    };
    createCliqueNodeOptions.Attributes = ConvertToAttributes(
        THashMap<TString, i64>{{"discovery_version", TDiscovery::Version}});
    WaitFor(RootClient_->CreateNode(
        Config_->Discovery->Directory,
        NObjectClient::EObjectType::MapNode,
        createCliqueNodeOptions))
        .ThrowOnError();

    Host_ = New<TClickHouseHost>(
        this,
        Config_,
        CliqueId_,
        InstanceId_,
        RpcPort_,
        MonitoringPort_,
        TcpPort_,
        HttpPort_);

    if (HttpServer_) {
        YT_LOG_INFO("Listening for HTTP requests on port %v", Config_->MonitoringPort);
        HttpServer_->Start();
    }

    if (RpcServer_) {
        YT_LOG_INFO("Listening for RPC requests on port %v", Config_->RpcPort);
        RpcServer_->Start();
    }

    Host_->Start();

    // Bootstrap never dies, so it is _kinda_ safe.
    TSignalRegistry::Get()->PushCallback(SIGINT, [=] { SigintHandler(); });
}

const IInvokerPtr& TBootstrap::GetControlInvoker() const
{
    return ControlQueue_->GetInvoker();
}

EInstanceState TBootstrap::GetState() const
{
    return SigintCounter_ == 0 ? EInstanceState::Active : EInstanceState::Stopped;
}

void TBootstrap::SigintHandler()
{
    ++SigintCounter_;
    if (SigintCounter_ > 1) {
        _exit(InterruptionExitCode);
    }
    YT_LOG_INFO("Stopping server due to SIGINT");
    Host_->StopDiscovery().Apply(BIND([this] {
        TDelayedExecutor::WaitForDuration(Config_->InterruptionGracefulTimeout);
        WaitFor(GetQueryRegistry()->GetIdleFuture()).ThrowOnError();
        Host_->StopTcpServers();
        Y_UNUSED(WaitFor(RpcServer_->Stop()));
        MonitoringManager_->Stop();
        HttpServer_->Stop();
        TLogManager::StaticShutdown();
        _exit(InterruptionExitCode);
    }).Via(GetControlInvoker()));;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
