#include "log_manager.h"
#include "private.h"
#include "config.h"
#include "log.h"
#include "writer.h"

#include <yt/core/concurrency/fork_aware_spinlock.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/scheduler_thread.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/hash.h>
#include <yt/core/misc/lock_free.h>
#include <yt/core/misc/pattern_formatter.h>
#include <yt/core/misc/proc.h>
#include <yt/core/misc/property.h>
#include <yt/core/misc/raw_formatter.h>
#include <yt/core/misc/singleton.h>
#include <yt/core/misc/variant.h>

#include <yt/core/profiling/profiler.h>
#include <yt/core/profiling/timing.h>

#include <yt/core/ytree/ypath_client.h>
#include <yt/core/ytree/ypath_service.h>
#include <yt/core/ytree/yson_serializable.h>
#include <yt/core/ytree/convert.h>

#include <util/system/defaults.h>
#include <util/system/sigset.h>
#include <util/system/yield.h>

#include <util/string/vector.h>

#include <atomic>

#ifdef _win_
    #include <io.h>
#else
    #include <unistd.h>
#endif

#ifdef _linux_
    #include <sys/inotify.h>
#endif

#include <errno.h>

namespace NYT {
namespace NLogging {

using namespace NYTree;
using namespace NConcurrency;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

static TLogger Logger(SystemLoggingCategory);
static const auto& Profiler = LoggingProfiler;

static const auto ProfilingPeriod = TDuration::Seconds(1);
static const auto DequeuePeriod = TDuration::MilliSeconds(100);

////////////////////////////////////////////////////////////////////////////////

class TNotificationHandle
    : private TNonCopyable
{
public:
    TNotificationHandle()
        : FD_(-1)
    {
#ifdef _linux_
        FD_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        YCHECK(FD_ >= 0);
#endif
    }

    ~TNotificationHandle()
    {
#ifdef _linux_
        YCHECK(FD_ >= 0);
        ::close(FD_);
#endif
    }

    int Poll()
    {
#ifdef _linux_
        YCHECK(FD_ >= 0);

        char buffer[sizeof(struct inotify_event) + NAME_MAX + 1];
        ssize_t rv = HandleEintr(::read, FD_, buffer, sizeof(buffer));

        if (rv < 0) {
            if (errno != EAGAIN) {
                LOG_ERROR(
                    TError::FromSystem(errno),
                    "Unable to poll inotify() descriptor %v",
                    FD_);
            }
        } else if (rv > 0) {
            Y_ASSERT(rv >= sizeof(struct inotify_event));
            struct inotify_event* event = (struct inotify_event*)buffer;

            if (event->mask & IN_ATTRIB) {
                LOG_TRACE(
                    "Watch %v has triggered metadata change (IN_ATTRIB)",
                    event->wd);
            }
            if (event->mask & IN_DELETE_SELF) {
                LOG_TRACE(
                    "Watch %v has triggered a deletion (IN_DELETE_SELF)",
                    event->wd);
            }
            if (event->mask & IN_MOVE_SELF) {
                LOG_TRACE(
                    "Watch %v has triggered a movement (IN_MOVE_SELF)",
                    event->wd);
            }

            return event->wd;
        } else {
            // Do nothing.
        }
#endif
        return 0;
    }

    DEFINE_BYVAL_RO_PROPERTY(int, FD);
};

////////////////////////////////////////////////////////////////////////////////

class TNotificationWatch
    : private TNonCopyable
{
public:
    TNotificationWatch(
        TNotificationHandle* handle,
        const Stroka& path,
        TClosure callback)
        : FD_(handle->GetFD())
        , WD_(-1)
        , Path_(path)
        , Callback_(std::move(callback))

    {
        FD_ = handle->GetFD();
        YCHECK(FD_ >= 0);

        CreateWatch();
    }

    ~TNotificationWatch()
    {
        DropWatch();
    }

    DEFINE_BYVAL_RO_PROPERTY(int, FD);
    DEFINE_BYVAL_RO_PROPERTY(int, WD);

    void Run()
    {
        Callback_.Run();
        // Reinitialize watch to hook to the newly created file.
        DropWatch();
        CreateWatch();
    }

private:
    void CreateWatch()
    {
        YCHECK(WD_ <= 0);
#ifdef _linux_
        WD_ = inotify_add_watch(
            FD_,
            Path_.c_str(),
            IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);

        if (WD_ < 0) {
            LOG_ERROR(TError::FromSystem(errno), "Error registering watch for %v",
                Path_);
            WD_ = -1;
        } else if (WD_ > 0) {
            LOG_TRACE("Registered watch %v for %v",
                WD_,
                Path_);
        } else {
            Y_UNREACHABLE();
        }
#else
        WD_ = -1;
#endif
    }

    void DropWatch()
    {
#ifdef _linux_
        if (WD_ > 0) {
            LOG_TRACE("Unregistering watch %v for %v",
                WD_,
                Path_);
            inotify_rm_watch(FD_, WD_);
        }
#endif
        WD_ = -1;
    }

private:
    Stroka Path_;
    TClosure Callback_;

};

////////////////////////////////////////////////////////////////////////////////

namespace {

void ReloadSignalHandler(int signal)
{
    NLogging::TLogManager::Get()->Reopen();
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TLogManager::TImpl
    : public TRefCounted
{
public:
    TImpl()
        : EventQueue_(New<TInvokerQueue>(
            EventCount_,
            NProfiling::EmptyTagIds,
            false,
            false))
        , LoggingThread_(New<TThread>(this))
    {
        SystemWriters_.push_back(New<TStderrLogWriter>());
        UpdateConfig(TLogConfig::CreateDefault(), false);
    }

    void EnsureStarted()
    {
        if (LoggingThread_->IsShutdown()) {
            return;
        }

        if (LoggingThread_->IsStarted()) {
            return;
        }

        LoggingThread_->Start();
        EventQueue_->SetThreadId(LoggingThread_->GetId());

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            EventQueue_,
            BIND(&TImpl::OnProfiling, MakeStrong(this)),
            ProfilingPeriod);
        ProfilingExecutor_->Start();

        DequeueExecutor_ = New<TPeriodicExecutor>(
            EventQueue_,
            BIND(&TImpl::OnDequeue, MakeStrong(this)),
            DequeuePeriod);
        DequeueExecutor_->Start();
    }

    void Configure(INodePtr node, const TYPath& path = "")
    {
        if (LoggingThread_->IsShutdown()) {
            return;
        }

        auto config = TLogConfig::CreateFromNode(node, path);
        LoggerQueue_.Enqueue(std::move(config));
    }

    void Configure(const Stroka& fileName, const TYPath& path)
    {
        try {
            TIFStream configStream(fileName);
            auto root = ConvertToNode(&configStream);
            auto configNode = GetNodeByYPath(root, path);
            Configure(configNode, path);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error while configuring logging");
        }
    }

    void Configure(TLogConfigPtr&& config)
    {
        if (LoggingThread_->IsShutdown()) {
            return;
        }

        LoggerQueue_.Enqueue(std::move(config));
    }

    void Shutdown()
    {
        if (LoggingThread_->IsStarted() && LoggingThread_->GetId() != ::TThread::CurrentThreadId()) {
            // Waiting for output of all previous messages.
            // Waiting no more than 1 second to prevent hanging.
            auto now = TInstant::Now();
            auto enqueuedEvents = EnqueuedEvents_.load();
            while (enqueuedEvents > WrittenEvents_.load() &&
                TInstant::Now() - now < Config_->ShutdownGraceTimeout)
            {
                SchedYield();
            }
        }

        EventQueue_->Shutdown();
        LoggingThread_->Shutdown();
        FlushWriters();
    }

    /*!
     * In some cases (when configuration is being updated at the same time),
     * the actual version is greater than the version returned by this method.
     */
    int GetVersion() const
    {
        return Version_;
    }

    ELogLevel GetMinLevel(const Stroka& category) const
    {
        TGuard<TForkAwareSpinLock> guard(SpinLock_);

        ELogLevel level = ELogLevel::Maximum;
        for (const auto& rule : Config_->Rules) {
            if (rule->IsApplicable(category)) {
                level = Min(level, rule->MinLevel);
            }
        }
        return level;
    }

    void Enqueue(TLogEvent&& event)
    {
        if (event.Level == ELogLevel::Fatal) {
            bool shutdown = false;
            if (!ShutdownRequested_.compare_exchange_strong(shutdown, true)) {
                // Fatal events should not get out of this call.
                Sleep(TDuration::Max());
            }

            // Add fatal message to log and notify event log queue.
            PushLogEvent(std::move(event));

            // Flush everything and die.
            Shutdown();

            // Last-minute information.
            TRawFormatter<1024> formatter;
            formatter.AppendString("\n*** Fatal error encountered in ");
            formatter.AppendString(event.Function);
            formatter.AppendString(" (");
            formatter.AppendString(event.FileName);
            formatter.AppendString(":");
            formatter.AppendNumber(event.Line);
            formatter.AppendString(") ***\n");
            formatter.AppendString(event.Message.c_str());
            formatter.AppendString("\n*** Aborting ***\n");

            HandleEintr(::write, 2, formatter.GetData(), formatter.GetBytesWritten());

            std::terminate();
        }

        if (ShutdownRequested_) {
            return;
        }

        if (LoggingThread_->IsShutdown()) {
            return;
        }

        EnsureStarted();

        // Order matters here; inherent race may lead to negative backlog and integer overflow.
        auto writtenEvents = WrittenEvents_.load();
        auto enqueuedEvents = EnqueuedEvents_.load();
        auto backlogEvents = enqueuedEvents - writtenEvents;

        // NB: This is somewhat racy but should work fine as long as more messages keep coming.
        if (Suspended_) {
            if (backlogEvents < LowBacklogWatermark_) {
                Suspended_ = false;
                LOG_INFO("Backlog size has dropped below low watermark %v, logging resumed",
                    LowBacklogWatermark_);
            }
        } else {
            if (backlogEvents >= HighBacklogWatermark_) {
                Suspended_ = true;
                LOG_WARNING("Backlog size has exceeded high watermark %v, logging suspended",
                    HighBacklogWatermark_);
            }
        }

        // NB: Always allow system messages to pass through.
        if (Suspended_ && event.Category != SystemLoggingCategory) {
            return;
        }

        PushLogEvent(std::move(event));
    }

    void Reopen()
    {
        ReopenRequested_ = true;
    }

private:
    using TLoggerQueueItem = TVariant<TLogEvent, TLogConfigPtr>;

    class TThread
        : public TSchedulerThread
    {
    public:
        explicit TThread(TImpl* owner)
            : TSchedulerThread(
                owner->EventCount_,
                "Logging",
                NProfiling::EmptyTagIds,
                false,
                false)
            , Owner_(owner)
        { }

    private:
        TImpl* const Owner_;

        virtual void OnThreadStart() override
        {
#ifdef _unix_
            // Set mask.
            sigset_t ss;
            sigemptyset(&ss);
            sigaddset(&ss, SIGHUP);
            sigprocmask(SIG_UNBLOCK, &ss, nullptr);

            // Set handler.
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sigemptyset(&sa.sa_mask);
            sa.sa_handler = &ReloadSignalHandler;

            YCHECK(sigaction(SIGHUP, &sa, nullptr) == 0);
#endif
        }

        virtual EBeginExecuteResult BeginExecute() override
        {
            return Owner_->BeginExecute();
        }

        virtual void EndExecute() override
        {
            Owner_->EndExecute();
        }
    };

    EBeginExecuteResult BeginExecute()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);
        return EventQueue_->BeginExecute(&CurrentAction_);
    }

