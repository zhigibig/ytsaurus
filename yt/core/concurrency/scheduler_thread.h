#pragma once

#include "private.h"
#include "invoker_queue.h"
#include "event_count.h"
#include "scheduler.h"
#include "execution_context.h"
#include "thread_affinity.h"

#include <core/actions/callback.h>
#include <core/actions/future.h>
#include <core/actions/invoker.h>
#include <core/actions/signal.h>

#include <core/misc/shutdownable.h>

#include <core/profiling/profiler.h>

#include <util/system/thread.h>

#include <util/thread/lfqueue.h>

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

class TSchedulerThread
    : public TRefCounted
    , public IScheduler
    , public IShutdownable
{
public:
    virtual ~TSchedulerThread();

    void Start();
    virtual void Shutdown() override;

    TThreadId GetId() const;
    bool IsStarted() const;
    bool IsShutdown() const;

    virtual TFiber* GetCurrentFiber() override;
    virtual void Return() override;
    virtual void Yield() override;
    virtual void YieldTo(TFiberPtr&& other) override;
    virtual void SwitchTo(IInvokerPtr invoker) override;
    virtual void SubscribeContextSwitched(TClosure callback) override;
    virtual void UnsubscribeContextSwitched(TClosure callback) override;
    virtual void WaitFor(TFuture<void> future, IInvokerPtr invoker) override;

protected:
    TSchedulerThread(
        std::shared_ptr<TEventCount> callbackEventCount,
        const Stroka& threadName,
        const NProfiling::TTagIdList& tagIds,
        bool enableLogging,
        bool enableProfiling);

    virtual EBeginExecuteResult BeginExecute() = 0;
    virtual void EndExecute() = 0;

    virtual void OnStart();
    virtual void OnShutdown();

    virtual void OnThreadStart();
    virtual void OnThreadShutdown();

    static void* ThreadMain(void* opaque);
    void ThreadMain();
    void ThreadMainStep();

    void FiberMain(ui64 spawnedEpoch);
    bool FiberMainStep(ui64 spawnedEpoch);

    void Reschedule(TFiberPtr fiber, TFuture<void> future, IInvokerPtr invoker);

    void OnContextSwitch();

    std::shared_ptr<TEventCount> CallbackEventCount;
    Stroka ThreadName;
    bool EnableLogging;

    NProfiling::TProfiler Profiler;

    // First bit is an indicator whether startup was performed.
    // Second bit is an indicator whether shutdown was requested.
    std::atomic<ui64> Epoch = {0};
    static constexpr ui64 StartedEpochMask = 0x1;
    static constexpr ui64 ShutdownEpochMask = 0x2;
    static constexpr ui64 TurnShift = 2;
    static constexpr ui64 TurnDelta = 1 << TurnShift;

    TEvent ThreadStartedEvent;
    TEvent ThreadShutdownEvent;

    TThreadId ThreadId = InvalidThreadId;
    TThread Thread;

    TExecutionContext SchedulerContext;

    std::list<TFiberPtr> RunQueue;
    NProfiling::TSimpleCounter CreatedFibersCounter;
    NProfiling::TSimpleCounter AliveFibersCounter;

    TFiberPtr IdleFiber;
    TFiberPtr CurrentFiber;

    TFuture<void> WaitForFuture;
    IInvokerPtr SwitchToInvoker;

    TCallbackList<void()> ContextSwitchCallbacks;

    DECLARE_THREAD_AFFINITY_SLOT(HomeThread);
};

DEFINE_REFCOUNTED_TYPE(TSchedulerThread)

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
