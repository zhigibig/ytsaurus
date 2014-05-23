#include "stdafx.h"
#include "changelog.h"
#include "file_changelog.h"
#include "sync_file_changelog.h"
#include "config.h"
#include "private.h"

#include <core/misc/fs.h>
#include <core/misc/cache.h>

#include <core/concurrency/thread_affinity.h>

#include <core/logging/tagged_logger.h>

#include <util/system/thread.h>

#include <util/folder/dirut.h>

#include <util/generic/singleton.h>

#include <atomic>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = HydraLogger;
static auto& Profiler = HydraProfiler;

static const TDuration FlushThreadQuantum = TDuration::MilliSeconds(10);

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogDispatcher::TChangelogQueue
    : public TRefCounted
{
public:
    explicit TChangelogQueue(TSyncFileChangelogPtr changelog)
        : Changelog_(changelog)
        , FlushedRecordCount_(changelog->GetRecordCount())
    { }


    void Lock()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        ++UseCount_;
    }

    void Unlock()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        --UseCount_;
    }


    TFuture<void> Append(TSharedRef data)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TGuard<TSpinLock> guard(SpinLock_);
        YCHECK(!SealForced_);
        AppendQueue_.push_back(std::move(data));
        ByteSize_ += data.Size();

        YCHECK(FlushPromise_);
        return FlushPromise_;
    }


    TFuture<void> AsyncFlush()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TGuard<TSpinLock> guard(SpinLock_);

        if (FlushQueue_.empty() && AppendQueue_.empty()) {
            return MakeFuture();
        }

        FlushForced_ = true;
        return FlushPromise_;
    }

    TFuture<void> AsyncSeal(int recordCount)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        {
            TGuard<TSpinLock> guard(SpinLock_);
            SealForced_ = true;
            SealRecordCount_ = recordCount;
        }

        return SealPromise_;
    }


    bool HasPendingActions()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        // Unguarded access seems OK.
        auto config = Changelog_->GetConfig();
        if (ByteSize_ >= config->FlushBufferSize) {
            return true;
        }

        if (Changelog_->GetLastFlushed() < TInstant::Now() - config->FlushPeriod) {
            return true;
        }

        if (FlushForced_) {
            return true;
        }

        if (SealForced_) {
            return true;
        }

        return false;
    }

    void RunPendingActions()
    {
        VERIFY_THREAD_AFFINITY(SyncThread);

        SyncFlush();
        SyncSeal();
    }

    bool TrySweep()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TPromise<void> promise;
        {
            TGuard<TSpinLock> guard(SpinLock_);

            if (!AppendQueue_.empty() || !FlushQueue_.empty()) {
                return false;
            }

            if (SealForced_ && !SealPromise_.IsSet()) {
                return false;
            }

            if (UseCount_.load() > 0) {
                return false;
            }

            promise = FlushPromise_;
            FlushPromise_.Reset();
            FlushForced_ = false;
        }

        promise.Set();

        return true;
    }
    

    std::vector<TSharedRef> Read(int firstRecordId, int maxRecords, i64 maxBytes)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        std::vector<TSharedRef> records;
        int currentRecordId = firstRecordId;
        int needRecords = maxRecords;
        i64 needBytes = maxBytes;
        i64 readBytes = 0;

        auto appendRecord = [&] (const TSharedRef& record) {
            records.push_back(record);
            --needRecords;
            ++currentRecordId;
            needBytes -= record.Size();
            readBytes += record.Size();
        };

        auto needMore = [&] () {
            return needRecords > 0 && needBytes > 0;
        };

        while (needMore()) {
            TGuard<TSpinLock> guard(SpinLock_);
            if (currentRecordId < FlushedRecordCount_) {
                // Read from disk, w/o spinlock.
                guard.Release();

                PROFILE_TIMING ("/changelog_read_io_time") {
                    auto diskRecords = Changelog_->Read(currentRecordId, needRecords, needBytes);
                    for (const auto& record : diskRecords) {
                        appendRecord(record);
                    }
                }
            } else {
                // Read from memory, w/ spinlock.

                auto readFromMemory = [&] (const std::vector<TSharedRef>& memoryRecords, int firstMemoryRecordId) {
                    if (!needMore())
                        return;
                    int memoryIndex = currentRecordId - firstMemoryRecordId;
                    YCHECK(memoryIndex >= 0);
                    while (memoryIndex < static_cast<int>(memoryRecords.size()) && needMore()) {
                        appendRecord(memoryRecords[memoryIndex++]);
                    }
                };

                PROFILE_TIMING ("/changelog_read_copy_time") {
                    readFromMemory(FlushQueue_, FlushedRecordCount_);
                    readFromMemory(AppendQueue_, FlushedRecordCount_ + FlushQueue_.size());
                }

                // Break since we don't except more records beyond this point.
                break;
            }
        }

        Profiler.Enqueue("/changelog_read_record_count", records.size());
        Profiler.Enqueue("/changelog_read_size", readBytes);

        return records;
    }

private:
    TSyncFileChangelogPtr Changelog_;

    TSpinLock SpinLock_;
    std::atomic<int> UseCount_ = 0;

    //! Number of records flushed to the underlying sync changelog.
    int FlushedRecordCount_ = 0;
    //! These records are currently being flushed to the underlying sync changelog and
    //! immediately follow the flushed part.
    std::vector<TSharedRef> FlushQueue_;
    //! Newly appended records go here. These records immediately follow the flush part.
    std::vector<TSharedRef> AppendQueue_;

    i64 ByteSize_ = 0;

    TPromise<void> FlushPromise_ = NewPromise();
    bool FlushForced_ = false;

    TPromise<void> SealPromise_ = NewPromise();
    bool SealForced_ = false;
    int SealRecordCount_ = -1;


    DECLARE_THREAD_AFFINITY_SLOT(SyncThread);


    void SyncFlush()
    {
        TPromise<void> flushPromise;
        {
            TGuard<TSpinLock> guard(SpinLock_);

            YCHECK(FlushQueue_.empty());
            FlushQueue_.swap(AppendQueue_);
            ByteSize_ = 0;

            YCHECK(FlushPromise_);
            flushPromise = FlushPromise_;
            FlushPromise_ = NewPromise();
            FlushForced_ = false;
        }

        if (!FlushQueue_.empty()) {
            PROFILE_TIMING("/changelog_flush_io_time") {
                Changelog_->Append(FlushedRecordCount_, FlushQueue_);
                Changelog_->Flush();
            }
        }

        {
            TGuard<TSpinLock> guard(SpinLock_);
            FlushedRecordCount_ += FlushQueue_.size();
            FlushQueue_.clear();
        }

        flushPromise.Set();
    }

    void SyncSeal()
    {
        TPromise<void> sealPromise;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (!SealForced_)
                return;
            sealPromise = SealPromise_;
            SealForced_ = false;
        }

        while (true) {
            {
                TGuard<TSpinLock> guard(SpinLock_);
                if (AppendQueue_.empty())
                    break;
            }
            SyncFlush();
        }

        PROFILE_TIMING("/changelog_seal_io_time") {
            Changelog_->Seal(SealRecordCount_);
        }

        sealPromise.Set();
    }

};

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogDispatcher::TImpl
    : public TRefCounted
{
public:
    explicit TImpl(const Stroka& threadName)
        : ThreadName_(threadName)
        , Thread_(ThreadFunc, static_cast<void*>(this))
        , WakeupEvent(Event::rManual)
        , Started_(NewPromise())
        // XXX(babenko): VS2013 Nov CTP does not have a proper ctor :(
        // , Finished_(false)
        , RecordCounter_("/record_rate")
        , SizeCounter_("/record_throughput")
    {
        Finished_ = false;
        Thread_.Start();
        Started_.Get();
    }

    ~TImpl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        Finished_ = true;
        WakeupEvent.Signal();
        Thread_.Join();
    }


    TFuture<void> Append(
        TSyncFileChangelogPtr changelog,
        const TSharedRef& record)
    {
        auto queue = GetQueueAndLock(changelog);
        auto result = queue->Append(record);
        queue->Unlock();
        WakeupEvent.Signal();

        Profiler.Increment(RecordCounter_);
        Profiler.Increment(SizeCounter_, record.Size());

        return result;
    }

    std::vector<TSharedRef> Read(
        TSyncFileChangelogPtr changelog,
        int recordId,
        int maxRecords,
        i64 maxBytes)
    {
        YCHECK(recordId >= 0);
        YCHECK(maxRecords >= 0);

        if (maxRecords == 0) {
            return std::vector<TSharedRef>();
        }

        auto queue = FindQueueAndLock(changelog);
        if (queue) {
            auto records = queue->Read(recordId, maxRecords, maxBytes);
            queue->Unlock();
            return std::move(records);
        } else {
            PROFILE_TIMING ("/changelog_read_io_time") {
                return changelog->Read(recordId, maxRecords, maxBytes);
            }
        }
    }

    TFuture<void> Flush(TSyncFileChangelogPtr changelog)
    {
        auto queue = FindQueue(changelog);
        return queue ? queue->AsyncFlush() : MakeFuture();
    }

    void Close(TSyncFileChangelogPtr changelog)
    {
        RemoveQueue(changelog);
        changelog->Close();
    }

    TFuture<void> Seal(TSyncFileChangelogPtr changelog, int recordCount)
    {
        auto queue = GetQueueAndLock(changelog);
        auto result = queue->AsyncSeal(recordCount);
        queue->Unlock();
        WakeupEvent.Signal();

        return result;
    }

    void Remove(TSyncFileChangelogPtr changelog)
    {
        RemoveQueue(changelog);

        auto path = changelog->GetFileName();

        changelog->Close();

        NFS::Remove(path);
        NFS::Remove(path + IndexSuffix);
    }

