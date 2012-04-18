#include "stdafx.h"
#include "async_change_log.h"
#include "meta_version.h"
#include "change_log.h"

#include <ytlib/misc/foreach.h>
#include <ytlib/misc/singleton.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/profiling/profiler.h>

#include <util/system/thread.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("MetaState");
static NProfiling::TProfiler Profiler("/meta_state");

////////////////////////////////////////////////////////////////////////////////

class TAsyncChangeLog::TChangeLogQueue
    : public TRefCounted
{
public:
    typedef TAsyncChangeLog::TAppendResult TAppendResult;

    // Guarded by TImpl::SpinLock
    TAtomic UseCount;

    TChangeLogQueue(const TChangeLogPtr& changeLog)
        : UseCount(0)
        , ChangeLog(changeLog)
        , FlushedRecordCount(changeLog->GetRecordCount())
        , Promise(NewPromise<TAppendPromise::TValueType>())
    { }

    TAppendResult Append(i32 recordId, const TSharedRef& data)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TGuard<TSpinLock> guard(SpinLock);

        if (recordId != DoGetRecordCount()) {
            LOG_FATAL("Unexpected record id in changelog %d: expected %d, got %d",
                ChangeLog->GetId(),
                DoGetRecordCount(),
                recordId);
        }

        AppendQueue.push_back(data);

        YASSERT(!Promise.IsNull());
        return Promise;
    }

    void Flush()
    {
        VERIFY_THREAD_AFFINITY(Flush);

        TAppendPromise promise(Null);

        {
            TGuard<TSpinLock> guard(SpinLock);

            YASSERT(FlushQueue.empty());
            FlushQueue.swap(AppendQueue);

            YASSERT(!Promise.IsNull());
            promise = Promise;
            Promise = NewPromise<TAppendPromise::TValueType>();
        }

        // In addition to making this code run a tiny bit faster,
        // this check also prevents us from calling TChangeLog::Append for an already finalized changelog 
        // (its queue may still be present in the map).
        if (!FlushQueue.empty()) {
            PROFILE_TIMING ("/changelog_flush_io_time") {
                ChangeLog->Append(FlushedRecordCount, FlushQueue);
                ChangeLog->Flush();
            }
        }

        promise.Set(TVoid());

        {
            TGuard<TSpinLock> guard(SpinLock);
            FlushedRecordCount += FlushQueue.ysize();
            FlushQueue.clear();
        }
    }

    void WaitUntilFlushed()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        PROFILE_TIMING ("/changelog_flush_wait_time") {
            TAppendPromise promise(Null);
            {
                TGuard<TSpinLock> guard(SpinLock);
                if (FlushQueue.empty() && AppendQueue.empty()) {
                    return;
                }
                promise = Promise;
            }
            promise.Get();
        }
    }

    i32 GetRecordCount()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TGuard<TSpinLock> guard(SpinLock);
        return DoGetRecordCount();
    }

    bool TrySweep()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        {
            TGuard<TSpinLock> guard(SpinLock);

            if (!AppendQueue.empty() || !FlushQueue.empty()) {
                return false;
            }

            if (UseCount != 0) {
                return false;
            }
        }

        Promise.Set(TVoid());
        Promise.Reset();

        return true;
    }

    // Can return less records than recordCount
    void Read(i32 firstRecordId, i32 recordCount, yvector<TSharedRef>* result)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        i32 flushedRecordCount;

        PROFILE_TIMING ("/changelog_read_copy_time") {
            TGuard<TSpinLock> guard(SpinLock);
            flushedRecordCount = FlushedRecordCount; 

            CopyRecords(
                FlushedRecordCount,
                FlushQueue,
                firstRecordId,
                recordCount,
                result);

            CopyRecords(
                FlushedRecordCount + FlushQueue.ysize(),
                AppendQueue,
                firstRecordId,
                recordCount,
                result);
        }

        PROFILE_TIMING ("/changelog_read_io_time") {
            if (firstRecordId < flushedRecordCount) {
                yvector<TSharedRef> buffer;
                i32 neededRecordCount = Min(
                    recordCount,
                    flushedRecordCount - firstRecordId);
                ChangeLog->Read(firstRecordId, neededRecordCount, &buffer);
                YASSERT(buffer.ysize() == neededRecordCount);

                buffer.insert(
                    buffer.end(),
                    result->begin(),
                    result->end());

                result->swap(buffer);
            }
        }

        Profiler.Enqueue("/changelog_read_record_count", result->size());
    }

private:
    i32 DoGetRecordCount() const
    {
        VERIFY_SPINLOCK_AFFINITY(SpinLock);
        return FlushedRecordCount + FlushQueue.ysize() + AppendQueue.ysize();
    }

    static void CopyRecords(
        i32 firstRecordId,
        const yvector<TSharedRef>& records,
        i32 neededFirstRecordId,
        i32 neededRecordCount,
        yvector<TSharedRef>* result)
    {
        i32 size = records.ysize();
        i32 begin = neededFirstRecordId - firstRecordId;
        i32 end = neededFirstRecordId + neededRecordCount - firstRecordId;
        auto beginIt = records.begin() + Min(Max(begin, 0), size);
        auto endIt = records.begin() + Min(Max(end, 0), size);
        if (endIt != beginIt) {
            result->insert(
                result->end(),
                beginIt,
                endIt);
        }
    }
    
    TChangeLogPtr ChangeLog;
    
    TSpinLock SpinLock;
    i32 FlushedRecordCount;
    yvector<TSharedRef> AppendQueue;
    yvector<TSharedRef> FlushQueue;
    TAppendPromise Promise;

    DECLARE_THREAD_AFFINITY_SLOT(Flush);
};

////////////////////////////////////////////////////////////////////////////////

class TAsyncChangeLog::TImpl
{
public:
    typedef TAsyncChangeLog::TAppendResult TAppendResult;

    static TImpl* Get()
    {
        return Singleton<TImpl>();
    }

    TImpl()
        : Thread(ThreadFunc, static_cast<void*>(this))
        , WakeupEvent(Event::rManual)
        , Finished(false)
        , RecordCounter("/record_rate")
        , SizeCounter("/record_throughput")
    {
        Thread.Start();    
    }

    ~TImpl()
    {
        Shutdown();
    }

    TAppendResult Append(
        const TChangeLogPtr& changeLog,
        i32 recordId,
        const TSharedRef& data)
    {
        LOG_TRACE("Async changelog record is enqueued at version %s",
            ~TMetaVersion(changeLog->GetId(), recordId).ToString());

        auto queue = GetQueueAndLock(changeLog);
        auto result = queue->Append(recordId, data);
        UnlockQueue(queue);
        WakeupEvent.Signal();

        Profiler.Increment(RecordCounter);
        Profiler.Increment(SizeCounter, data.Size());

        return result;
    }

    void Read(
        const TChangeLogPtr& changeLog,
        i32 firstRecordId,
        i32 recordCount,
        yvector<TSharedRef>* result)
    {
        YASSERT(firstRecordId >= 0);
        YASSERT(recordCount >= 0);
        YASSERT(result);

        if (recordCount == 0) {
            return;
        }

        auto queue = FindQueueAndLock(changeLog);
        if (queue) {
            queue->Read(firstRecordId, recordCount, result);
            UnlockQueue(queue);
        } else {
            PROFILE_TIMING ("/changelog_read_io_time") {
                changeLog->Read(firstRecordId, recordCount, result);
            }
        }
    }

    void Flush(const TChangeLogPtr& changeLog)
    {
        auto queue = FindQueue(changeLog);
        if (queue) {
            queue->WaitUntilFlushed();
        }

        PROFILE_TIMING ("/changelog_flush_io_time") {
            changeLog->Flush();
        }
    }

    i32 GetRecordCount(const TChangeLogPtr& changeLog)
    {
        auto queue = FindQueueAndLock(changeLog);
        if (queue) {
            auto result = queue->GetRecordCount();
            UnlockQueue(queue);
            return result;
        } else {
            return changeLog->GetRecordCount();
        }
    }

    void Finalize(const TChangeLogPtr& changeLog)
    {
        Flush(changeLog);

        PROFILE_TIMING ("/changelog_finalize_time") {
            changeLog->Finalize();
        }

        LOG_DEBUG("Async changelog %d is finalized", changeLog->GetId());
    }

    void Truncate(const TChangeLogPtr& changeLog, i32 atRecordId)
    {
        // TODO: Later on this can be improved to asynchronous behavior by
        // getting rid of explicit synchronization.
        Flush(changeLog);

        PROFILE_TIMING ("/changelog_truncate_time") {
            changeLog->Truncate(atRecordId);
        }
    }

    void Shutdown()
    {
        Finished = true;
        WakeupEvent.Signal();
        Thread.Join();
    }

private:
    TChangeLogQueuePtr FindQueue(const TChangeLogPtr& changeLog) const
    {
        TGuard<TSpinLock> guard(SpinLock);
        auto it = ChangeLogQueues.find(changeLog);
        return it == ChangeLogQueues.end() ? NULL : it->second;
    }

    TChangeLogQueuePtr FindQueueAndLock(const TChangeLogPtr& changeLog) const
    {
        TGuard<TSpinLock> guard(SpinLock);
        auto it = ChangeLogQueues.find(changeLog);
        if (it == ChangeLogQueues.end()) {
            return NULL;
        }
        auto& queue = it->second;
        AtomicIncrement(queue->UseCount);
        return queue;
    }

    TChangeLogQueuePtr GetQueueAndLock(const TChangeLogPtr& changeLog)
    {
        TGuard<TSpinLock> guard(SpinLock);
        TChangeLogQueuePtr queue;

        auto it = ChangeLogQueues.find(changeLog);
        if (it != ChangeLogQueues.end()) {
            queue = it->second;
        } else {
            queue = New<TChangeLogQueue>(changeLog);
            YVERIFY(ChangeLogQueues.insert(MakePair(changeLog, queue)).second);
        }

        AtomicIncrement(queue->UseCount);
        return queue;
    }

    void UnlockQueue(const TChangeLogQueuePtr& queue)
    {
        AtomicDecrement(queue->UseCount);
    }

    void FlushQueues()
    {
        // Take a snapshot.
        std::vector<TChangeLogQueuePtr> queues;
        {
            TGuard<TSpinLock> guard(SpinLock);
            FOREACH (const auto& it, ChangeLogQueues) {
                queues.push_back(it.second);
            }
        }

        // Flush the queues.
        FOREACH (auto& queue, queues) {
            queue->Flush();
        }
    }

    // Returns True if there is any unswept queue left in the map.
    bool CleanQueues()
    {
        TGuard<TSpinLock> guard(SpinLock);
        
        auto it = ChangeLogQueues.begin();
        while (it != ChangeLogQueues.end()) {
            auto jt = it++;
            auto queue = jt->second;
            if (queue->TrySweep()) {
                LOG_DEBUG("Async changelog queue %d was swept", jt->first->GetId());
                ChangeLogQueues.erase(jt);
            }
        }

        return !ChangeLogQueues.empty();
    }

    // Returns True if there is any unswept queue left in the map.
    bool FlushAndClean()
    {
        FlushQueues();
        return CleanQueues();
    }

    static void* ThreadFunc(void* param)
    {
        auto* impl = (TImpl*) param;
        impl->ThreadMain();
        return NULL;
    }
    
    void ThreadMain()
    {
        NThread::SetCurrentThreadName("AsyncChangeLog");

        while (!Finished) {
            if (FlushAndClean())
                continue;

            WakeupEvent.Reset();

            if (FlushAndClean())
                continue;

            if (!Finished) {
                WakeupEvent.Wait();
            }
        }
    }

    TSpinLock SpinLock;
    yhash_map<TChangeLogPtr, TChangeLogQueuePtr> ChangeLogQueues;

    TThread Thread;
    Event WakeupEvent;
    volatile bool Finished;

    NProfiling::TRateCounter RecordCounter;
    NProfiling::TRateCounter SizeCounter;

};

////////////////////////////////////////////////////////////////////////////////

TAsyncChangeLog::TAsyncChangeLog(const TChangeLogPtr& changeLog)
    : ChangeLog(changeLog)
{
    YASSERT(changeLog);
}

TAsyncChangeLog::~TAsyncChangeLog()
{ }

TAsyncChangeLog::TAppendResult TAsyncChangeLog::Append(
    i32 recordId,
    const TSharedRef& data)
{
    return TImpl::Get()->Append(ChangeLog, recordId, data);
}

void TAsyncChangeLog::Finalize()
{
    TImpl::Get()->Finalize(ChangeLog);
}

void TAsyncChangeLog::Flush()
{
    TImpl::Get()->Flush(ChangeLog);
}

void TAsyncChangeLog::Read(i32 firstRecordId, i32 recordCount, yvector<TSharedRef>* result)
{
    TImpl::Get()->Read(ChangeLog, firstRecordId, recordCount, result);
}

i32 TAsyncChangeLog::GetId() const
{
    return ChangeLog->GetId();
}

i32 TAsyncChangeLog::GetRecordCount() const
{
    return TImpl::Get()->GetRecordCount(ChangeLog);
}

i32 TAsyncChangeLog::GetPrevRecordCount() const
{
    return ChangeLog->GetPrevRecordCount();
}

bool TAsyncChangeLog::IsFinalized() const
{
    return ChangeLog->IsFinalized();
}

void TAsyncChangeLog::Truncate(i32 atRecordId)
{
    return TImpl::Get()->Truncate(ChangeLog, atRecordId);
}

void TAsyncChangeLog::Shutdown()
{
    TImpl::Get()->Shutdown();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT

