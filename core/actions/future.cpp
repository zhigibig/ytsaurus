#include "future.h"
#include "invoker_util.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

const TFuture<void> VoidFuture = MakeWellKnownFuture(TError());
const TFuture<bool> TrueFuture = MakeWellKnownFuture(TErrorOr<bool>(true));
const TFuture<bool> FalseFuture = MakeWellKnownFuture(TErrorOr<bool>(false));

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

TFutureCallbackCookie TFutureState<void>::Subscribe(TVoidResultHandler handler)
{
    // Fast path.
    if (Set_) {
        RunNoExcept(handler, ResultError_);
        return NullFutureCallbackCookie;
    }

    // Slow path.
    {
        auto guard = Guard(SpinLock_);
        InstallAbandonedError();
        if (Set_) {
            guard.Release();
            RunNoExcept(handler, ResultError_);
            return NullFutureCallbackCookie;
        } else {
            HasHandlers_ = true;
            return VoidResultHandlers_.Add(std::move(handler));
        }
    }
}

void TFutureState<void>::Unsubscribe(TFutureCallbackCookie cookie)
{
    // Fast path.
    if (Set_ || cookie == NullFutureCallbackCookie) {
        return;
    }

    {
        auto guard = Guard(SpinLock_);
        if (Set_) {
            return;
        }
        YT_VERIFY(DoUnsubscribe(cookie, &guard));
    }
}

bool TFutureState<void>::Cancel(const TError& error) noexcept
{
    // NB: Cancel() could have been invoked when the last future reference
    // is already released.
    if (!TryRefFuture()) {
        // The instance is mostly dead anyway.
        return false;
    }
    // The reference is acquired above.
    TIntrusivePtr<TFutureState<void>> this_(this, /* addReference */ false);

    {
        auto guard = Guard(SpinLock_);
        if (Set_ || AbandonedUnset_ || Canceled_) {
            return false;
        }
        CancelationError_ = error;
        Canceled_ = true;
    }

    if (CancelHandlers_.empty()) {
        if (!TrySetError(NDetail::MakeCanceledError(error))) {
            return false;
        }
    } else {
        for (const auto& handler : CancelHandlers_) {
            RunNoExcept(handler, error);
        }
        CancelHandlers_.clear();
    }

    return true;
}

void TFutureState<void>::OnCanceled(TCancelHandler handler)
{
    // Fast path.
    if (Set_) {
        return;
    }
    if (Canceled_) {
        RunNoExcept(handler, CancelationError_);
        return;
    }

    // Slow path.
    {
        auto guard = Guard(SpinLock_);
        InstallAbandonedError();
        if (Canceled_) {
            guard.Release();
            RunNoExcept(handler, CancelationError_);
        } else if (!Set_) {
            CancelHandlers_.push_back(std::move(handler));
            HasHandlers_ = true;
        }
    }
}

bool TFutureState<void>::TimedWait(TDuration timeout) const
{
    return TimedWait(timeout.ToDeadLine());
}

bool TFutureState<void>::TimedWait(TInstant deadline) const
{
    // Fast path.
    if (Set_ || AbandonedUnset_) {
        return true;
    }

    // Slow path.
    {
        auto guard = Guard(SpinLock_);
        InstallAbandonedError();
        if (Set_) {
            return true;
        }
        if (!ReadyEvent_) {
            ReadyEvent_.reset(new NConcurrency::TEvent());
        }
    }

    return ReadyEvent_->Wait(deadline);
}

void TFutureState<void>::InstallAbandonedError() const
{
    const_cast<TFutureState<void>*>(this)->InstallAbandonedError();
}

void TFutureState<void>::InstallAbandonedError()
{
    VERIFY_SPINLOCK_AFFINITY(SpinLock_);
    if (AbandonedUnset_ && !Set_) {
        SetResultError(NDetail::MakeAbandonedError());
        Set_ = true;
    }
}

void TFutureState<void>::ResetResult()
{ }

void TFutureState<void>::SetResultError(const TError& error)
{
    VERIFY_SPINLOCK_AFFINITY(SpinLock_);
    ResultError_ = error;
}

bool TFutureState<void>::TrySetError(const TError& error)
{
    return TrySet(error);
}

bool TFutureState<void>::DoUnsubscribe(TFutureCallbackCookie cookie, TGuard<TSpinLock>* guard)
{
    VERIFY_SPINLOCK_AFFINITY(SpinLock_);
    return VoidResultHandlers_.TryRemove(cookie, guard);
}

void TFutureState<void>::WaitUntilSet() const
{
    // Fast path.
    if (Set_) {
        return;
    }

    // Slow path.
    {
        auto guard = Guard(SpinLock_);
        InstallAbandonedError();
        if (Set_) {
            return ;
        }
        if (!ReadyEvent_) {
            ReadyEvent_ = std::make_unique<NConcurrency::TEvent>();
        }
    }

    ReadyEvent_->Wait();
}

bool TFutureState<void>::CheckIfSet() const
{
    // Fast path.
    if (Set_) {
        return true;
    } else if (!AbandonedUnset_) {
        return false;
    }

    // Slow path.
    {
        auto guard = Guard(SpinLock_);
        InstallAbandonedError();
        return Set_;
    }
}

void TFutureState<void>::OnLastFutureRefLost()
{
    ResetResult();
    UnrefCancelable();
}

void TFutureState<void>::OnLastPromiseRefLost()
{
    // Check for fast path.
    if (Set_) {
        // Just kill the fake weak reference.
        UnrefFuture();
        return;
    }

    // Another fast path: no subscribers.
    {
        auto guard = Guard(SpinLock_);
        if (!HasHandlers_) {
            YT_ASSERT(!AbandonedUnset_);
            AbandonedUnset_ = true;
            // Cannot access this after UnrefFuture; in particular, cannot touch SpinLock_ in guard's dtor.
            guard.Release();
            UnrefFuture();
            return;
        }
    }

    // Slow path: notify the subscribers in a dedicated thread.
    GetFinalizerInvoker()->Invoke(BIND([=] () {
        // Set the promise if the value is still missing.
        TrySetError(NDetail::MakeAbandonedError());
        // Kill the fake weak reference.
        UnrefFuture();
    }));
}

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
