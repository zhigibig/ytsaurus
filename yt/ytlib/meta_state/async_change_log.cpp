#include "async_change_log.h"

#include "../actions/action_util.h"

#include <util/system/thread.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MasterLogger;

// TODO: Extract these settings to the global configuration.
static i32 UnflushedBytesThreshold = 1 << 20;
static i32 UnflushedRecordsThreshold = 100000;

////////////////////////////////////////////////////////////////////////////////

class TAsyncChangeLog::TImpl
    : public TActionQueue
{
public:
    ////////////////////////////////////////////////////////////////////////////////

    //! Queue for asynchronous appending of the changes to the changelog.
    /*!
     * Internally, this class delegates all the work to the underlying changelog
     * and eventually performs I/O synchronization hence marking changes as flushed.
     */
    class TChangeLogQueue
        : public TRefCountedBase
    {
    public:
        typedef TIntrusivePtr<TChangeLogQueue> TPtr;

        //! Constructs an empty queue around underlying changelog.
        TChangeLogQueue(TChangeLog::TPtr changeLog)
            : ChangeLog(changeLog)
            , UnflushedBytes(0)
            , UnflushedRecords(0)
        { }

        //! Lazily appends the record to the changelog.
        IAction::TPtr Append(
            i32 recordId,
            const TSharedRef& data,
            const TAppendResult::TPtr& result)
        {
            THolder<TRecord> recordHolder(new TRecord(recordId, data, result));
            TRecord* record = recordHolder.Get();

            {
                TGuard<TSpinLock> guard(SpinLock);
                Records.PushBack(recordHolder.Release());
            }

            return FromMethod(&TChangeLogQueue::DoAppend, this, record);
        }

        //! Flushes the underlying changelog.
        void Flush()
        {
            ChangeLog->Flush();

            UnflushedBytes = 0;
            UnflushedRecords = 0;

            {
                TGuard<TSpinLock> guard(SpinLock);
                // Curse you, arcadia/util!
                Records.ForEach(SweepRecord);
            }
        }

        //! Checks if the queue is empty. Note that despite the fact that this call locks
        //! the list to perform the actual check, its result is only meaningful when
        //! no additional records may be enqueued into it.
        bool IsEmpty() const
        {
            TGuard<TSpinLock> guard(SpinLock);
            return Records.Empty();
        }

        //! An unflushed record (auxiliary structure).
        struct TRecord : public TIntrusiveListItem<TRecord>
        {
            i32 Id;
            TSharedRef Data;
            TAsyncChangeLog::TAppendResult::TPtr Result;
            bool WaitingForSync;

            TRecord(
                i32 id,
                const TSharedRef& data,
                const TAsyncChangeLog::TAppendResult::TPtr& result)
                : Id(id)
                , Data(data)
                , Result(result)
                , WaitingForSync(false)
            { }
        }; // struct TRecord

        //! A list of unflushed records (auxiliary structure).
        /*!
         * \note
         * Currently, the queue is based on the linked list to prevent accidental
         * memory reallocations. It should be taken into account that this incurs
         * a memory allocation for each enqueued record. Hence in the future
         * more refined memory strategy should be used.
         */
        class TRecords : public TIntrusiveListWithAutoDelete<TRecord, TDelete>
        {
        }; // class TRecords

        //! Underlying changelog.
        TChangeLog::TPtr ChangeLog;

        //! Number of currently unflushed bytes (since the last synchronization).
        i32 UnflushedBytes;

        //! Number of currently unflushed records (since the last synchronization).
        i32 UnflushedRecords;

        //! A list of unflushed records.
        TRecords Records;

        //! Preserves the atomicity of the operations on the list.
        mutable TSpinLock SpinLock;

    private:
        //! Sweep operation performed during #Flush.
        static void SweepRecord(TRecord* record)
        {
            if (record->WaitingForSync) {
                record->Result->Set(TVoid());
                record->Unlink();

                delete record;
            }
        }

        //! Actually appends the record and flushes the queue if required.
        void DoAppend(TRecord* record)
        {
            YASSERT(record != NULL);
            YASSERT(!record->WaitingForSync);

            ChangeLog->Append(record->Id, record->Data);

            {
                TGuard<TSpinLock> guard(SpinLock);
                record->WaitingForSync = true;
            }

            UnflushedBytes += record->Data.Size();
            UnflushedRecords += 1;

            if (UnflushedBytes >= UnflushedBytesThreshold ||
                UnflushedRecords >= UnflushedRecordsThreshold)
            {
                Flush();
            }
        }

    }; // class TChangeLogQueue

    ////////////////////////////////////////////////////////////////////////////////

    void Append(
        TChangeLog::TPtr changeLog,
        i32 recordId,
        const TSharedRef& data,
        const TAppendResult::TPtr& result)
    {
        TGuard<TSpinLock> guard(SpinLock);

        TChangeLogQueueMap::iterator it = ChangeLogQueues.find(changeLog);
        TChangeLogQueue::TPtr queue;

        if (it == ChangeLogQueues.end()) {
            queue = New<TChangeLogQueue>(changeLog);
            it = ChangeLogQueues.insert(MakePair(changeLog, queue)).first;
        } else {
            queue = it->second;
        }

        Invoke(queue->Append(recordId, data, result));
    }

    TVoid Finalize(TChangeLog::TPtr changeLog)
    {
        TChangeLogQueue::TPtr queue = FindQueue(changeLog);
        if (~queue != NULL) {
            queue->Flush();
        }
        changeLog->Finalize();
        return TVoid();
    }

    TVoid Flush(TChangeLog::TPtr changeLog)
    {
        TChangeLogQueue::TPtr queue = FindQueue(changeLog);
        if (~queue != NULL) {
            queue->Flush();
        }
        changeLog->Flush();
        return TVoid();
    }

    TChangeLogQueue::TPtr FindQueue(TChangeLog::TPtr changeLog)
    {
        TGuard<TSpinLock> guard(SpinLock);
        TChangeLogQueueMap::iterator it = ChangeLogQueues.find(changeLog);
        return it != ChangeLogQueues.end() ? it->second : NULL;
    }

    virtual void OnIdle()
    {
        // Take a snapshot.
        yvector<TChangeLogQueue::TPtr> queues;
        {
            TGuard<TSpinLock> guard(SpinLock);
            
            if (ChangeLogQueues.empty()) {
                // Hash map from arcadia/util does not support iteration over
                // the empty map with iterators. It crashes with a dump assertion
                // deeply within implementation details.
                return;
            }

            for (TChangeLogQueueMap::iterator it = ChangeLogQueues.begin();
                 it != ChangeLogQueues.end();
                 ++it)
            {
                queues.push_back(it->Second());
            }
        }

        // Flush the queues in the snapshot.
        for (yvector<TChangeLogQueue::TPtr>::iterator it = queues.begin();
             it != queues.end();
             ++it)
        {
            (*it)->Flush();
        }

        // Sweep the empty queues.
        {
            // Taking this lock ensures that if a queue is found empty then it can be safely
            // erased from the map.
            TGuard<TSpinLock> guard(SpinLock);
            TChangeLogQueueMap::iterator it, jt;
            for (it = ChangeLogQueues.begin(); it != ChangeLogQueues.end(); /**/) {
                jt = it++;
                if (jt->second->IsEmpty()) {
                    ChangeLogQueues.erase(jt);
                }
            }
        }
    }

private:
    typedef yhash_map<TChangeLog::TPtr,
        TChangeLogQueue::TPtr,
        TIntrusivePtrHash<TChangeLog> > TChangeLogQueueMap;

    TChangeLogQueueMap ChangeLogQueues;
    TSpinLock SpinLock;
};

////////////////////////////////////////////////////////////////////////////////

TAsyncChangeLog::TAsyncChangeLog(TChangeLog::TPtr changeLog)
    : ChangeLog(changeLog)
    , Impl(RefCountedSingleton<TImpl>())
{ }

TAsyncChangeLog::~TAsyncChangeLog()
{ }

TAsyncChangeLog::TAppendResult::TPtr TAsyncChangeLog::Append(
    i32 recordId,
    const TSharedRef& data)
{
    TAppendResult::TPtr result = New<TAppendResult>();
    Impl->Append(ChangeLog, recordId, data, result);
    return result;
}

void TAsyncChangeLog::Finalize()
{
    FromMethod(&TImpl::Finalize, Impl, ChangeLog)
        ->AsyncVia(~Impl)
        ->Do()
        ->Get();

    LOG_INFO("Changelog %d is finalized", ChangeLog->GetId());
}

void TAsyncChangeLog::Flush()
{
    FromMethod(&TImpl::Flush, Impl, ChangeLog)
        ->AsyncVia(~Impl)
        ->Do()
        ->Get();

    LOG_INFO("Changelog %d is flushed", ChangeLog->GetId());
}

void TAsyncChangeLog::Read(i32 firstRecordId, i32 recordCount, yvector<TSharedRef>* result)
{
    YASSERT(firstRecordId >= 0);
    YASSERT(recordCount >= 0);
    YASSERT(result);

    if (recordCount == 0) {
        return;
    }

    TImpl::TChangeLogQueue::TPtr queue = Impl->FindQueue(ChangeLog);

    if (~queue == NULL) {
        ChangeLog->Read(firstRecordId, recordCount, result);
        return;
    }

    // Determine whether unflushed records intersect with requested records.
    // To achieve this we have to lock the queue in order to iterate
    // through currently flushing record.
    TGuard<TSpinLock> guard(queue->SpinLock);

    i32 lastRecordId = firstRecordId + recordCount; // Non-inclusive.
    i32 firstUnflushedRecordId = lastRecordId;

    yvector<TSharedRef> unflushedRecords;

    for (TImpl::TChangeLogQueue::TRecords::iterator it = queue->Records.Begin();
        it != queue->Records.End();
        ++it)
    {
        if (it->Id >= lastRecordId)
            break;

        if (it->Id < firstRecordId)
            continue;

        firstUnflushedRecordId = Min(firstUnflushedRecordId, it->Id);

        unflushedRecords.push_back(it->Data);
    }

    // At this moment we can release the lock.
    guard.Release();

    if (unflushedRecords.empty()) {
        ChangeLog->Read(firstRecordId, recordCount, result);
        return;
    }

    ChangeLog->Read(firstRecordId, firstUnflushedRecordId - firstRecordId, result);

    i32 firstUnreadRecordId = firstRecordId + result->ysize();

    if (firstUnreadRecordId != firstUnflushedRecordId) {
        LOG_FATAL("Gap found while reading changelog (FirstUnreadRecordId: %d, FirstUnflushedRecordId: %d)",
            firstUnreadRecordId,
            firstUnflushedRecordId);
    } else {
        result->insert(result->end(), unflushedRecords.begin(), unflushedRecords.end());
    }
}

i32 TAsyncChangeLog::GetId() const
{
    return ChangeLog->GetId();
}

i32 TAsyncChangeLog::GetRecordCount() const
{
    TImpl::TChangeLogQueue::TPtr queue = Impl->FindQueue(ChangeLog);

    if (~queue == NULL) {
        return ChangeLog->GetRecordCount();
    }
    
    TGuard<TSpinLock> guard(queue->SpinLock);
    if (queue->Records.Empty()) {
        return ChangeLog->GetRecordCount();
    } else {
        TImpl::TChangeLogQueue::TRecords::const_iterator it = queue->Records.End();
        --it;
        return it->Id + 1;
    }
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
    // TODO: Later on this can be improved to asynchronous behaviour by
    // getting rid of explicit synchronization.
    Flush();

    return ChangeLog->Truncate(atRecordId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