    void EndExecute()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        EventQueue_->EndExecute(&CurrentAction_);
    }


    const std::vector<ILogWriterPtr>& GetWriters(const TLogEvent& event)
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        if (event.Category == SystemLoggingCategory) {
            return SystemWriters_;
        }

        std::pair<Stroka, ELogLevel> cacheKey(event.Category, event.Level);
        auto it = CachedWriters_.find(cacheKey);
        if (it != CachedWriters_.end()) {
            return it->second;
        }

        yhash_set<Stroka> writerIds;
        for (const auto& rule : Config_->Rules) {
            if (rule->IsApplicable(event.Category, event.Level)) {
                writerIds.insert(rule->Writers.begin(), rule->Writers.end());
            }
        }

        std::vector<ILogWriterPtr> writers;
        for (const auto& writerId : writerIds) {
            auto writerIt = Writers_.find(writerId);
            YCHECK(writerIt != Writers_.end());
            writers.push_back(writerIt->second);
        }

        auto pair = CachedWriters_.insert(std::make_pair(cacheKey, writers));
        YCHECK(pair.second);

        return pair.first->second;
    }

    void Write(const TLogEvent& event)
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        for (const auto& writer : GetWriters(event)) {
            writer->Write(event);
        }
    }

    std::unique_ptr<TNotificationWatch> CreateNoficiationWatch(ILogWriterPtr writer, const Stroka& fileName)
    {
#ifdef _linux_
        if (Config_->WatchPeriod) {
            if (!NotificationHandle_) {
                NotificationHandle_.reset(new TNotificationHandle());
            }
            return std::unique_ptr<TNotificationWatch>(
                new TNotificationWatch(
                    NotificationHandle_.get(),
                    fileName.c_str(),
                    BIND(&ILogWriter::Reload, writer)));
        }
#endif
        return nullptr;
    }

    void UpdateConfig(const TLogConfigPtr& config, bool verifyThreadAffinity = true)
    {
        if (verifyThreadAffinity) {
            VERIFY_THREAD_AFFINITY(LoggingThread);
        }

        if (ShutdownRequested_) {
            return;
        }

        if (LoggingThread_->IsShutdown()) {
            return;
        }

        EnsureStarted();

        FlushWriters();

        {
            decltype(Writers_) writers;
            decltype(CachedWriters_) cachedWriters;

            TGuard<TForkAwareSpinLock> guard(SpinLock_);
            Writers_.swap(writers);
            CachedWriters_.swap(cachedWriters);
            Config_ = config;
            HighBacklogWatermark_ = Config_->HighBacklogWatermark;
            LowBacklogWatermark_ = Config_->LowBacklogWatermark;

            guard.Release();

            // writers and cachedWriter will die here where we don't
            // hold the spinlock anymore.
        }

        for (const auto& pair : Config_->WriterConfigs) {
            const auto& name = pair.first;
            const auto& config = pair.second;

            ILogWriterPtr writer;
            std::unique_ptr<TNotificationWatch> watch;

            switch (config->Type) {
                case EWriterType::Stdout:
                    writer = New<TStdoutLogWriter>();
                    break;
                case EWriterType::Stderr:
                    writer = New<TStderrLogWriter>();
                    break;
                case EWriterType::File:
                    writer = New<TFileLogWriter>(config->FileName);
                    watch = CreateNoficiationWatch(writer, config->FileName);
                    break;
                default:
                    Y_UNREACHABLE();
            }

            YCHECK(Writers_.insert(std::make_pair(name, std::move(writer))).second);

            if (watch) {
                if (watch->GetWD() >= 0) {
                    // Watch can fail to initialize if the writer is disabled
                    // e.g. due to the lack of space.
                    YCHECK(NotificationWatchesIndex_.insert(
                        std::make_pair(watch->GetWD(), watch.get())).second);
                }
                NotificationWatches_.emplace_back(std::move(watch));
            }
        }

        Version_++;

        if (FlushExecutor_) {
            FlushExecutor_->Stop();
            FlushExecutor_.Reset();
        }

        if (WatchExecutor_) {
            WatchExecutor_->Stop();
            WatchExecutor_.Reset();
        }

        auto flushPeriod = Config_->FlushPeriod;
        if (flushPeriod) {
            FlushExecutor_ = New<TPeriodicExecutor>(
                EventQueue_,
                BIND(&TImpl::FlushWriters, MakeStrong(this)),
                *flushPeriod);
            FlushExecutor_->Start();
        }

        auto watchPeriod = Config_->WatchPeriod;
        if (watchPeriod) {
            WatchExecutor_ = New<TPeriodicExecutor>(
                EventQueue_,
                BIND(&TImpl::WatchWriters, MakeStrong(this)),
                *watchPeriod);
            WatchExecutor_->Start();
        }

        auto checkSpacePeriod = Config_->CheckSpacePeriod;
        if (checkSpacePeriod) {
            CheckSpaceExecutor_ = New<TPeriodicExecutor>(
                EventQueue_,
                BIND(&TImpl::CheckSpace, MakeStrong(this)),
                *checkSpacePeriod);
            CheckSpaceExecutor_->Start();
        }
    }

    void FlushWriters()
    {
        for (auto& pair : Writers_) {
            pair.second->Flush();
        }
    }

    void ReloadWriters()
    {
        Version_++;
        for (auto& pair : Writers_) {
            pair.second->Reload();
        }
    }

    void CheckSpace()
    {
        for (auto& pair : Writers_) {
            pair.second->CheckSpace(Config_->MinDiskSpace);
        }
    }

    void WatchWriters()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        if (!NotificationHandle_)
            return;

        int previousWD = -1, currentWD = -1;
        while ((currentWD = NotificationHandle_->Poll()) > 0) {
            if (currentWD == previousWD) {
                continue;
            }
            auto&& it = NotificationWatchesIndex_.find(currentWD);
            auto&& jt = NotificationWatchesIndex_.end();
            YCHECK(it != jt);

            auto* watch = it->second;
            watch->Run();

            if (watch->GetWD() != currentWD) {
                NotificationWatchesIndex_.erase(it);
                if (watch->GetWD() >= 0) {
                    // Watch can fail to initialize if the writer is disabled
                    // e.g. due to the lack of space.
                    YCHECK(NotificationWatchesIndex_.insert(
                        std::make_pair(watch->GetWD(), watch)).second);
                }
            }

            previousWD = currentWD;
        }
    }

    void PushLogEvent(TLogEvent&& event)
    {
        ++EnqueuedEvents_;
        LoggerQueue_.Enqueue(std::move(event));
    }

    void OnProfiling()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        auto writtenEvents = WrittenEvents_.load();
        auto enqueuedEvents = EnqueuedEvents_.load();

        Profiler.Enqueue("/enqueued_events", enqueuedEvents, EMetricType::Counter);
        Profiler.Enqueue("/written_events", writtenEvents, EMetricType::Counter);
        Profiler.Enqueue("/backlog_events", enqueuedEvents - writtenEvents, EMetricType::Counter);
    }

    void OnDequeue()
    {
        VERIFY_THREAD_AFFINITY(LoggingThread);

        int eventsWritten = 0;
        while (LoggerQueue_.DequeueAll(true, [&] (const TLoggerQueueItem& item) {
            if (const auto* configPtr = item.TryAs<TLogConfigPtr>()) {
                UpdateConfig(*configPtr);
            } else if (const auto* eventPtr = item.TryAs<TLogEvent>()) {
                if (ReopenRequested_) {
                    ReopenRequested_ = false;
                    ReloadWriters();
                }

                Write(*eventPtr);
                ++eventsWritten;
            } else {
                Y_UNREACHABLE();
            }
        }))
        { }

        if (eventsWritten > 0 && !Config_->FlushPeriod) {
            FlushWriters();
        }

        WrittenEvents_ += eventsWritten;
    }


    const std::shared_ptr<TEventCount> EventCount_ = std::make_shared<TEventCount>();
    const TInvokerQueuePtr EventQueue_;

    TIntrusivePtr<TThread> LoggingThread_;
    DECLARE_THREAD_AFFINITY_SLOT(LoggingThread);

    TEnqueuedAction CurrentAction_;

    // Configuration.
    TForkAwareSpinLock SpinLock_;
    // Version forces this very module's Logger object to update to our own
    // default configuration (default level etc.).
    std::atomic<int> Version_ = {-1};
    TLogConfigPtr Config_;

    // These are just copies from _Config.
    // The values are being read from arbitrary threads but stale values are fine.
    int HighBacklogWatermark_ = -1;
    int LowBacklogWatermark_ = -1;

    bool Suspended_ = false;

    TMultipleProducerSingleConsumerLockFreeStack<TLoggerQueueItem> LoggerQueue_;

    std::atomic<ui64> EnqueuedEvents_ = {0};
    std::atomic<ui64> WrittenEvents_ = {0};

    yhash_map<Stroka, ILogWriterPtr> Writers_;
    yhash_map<std::pair<Stroka, ELogLevel>, std::vector<ILogWriterPtr>> CachedWriters_;
    std::vector<ILogWriterPtr> SystemWriters_;

    volatile bool ReopenRequested_ = false;
    std::atomic<bool> ShutdownRequested_ = {false};

    TPeriodicExecutorPtr FlushExecutor_;
    TPeriodicExecutorPtr WatchExecutor_;
    TPeriodicExecutorPtr CheckSpaceExecutor_;
    TPeriodicExecutorPtr ProfilingExecutor_;
    TPeriodicExecutorPtr DequeueExecutor_;

    std::unique_ptr<TNotificationHandle> NotificationHandle_;
    std::vector<std::unique_ptr<TNotificationWatch>> NotificationWatches_;
    yhash_map<int, TNotificationWatch*> NotificationWatchesIndex_;
};