private:
    Stroka ThreadName_;

    TSpinLock SpinLock_;
    yhash_map<TSyncFileChangelogPtr, TChangelogQueuePtr> QueueMap_;

    TThread Thread_;
    Event WakeupEvent;
    TPromise<void> Started_;
    std::atomic<bool> Finished_;

    NProfiling::TRateCounter RecordCounter_;
    NProfiling::TRateCounter SizeCounter_;


    TChangelogQueuePtr FindQueue(TSyncFileChangelogPtr changelog) const
    {
        TGuard<TSpinLock> guard(SpinLock_);
        auto it = QueueMap_.find(changelog);
        return it == QueueMap_.end() ? nullptr : it->second;
    }

    TChangelogQueuePtr FindQueueAndLock(TSyncFileChangelogPtr changelog) const
    {
        TGuard<TSpinLock> guard(SpinLock_);
        auto it = QueueMap_.find(changelog);
        if (it == QueueMap_.end()) {
            return nullptr;
        }

        auto queue = it->second;
        queue->Lock();
        return queue;
    }

    TChangelogQueuePtr GetQueueAndLock(TSyncFileChangelogPtr changelog)
    {
        TGuard<TSpinLock> guard(SpinLock_);
        TChangelogQueuePtr queue;

        auto it = QueueMap_.find(changelog);
        if (it != QueueMap_.end()) {
            queue = it->second;
        } else {
            queue = New<TChangelogQueue>(changelog);
            YCHECK(QueueMap_.insert(std::make_pair(changelog, queue)).second);
        }

        queue->Lock();
        return queue;
    }

    void RemoveQueue(TSyncFileChangelogPtr changelog)
    {
        TGuard<TSpinLock> guard(SpinLock_);
        QueueMap_.erase(changelog);
    }

    void FlushQueues()
    {
        // Take a snapshot.
        std::vector<TChangelogQueuePtr> queues;
        {
            TGuard<TSpinLock> guard(SpinLock_);
            for (const auto& pair : QueueMap_) {
                const auto& queue = pair.second;
                if (queue->HasPendingActions()) {
                    queues.push_back(queue);
                }
            }
        }

        // Flush and seal the changelogs.
        for (auto queue : queues) {
            queue->RunPendingActions();
        }
    }

    void SweepQueues()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        auto it = QueueMap_.begin();
        while (it != QueueMap_.end()) {
            auto jt = it++;
            auto queue = jt->second;
            if (queue->TrySweep()) {
                QueueMap_.erase(jt);
            }
        }
    }


    void ProcessQueues()
    {
        FlushQueues();
        SweepQueues();
    }


    static void* ThreadFunc(void* param)
    {
        auto* this_ = (TImpl*) param;
        this_->ThreadMain();
        return nullptr;
    }

    void ThreadMain()
    {
        NConcurrency::SetCurrentThreadName(~ThreadName_);
        Started_.Set();

        while (!Finished_) {
            ProcessQueues();
            WakeupEvent.Reset();
            WakeupEvent.WaitT(FlushThreadQuantum);
        }
    }


};

