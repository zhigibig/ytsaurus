#include "hr_timer.h"

#if defined(_linux_)
#include <time.h>
#elif defined(_darwin_)
#include <mach/mach_time.h>
#elif defined(_win_)
// Any other includes?
#endif

#include <limits>

namespace NYT {
namespace NHRTimer {

////////////////////////////////////////////////////////////////////////////////

static const ui64 NumberOfNsInS = 1000000000UL;
static const ui64 NumberOfSamples = 1000UL;

void GetHRInstant(THRInstant* instant)
{
#if defined(_linux_)
    // See:
    // http://stackoverflow.com/questions/6814792/why-is-clock-gettime-so-erratic
    // http://stackoverflow.com/questions/7935518/is-clock-gettime-adequate-for-submicrosecond-timing
    struct timespec* ts = reinterpret_cast<struct timespec*>(instant);
    YCHECK(clock_gettime(CLOCK_REALTIME, ts) == 0);
#elif defined(_darwin_)
    // See http://lists.mysql.com/commits/70966
    static mach_timebase_info_data_t info = { 0, 0 };
    if (UNLIKELY(info.denom == 0)) {
        YCHECK(mach_timebase_info(&info) == 0);
    }
    ui64 time;
    YCHECK((time = mach_absolute_time()));
    time *= info.numer;
    time /= info.denom;
    instant->Seconds = time / NumberOfNsInS;
    instant->Nanoseconds = time % NumberOfNsInS;
#elif defined(_win_)
    static LARGE_INTEGER frequency = 0;
    if (UNLIKELY(frequency == 0)) {
        YCHECK(QueryPerformanceFrequency(&frequency));

    }
    LARGE_INTEGER time;
    YCHECK((QueryPerformanceCounter(&time)));
    instant->Seconds = time / frequency;
    instant->Nanoseconds = (time % frequency) * NumberOfNsInS / frequency;
#else
    #error "Unsupported architecture"
#endif
}

THRDuration GetHRDuration(const THRInstant& begin, const THRInstant& end)
{
    if (end.Seconds == begin.Seconds) {
        YASSERT(end.Nanoseconds >= begin.Nanoseconds);
        return end.Nanoseconds - begin.Nanoseconds;
    }

    YASSERT(
        end.Seconds > begin.Seconds &&
        end.Seconds - begin.Seconds <
            std::numeric_limits<THRDuration>::max() / NumberOfNsInS);
    return
        ( end.Seconds - begin.Seconds ) * NumberOfNsInS
        + end.Nanoseconds - begin.Nanoseconds;
}

THRDuration GetHRResolution()
{
    static THRDuration result = 0;
    if (LIKELY(result)) {
        return result;
    }

    std::vector<THRDuration> samples(NumberOfSamples);
    THRInstant begin;
    THRInstant end;
    for (int i = 0; i < NumberOfSamples; ++i) {
        GetHRInstant(&begin);
        GetHRInstant(&end);
        samples[i] = GetHRDuration(begin, end);
    }

    std::sort(samples.begin(), samples.end());
    result = samples[samples.size() / 2];
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHRTimer
} // namespace NYT
