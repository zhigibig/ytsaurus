#ifndef TIMING_INL_H_
#error "Direct inclusion of this file is not allowed, include timing.h"
// For the sake of sane code completion.
#include "timing.h"
#endif
#undef TIMING_INL_H_

#include "profiler.h"

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

template <class TTimer>
TValueIncrementingTimingGuard<TTimer>::TValueIncrementingTimingGuard(TDuration* value)
    : Value_(value)
{ }

template <class TTimer>
TValueIncrementingTimingGuard<TTimer>::~TValueIncrementingTimingGuard()
{
    *Value_ += Timer_.GetElapsedTime();
}

////////////////////////////////////////////////////////////////////////////////

template <class TTimer>
TTimerGuard<TTimer>::TTimerGuard(TTimer* timer)
    : Timer_(timer)
{
    Timer_->Start();
}

template <class TTimer>
TTimerGuard<TTimer>::~TTimerGuard()
{
    Timer_->Stop();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
