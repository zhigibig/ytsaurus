#include "stdafx.h"
#include "action_queue.h"
#include "action_queue_detail.h"

#include <core/actions/invoker_util.h>

#include <core/ypath/token.h>

#include <core/profiling/profiling_manager.h>

namespace NYT {
namespace NConcurrency {

using namespace NProfiling;
using namespace NYPath;
using namespace NYTree;

///////////////////////////////////////////////////////////////////////////////

namespace {

TTagIdList GetThreadTagIds(const Stroka& threadName)
{
    TTagIdList tagIds;
    auto* profilingManager = TProfilingManager::Get();
    tagIds.push_back(profilingManager->RegisterTag("thread", threadName));
    return tagIds;
}

TTagIdList GetBucketTagIds(const Stroka& threadName, const Stroka& bucketName)
{
    TTagIdList tagIds;
    auto* profilingManager = TProfilingManager::Get();
    tagIds.push_back(profilingManager->RegisterTag("thread", threadName));
    tagIds.push_back(profilingManager->RegisterTag("bucket", bucketName));
    return tagIds;
}

} // namespace

///////////////////////////////////////////////////////////////////////////////

class TActionQueue::TImpl
    : public TRefCounted
{
public:
    explicit TImpl(
        const Stroka& threadName,
        bool enableLogging,
        bool enableProfiling)
        : Queue_(New<TInvokerQueue>(
            &EventCount_,
            GetThreadTagIds(threadName),
            enableLogging,
            enableProfiling))
        , Thread_(New<TSingleQueueExecutorThread>(
            Queue_,
            &EventCount_,
            threadName,
            GetThreadTagIds(threadName),
            enableLogging,
            enableProfiling))
    {
        Thread_->Start();
        Queue_->SetThreadId(Thread_->GetId());
    }

    ~TImpl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        Queue_->Shutdown();
        Thread_->Shutdown();
    }

    IInvokerPtr GetInvoker()
    {
        return Queue_;
    }

private:
    TEventCount EventCount_;
    TInvokerQueuePtr Queue_;
    TSingleQueueExecutorThreadPtr Thread_;

};

TActionQueue::TActionQueue(
    const Stroka& threadName,
    bool enableLogging,
    bool enableProfiling)
    : Impl(New<TImpl>(
        threadName,
        enableLogging,
        enableProfiling))
{ }

TActionQueue::~TActionQueue()
{ }

void TActionQueue::Shutdown()
{
    return Impl->Shutdown();
}

IInvokerPtr TActionQueue::GetInvoker()
{
    return Impl->GetInvoker();
}

TCallback<TActionQueuePtr()> TActionQueue::CreateFactory(const Stroka& threadName)
{
    return BIND([=] () {
        return New<TActionQueue>(threadName);
    });
}

///////////////////////////////////////////////////////////////////////////////

class TFairShareActionQueue::TImpl
    : public TExecutorThread
{
public:
    TImpl(
        const Stroka& threadName,
        const std::vector<Stroka>& bucketNames)
        : TExecutorThread(
            &EventCount,
            threadName,
            GetThreadTagIds(threadName),
            true,
            true)
        , Buckets_(bucketNames.size())
        , CurrentBucket_(nullptr)
    {
        Start();
        for (int index = 0; index < static_cast<int>(bucketNames.size()); ++index) {
            auto& queue = Buckets_[index].Queue;
            queue = New<TInvokerQueue>(
                &EventCount,
                GetBucketTagIds(threadName, bucketNames[index]),
                true,
                true);
            queue->SetThreadId(GetId());
        }
    }

    ~TImpl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        for (auto& bucket : Buckets_) {
            bucket.Queue->Shutdown();
        }
        TExecutorThread::Shutdown();
    }

    IInvokerPtr GetInvoker(int index)
    {
        YASSERT(0 <= index && index < static_cast<int>(Buckets_.size()));
        return Buckets_[index].Queue;
    }

private:
    TEventCount EventCount;

    struct TBucket
    {
        TBucket()
            : ExcessTime(0)
        { }

        TInvokerQueuePtr Queue;
        TCpuDuration ExcessTime;
    };

    std::vector<TBucket> Buckets_;
    TCpuInstant StartInstant_;

    TEnqueuedAction CurrentCallback_;
    TBucket* CurrentBucket_;


    TBucket* GetStarvingBucket()
    {
        // Compute min excess over non-empty queues.
        i64 minExcess = std::numeric_limits<i64>::max();
        TBucket* minBucket = nullptr;
        for (auto& bucket : Buckets_) {
            auto queue = bucket.Queue;
            // NB: queue can be null during startup due to race with ctor
            if (queue && !queue->IsEmpty()) {
                if (bucket.ExcessTime < minExcess) {
                    minExcess = bucket.ExcessTime;
                    minBucket = &bucket;
                }
            }
        }
        return minBucket;
    }

    virtual EBeginExecuteResult BeginExecute() override
    {
        YCHECK(!CurrentBucket_);

        // Check if any callback is ready at all.
        CurrentBucket_ = GetStarvingBucket();
        if (!CurrentBucket_) {
            return EBeginExecuteResult::QueueEmpty;
        }

        // Reduce excesses (with truncation).
        for (auto& bucket : Buckets_) {
            bucket.ExcessTime = std::max<i64>(0, bucket.ExcessTime - CurrentBucket_->ExcessTime);
        }

        // Pump the starving queue.
        StartInstant_ = GetCpuInstant();
        return CurrentBucket_->Queue->BeginExecute(&CurrentCallback_);
    }

    virtual void EndExecute() override
    {
        if (!CurrentBucket_)
            return;

        CurrentBucket_->Queue->EndExecute(&CurrentCallback_);
        CurrentBucket_->ExcessTime += (GetCpuInstant() - StartInstant_);
        CurrentBucket_ = nullptr;
    }

};

TFairShareActionQueue::TFairShareActionQueue(
    const Stroka& threadName,
    const std::vector<Stroka>& bucketNames)
    : Impl_(New<TImpl>(threadName, bucketNames))
{ }

TFairShareActionQueue::~TFairShareActionQueue()
{ }

IInvokerPtr TFairShareActionQueue::GetInvoker(int index)
{
    return Impl_->GetInvoker(index);
}

void TFairShareActionQueue::Shutdown()
{
    return Impl_->Shutdown();
}

///////////////////////////////////////////////////////////////////////////////

class TThreadPool::TImpl
    : public TRefCounted
{
public:
    TImpl(int threadCount, const Stroka& threadNamePrefix)
        : Queue_(New<TInvokerQueue>(
            &EventCount_,
            GetThreadTagIds(threadNamePrefix),
            true,
            true))
    {
        for (int i = 0; i < threadCount; ++i) {
            auto thread = New<TSingleQueueExecutorThread>(
                Queue_,
                &EventCount_,
                Sprintf("%s:%d", ~threadNamePrefix, i),
                GetThreadTagIds(threadNamePrefix),
                true,
                true);
            Threads_.push_back(thread);
            thread->Start();
        }
    }

    ~TImpl()
    {
        Shutdown();
    }

    void Shutdown()
    {
        Queue_->Shutdown();
        for (auto thread : Threads_) {
            thread->Shutdown();
        }
    }

    IInvokerPtr GetInvoker()
    {
        return Queue_;
    }

private:
    TEventCount EventCount_;
    TInvokerQueuePtr Queue_;
    std::vector<TExecutorThreadPtr> Threads_;

};

TThreadPool::TThreadPool(int threadCount, const Stroka& threadNamePrefix)
    : Impl(New<TImpl>(threadCount, threadNamePrefix))
{ }

TThreadPool::~TThreadPool()
{ }

void TThreadPool::Shutdown()
{
    return Impl->Shutdown();
}

IInvokerPtr TThreadPool::GetInvoker()
{
    return Impl->GetInvoker();
}

TCallback<TThreadPoolPtr()> TThreadPool::CreateFactory(int queueCount, const Stroka& threadName)
{
    return BIND([=] () {
        return NYT::New<NConcurrency::TThreadPool>(queueCount, threadName);
    });
}

///////////////////////////////////////////////////////////////////////////////

class TSerializedInvoker
    : public IInvoker
{
public:
    explicit TSerializedInvoker(IInvokerPtr underlyingInvoker)
        : UnderlyingInvoker_(std::move(underlyingInvoker))
    {
        Lock_.clear();
    }

    virtual void Invoke(const TClosure& callback) override
    {
        Queue_.Enqueue(callback);
        TrySchedule();
    }

    virtual NConcurrency::TThreadId GetThreadId() const override
    {
        return UnderlyingInvoker_->GetThreadId();
    }

private:
    IInvokerPtr UnderlyingInvoker_;
    TLockFreeQueue<TClosure> Queue_;
    std::atomic_flag Lock_;


    void TrySchedule()
    {
        if (Queue_.IsEmpty()) {
            return;
        }

        if (!Lock_.test_and_set(std::memory_order_acquire)) {
            UnderlyingInvoker_->Invoke(BIND(&TSerializedInvoker::DoInvoke, MakeStrong(this)));
        }
    }

    void DoInvoke()
    {
        // Execute as many callbacks as possible to minimize context switches.
        TClosure callback;
        while (Queue_.Dequeue(&callback)) {
            TCurrentInvokerGuard guard(this);
            callback.Run();
        }

        Lock_.clear(std::memory_order_release);
        TrySchedule();
    }

};

IInvokerPtr CreateSerializedInvoker(IInvokerPtr underlyingInvoker)
{
    return New<TSerializedInvoker>(underlyingInvoker);
}

///////////////////////////////////////////////////////////////////////////////

class TPrioritizedInvoker
    : public IPrioritizedInvoker
{
public:
    explicit TPrioritizedInvoker(IInvokerPtr underlyingInvoker)
        : UnderlyingInvoker_(std::move(underlyingInvoker))
    { }

    virtual void Invoke(const TClosure& callback, i64 priority) override
    {
        {
            TGuard<TSpinLock> guard(SpinLock_);
            TEntry entry;
            entry.Callback = callback;
            entry.Priority = priority;
            Heap_.emplace_back(std::move(entry));
            std::push_heap(Heap_.begin(), Heap_.end());
        }
        UnderlyingInvoker_->Invoke(BIND(&TPrioritizedInvoker::DoExecute, MakeStrong(this)));
    }

    virtual void Invoke(const TClosure& callback) override
    {
        UnderlyingInvoker_->Invoke(callback);
    }

    virtual TThreadId GetThreadId() const override
    {
        return UnderlyingInvoker_->GetThreadId();
    }

private:
    IInvokerPtr UnderlyingInvoker_;

    struct TEntry
    {
        TClosure Callback;
        i64 Priority;

        bool operator < (const TEntry& other) const
        {
            return Priority < other.Priority;
        }
    };

    TSpinLock SpinLock_;
    std::vector<TEntry> Heap_;

    void DoExecute()
    {
        TGuard<TSpinLock> guard(SpinLock_);
        std::pop_heap(Heap_.begin(), Heap_.end());
        auto callback = std::move(Heap_.back().Callback);
        Heap_.pop_back();
        guard.Release();
        callback.Run();
    }

};

IPrioritizedInvokerPtr CreatePrioritizedInvoker(IInvokerPtr underlyingInvoker)
{
    return New<TPrioritizedInvoker>(std::move(underlyingInvoker));
}

///////////////////////////////////////////////////////////////////////////////

class TFakePrioritizedInvoker
    : public IPrioritizedInvoker
{
public:
    explicit TFakePrioritizedInvoker(IInvokerPtr underlyingInvoker)
        : UnderlyingInvoker_(std::move(underlyingInvoker))
    { }

    virtual void Invoke(const TClosure& callback, i64 /*priority*/) override
    {
        return UnderlyingInvoker_->Invoke(callback);
    }

    virtual void Invoke(const TClosure& callback) override
    {
        return UnderlyingInvoker_->Invoke(callback);
    }

    virtual NConcurrency::TThreadId GetThreadId() const override
    {
        return UnderlyingInvoker_->GetThreadId();
    }

private:
    IInvokerPtr UnderlyingInvoker_;

};

IPrioritizedInvokerPtr CreateFakePrioritizedInvoker(IInvokerPtr underlyingInvoker)
{
    return New<TFakePrioritizedInvoker>(std::move(underlyingInvoker));
}

///////////////////////////////////////////////////////////////////////////////

class TBoundedConcurrencyInvoker
    : public IInvoker
{
public:
    TBoundedConcurrencyInvoker(
        IInvokerPtr underlyingInvoker,
        int maxConcurrentInvocations)
        : UnderlyingInvoker_(underlyingInvoker)
        , MaxConcurrentInvocations_(maxConcurrentInvocations)
        , Semaphore_(0)
    { }

    virtual void Invoke(const TClosure& callback) override
    {
        // Fast path, no queue.
        if (++Semaphore_ <= MaxConcurrentInvocations_) {
            Schedule(callback);
            return;
        }

        // Slow path, with queue.
        --Semaphore_;
        Queue_.Enqueue(callback);

        if (++Semaphore_ <= MaxConcurrentInvocations_) {
            ScheduleOrRelease();
        } else {
            --Semaphore_;
        }
    }

    virtual TThreadId GetThreadId() const
    {
        return UnderlyingInvoker_->GetThreadId();
    }

private:
    IInvokerPtr UnderlyingInvoker_;
    int MaxConcurrentInvocations_;

    std::atomic<int> Semaphore_;
    TLockFreeQueue<TClosure> Queue_;


    void Schedule(const TClosure& callback)
    {
        UnderlyingInvoker_->Invoke(BIND(
            &TBoundedConcurrencyInvoker::CallbackWrapper,
            MakeStrong(this),
            callback));
    }

    void ScheduleOrRelease()
    {
        TClosure callback;
        if (Queue_.Dequeue(&callback)) {
            Schedule(callback);
        } else {
            --Semaphore_;
        }
    }

    void CallbackWrapper(const TClosure& callback)
    {
        callback.Run();
        ScheduleOrRelease();
    }

};

IInvokerPtr CreateBoundedConcurrencyInvoker(
    IInvokerPtr underlyingInvoker,
    int maxConcurrentInvocations)
{
    return New<TBoundedConcurrencyInvoker>(
        underlyingInvoker,
        maxConcurrentInvocations);
}

///////////////////////////////////////////////////////////////////////////////

IPrioritizedInvokerPtr CreateBoundedConcurrencyInvoker(
    IPrioritizedInvokerPtr underlyingInvoker,
    int maxConcurrentInvocations)
{
    YUNIMPLEMENTED();
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