////////////////////////////////////////////////////////////////////////////////

TLogManager::TLogManager()
    : Impl_(new TImpl())
{ }

TLogManager::~TLogManager()
{ }

TLogManager* TLogManager::Get()
{
    return Singleton<TLogManager>();
}

void TLogManager::StaticShutdown()
{
    Get()->Shutdown();
}

void TLogManager::Configure(INodePtr node)
{
    Impl_->Configure(node);
}

void TLogManager::Configure(const Stroka& fileName, const TYPath& path)
{
    Impl_->Configure(fileName, path);
}

void TLogManager::Configure(TLogConfigPtr&& config)
{
    Impl_->Configure(std::move(config));
}

void TLogManager::Shutdown()
{
    Impl_->Shutdown();
}

int TLogManager::GetVersion() const
{
    return Impl_->GetVersion();
}

ELogLevel TLogManager::GetMinLevel(const Stroka& category) const
{
    return Impl_->GetMinLevel(category);
}

void TLogManager::Enqueue(TLogEvent&& event)
{
    Impl_->Enqueue(std::move(event));
}

void TLogManager::Reopen()
{
    Impl_->Reopen();
}

////////////////////////////////////////////////////////////////////////////////

void SimpleConfigureLogging(
    const char* logLevelStr,
    const char* logExcludeCategoriesStr,
    const char* logIncludeCategoriesStr)
{
    if (!logLevelStr && !logExcludeCategoriesStr && !logIncludeCategoriesStr) {
        return;
    }

    const char* const stderrWriterName = "stderr";

    auto rule = New<TRuleConfig>();
    rule->Writers.push_back(stderrWriterName);

    if (logLevelStr) {
        Stroka logLevel = logLevelStr;
        logLevel.to_upper(0, std::min(logLevel.size(), static_cast<size_t>(1)));
        rule->MinLevel = TEnumTraits<ELogLevel>::FromString(logLevel);
    } else {
        rule->MinLevel = ELogLevel::Fatal;
    }

    std::vector<Stroka> logExcludeCategories;
    if (logExcludeCategoriesStr) {
        logExcludeCategories = splitStroku(logExcludeCategoriesStr, ",");
    }

    for (const auto& excludeCategory : logExcludeCategories) {
        rule->ExcludeCategories.insert(excludeCategory);
    }

    std::vector<Stroka> logIncludeCategories;
    if (logIncludeCategoriesStr) {
        logIncludeCategories = splitStroku(logIncludeCategoriesStr, ",");
    }

    if (!logIncludeCategories.empty()) {
        rule->IncludeCategories.Assign(yhash_set<Stroka>());
        for (const auto& includeCategory : logIncludeCategories) {
            rule->IncludeCategories->insert(includeCategory);
        }
    }

    auto config = New<TLogConfig>();
    config->Rules.push_back(std::move(rule));

    config->MinDiskSpace = 0;
    config->HighBacklogWatermark = std::numeric_limits<int>::max();
    config->LowBacklogWatermark = 0;

    auto stderrWriter = New<TWriterConfig>();
    stderrWriter->Type = EWriterType::Stderr;

    config->WriterConfigs.insert(std::make_pair(stderrWriterName, std::move(stderrWriter)));

    TLogManager::Get()->Configure(std::move(config));
}

void SimpleConfigureLoggingFromEnv()
{
    SimpleConfigureLogging(
        getenv("YT_LOG_LEVEL"),
        getenv("YT_LOG_EXCLUDE_CATEGORIES"),
        getenv("YT_LOG_INCLUDE_CATEGORIES"));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT
