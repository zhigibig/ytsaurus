#pragma once

#include "public.h"

#include <yt/core/actions/future.h>

// Someone above has defined this by including one of Windows headers.
#undef Yield

namespace NYT {
namespace NConcurrency {

////////////////////////////////////////////////////////////////////////////////

//! Scheduler holds a group of fibers executing on a particular thread
//! and provide means for cooperative multitasking on that thread.
struct IScheduler
{
    virtual ~IScheduler() = default;

    virtual TFiber* GetCurrentFiber() = 0;

    //! Returns control back to the scheduler.
    //! This must be called upon fiber termination.
    virtual void Return() = 0;

    //! Transfers control to other fiber and reschedules currently executing fiber
    //! to the end of the run queue.
    virtual void YieldTo(TFiberPtr&& other) = 0;

    //! Transfers control back to the scheduler and reschedules currently executing
    //! fiber via the specified invoker.
    virtual void SwitchTo(IInvokerPtr invoker) = 0;

    //! Installs a new context switch handler.
    /*!
     *  The provided #handler will be invoked in the scheduler's context
     *  when the current control context is switched. This happens on
     *  #Yield or #SwitchTo calls, when the fiber is canceled, terminates,
     *  or crashes due to an unhandled exception. Once invoked, the callback
     *  is discarded.
     */
    virtual void PushContextSwitchHandler(std::function<void()> callback) = 0;

    //! Removes the top switch handler.
    virtual void PopContextSwitchHandler() = 0;

    //! Transfers control back to the scheduler and puts currently executing fiber
    //! into sleep until occurrence of an external event.
    virtual void WaitFor(TFuture<void> future, IInvokerPtr invoker) = 0;

};

////////////////////////////////////////////////////////////////////////////////
// Provides a way to work with the current scheduler.
// Scheduler is thread-scoped so this is an access to TLS.

//! Returns the current scheduler. Fails if there's none.
IScheduler* GetCurrentScheduler();

//! Returns the current scheduler or |nullptr| if there's none.
IScheduler* TryGetCurrentScheduler();

class TCurrentSchedulerGuard
{
public:
    explicit TCurrentSchedulerGuard(IScheduler* scheduler);
    ~TCurrentSchedulerGuard();

private:
    IScheduler* SavedScheduler_;
};

////////////////////////////////////////////////////////////////////////////////
// Shortcuts.

TFiberId GetCurrentFiberId();
void Yield();
void SwitchTo(IInvokerPtr invoker);

////////////////////////////////////////////////////////////////////////////////

class TContextSwitchGuard
{
public:
    TContextSwitchGuard(std::function<void()> handler);
    TContextSwitchGuard(const TContextSwitchGuard& other) = delete;
    ~TContextSwitchGuard();

private:
    bool Active_;

};

////////////////////////////////////////////////////////////////////////////////

template <class T>
TErrorOr<T> WaitFor(TFuture<T> future);

template <class T>
TErrorOr<T> WaitFor(TFuture<T> future, IInvokerPtr invoker);

////////////////////////////////////////////////////////////////////////////////

//! Thrown when a fiber is being terminated by an external event.
class TFiberCanceledException
{ };

//! Delegates to TFiber::GetCanceler for the current fiber.
//! Used to avoid dependencies on |fiber.h|.
TClosure GetCurrentFiberCanceler();

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT

#define SCHEDULER_INL_H_
#include "scheduler-inl.h"
#undef SCHEDULER_INL_H_
