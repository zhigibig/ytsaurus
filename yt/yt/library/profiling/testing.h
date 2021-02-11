#pragma once

#include "sensor.h"

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

struct TTesting
{
    static double ReadGauge(const TGauge& gauge);
    static TDuration ReadTimeGauge(const TTimeGauge& gauge);
    static i64 ReadCounter(const TCounter& counter);
    static TDuration ReadTimeCounter(const TTimeCounter& counter);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