////////////////////////////////////////////////////////////////////////////////

class TFileChangelog
    : public IChangelog
{
public:
    TFileChangelog(
        TFileChangelogDispatcherPtr dispatcher,
        TFileChangelogConfigPtr config,
        TSyncFileChangelogPtr changelog)
        : DispatcherImpl_(dispatcher->Impl_)
        , Config_(config)
        , SyncChangelog_(changelog)
        , RecordCount_(changelog->GetRecordCount())
        , DataSize_(changelog->GetDataSize())
    { }

    virtual int GetRecordCount() const override
    {
        return RecordCount_;
    }

    virtual i64 GetDataSize() const override
    {
        return DataSize_;
    }

    virtual TSharedRef GetMeta() const override
    {
        return SyncChangelog_->GetMeta();
    }

    virtual bool IsSealed() const override
    {
        return SyncChangelog_->IsSealed();
    }

    virtual TFuture<void> Append(const TSharedRef& data) override
    {
        RecordCount_ += 1;
        DataSize_ += data.Size();

        return DispatcherImpl_->Append(SyncChangelog_, data);
    }

    virtual TFuture<void> Flush() override
    {
        return DispatcherImpl_->Flush(SyncChangelog_);
    }

    virtual void Close() override
    {
        return DispatcherImpl_->Close(SyncChangelog_);
    }

    virtual std::vector<TSharedRef> Read(
        int firstRecordId,
        int maxRecords,
        i64 maxBytes) const override
    {
        return DispatcherImpl_->Read(
            SyncChangelog_,
            firstRecordId,
            maxRecords,
            maxBytes);
    }

    virtual TFuture<void> Seal(int recordCount) override
    {
        YCHECK(recordCount <= RecordCount_);
        RecordCount_.store(recordCount);

        return DispatcherImpl_->Seal(SyncChangelog_, recordCount);
    }

    virtual void Unseal() override
    {
        SyncChangelog_->Unseal();
    }

    void Remove()
    {
        DispatcherImpl_->Remove(SyncChangelog_);
    }

private:
    TFileChangelogDispatcher::TImplPtr DispatcherImpl_;
    TFileChangelogConfigPtr Config_;
    TSyncFileChangelogPtr SyncChangelog_;

    std::atomic<int> RecordCount_;
    std::atomic<i64> DataSize_;

};

DEFINE_REFCOUNTED_TYPE(TFileChangelog)

////////////////////////////////////////////////////////////////////////////////

TFileChangelogDispatcher::TFileChangelogDispatcher(const Stroka& threadName)
    : Impl_(New<TImpl>(threadName))
{ }

IChangelogPtr TFileChangelogDispatcher::CreateChangelog(
    const Stroka& path,
    const TSharedRef& meta,
    TFileChangelogConfigPtr config)
{
    auto syncChangelog = New<TSyncFileChangelog>(path, config);
    syncChangelog->Create(meta);

    return New<TFileChangelog>(this, config, syncChangelog);
}

IChangelogPtr TFileChangelogDispatcher::OpenChangelog(
    const Stroka& path,
    TFileChangelogConfigPtr config)
{
    auto syncChangelog = New<TSyncFileChangelog>(path, config);
    syncChangelog->Open();

    return New<TFileChangelog>(this, config, syncChangelog);
}

void TFileChangelogDispatcher::RemoveChangelog(IChangelogPtr changelog)
{
    auto* fileChangelog = dynamic_cast<TFileChangelog*>(changelog.Get());
    YCHECK(fileChangelog);
    fileChangelog->Remove();
}

////////////////////////////////////////////////////////////////////////////////

class TCachedFileChangelog
    : public TCacheValueBase<int, TCachedFileChangelog>
    , public TFileChangelog
{
public:
    explicit TCachedFileChangelog(
        TFileChangelogDispatcherPtr dispather,
        TFileChangelogConfigPtr config,
        TSyncFileChangelogPtr changelog,
        int id)
        : TCacheValueBase(id)
        , TFileChangelog(
            dispather,
            config,
            changelog)
    { }

};

class TFileChangelogStore
    : public TSizeLimitedCache<int, TCachedFileChangelog>
    , public IChangelogStore
{
public:
    TFileChangelogStore(
        const Stroka& threadName,
        const TCellGuid& cellGuid,
        TFileChangelogStoreConfigPtr config)
        : TSizeLimitedCache(config->MaxCachedChangelogs)
        , Dispatcher_(New<TFileChangelogDispatcher>(threadName))
        , CellGuid_(cellGuid)
        , Config_(config)
        , Logger(HydraLogger)
    {
        Logger.AddTag(Sprintf("Path: %s", ~Config_->Path));
    }

    void Start()
    {
        LOG_DEBUG("Preparing changelog store");

        NFS::ForcePath(Config_->Path);
        NFS::CleanTempFiles(Config_->Path);
    }

    virtual const TCellGuid& GetCellGuid() const override
    {
        return CellGuid_;
    }

    virtual IChangelogPtr CreateChangelog(
        int id,
        const TSharedRef& meta) override
    {
        TInsertCookie cookie(id);
        if (!BeginInsert(&cookie)) {
            LOG_FATAL("Trying to create an already existing changelog %d",
                id);
        }

        auto path = GetChangelogPath(id);

        try {
            auto changelog = New<TSyncFileChangelog>(
                path,
                Config_);
            changelog->Create(meta);
            cookie.EndInsert(New<TCachedFileChangelog>(
                Dispatcher_,
                Config_,
                changelog,
                id));
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "Error creating changelog %d", id);
        }

        return cookie.GetValue().Get().Value();
    }

    virtual IChangelogPtr TryOpenChangelog(int id) override
    {
        TInsertCookie cookie(id);
        if (BeginInsert(&cookie)) {
            auto path = GetChangelogPath(id);
            if (!isexist(~path)) {
                cookie.Cancel(TError(
                    NHydra::EErrorCode::NoSuchChangelog,
                    "No such changelog %d",
                    id));
            } else {
                try {
                    auto changelog = New<TSyncFileChangelog>(
                        path,
                        Config_);
                    changelog->Open();
                    cookie.EndInsert(New<TCachedFileChangelog>(
                        Dispatcher_,
                        Config_,
                        changelog,
                        id));
                } catch (const std::exception& ex) {
                    LOG_FATAL(ex, "Error opening changelog %d", id);
                }
            }
        }

        auto changelogOrError = cookie.GetValue().Get();
        return changelogOrError.IsOK() ? changelogOrError.Value() : nullptr;
    }

    virtual int GetLatestChangelogId(int initialId) override
    {
        for (int id = initialId; ; ++id) {
            auto path = GetChangelogPath(id);
            if (!isexist(~path)) {
                return id == initialId ? NonexistingSegmentId : id - 1;
            }
        }
    }

private:
    TFileChangelogDispatcherPtr Dispatcher_;

    TCellGuid CellGuid_;
    TFileChangelogStoreConfigPtr Config_;

    NLog::TTaggedLogger Logger;


    Stroka GetChangelogPath(int id)
    {
        return NFS::CombinePaths(Config_->Path, Sprintf("%09d", id) + LogSuffix);
    }

};

IChangelogStorePtr CreateFileChangelogStore(
    const Stroka& threadName,
    const TCellGuid& cellGuid,
    TFileChangelogStoreConfigPtr config)
{
    auto store = New<TFileChangelogStore>(
        threadName,
        cellGuid,
        config);
    store->Start();
    return store;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT

