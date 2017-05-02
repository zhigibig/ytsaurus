#include "periodic_yielder.h"

#include "scheduler.h"

namespace NYT {
namespace NConcurrency {

using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

TPeriodicYielder::TPeriodicYielder(TDuration period)
    : Period_(DurationToCpuDuration(period))
    , Disabled_(false)
{ }

bool TPeriodicYielder::TryYield()
{
    if (Disabled_) {
        return false;
    }

    if (GetCpuInstant() - LastYieldTime_ > Period_) {
        // YT-5601: replace with Yield after merge into prestable/18.
        WaitFor(VoidFuture);
        LastYieldTime_ = GetCpuInstant();
        return true;
    }

    return false;
}

void TPeriodicYielder::SetDisabled(bool value)
{
    Disabled_ = value;
}

void TPeriodicYielder::SetPeriod(TDuration value)
{
    Period_ = DurationToCpuDuration(value);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NConcurrency
} // namespace NYT
