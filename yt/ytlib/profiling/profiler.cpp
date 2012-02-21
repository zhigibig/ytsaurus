#include "stdafx.h"
#include "profiler.h"
#include "profiling_manager.h"

#include <util/system/datetime.h>

namespace NYT {
namespace NProfiling  {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TProfiler::TProfiler(const TYPath& pathPrefix)
    : PathPrefix(pathPrefix)
{ }

void TProfiler::Enqueue(const TYPath& path, TValue value)
{
	TQueuedSample sample;
	sample.Time = GetCycleCount();
	sample.PathPrefix = PathPrefix;
	sample.Path = path;
	sample.Value = value;
	TProfilingManager::Get()->Enqueue(sample);
}

TCpuClock TProfiler::StartTiming()
{
    return GetCycleCount();
}

void TProfiler::StopTiming(TCpuClock start, const NYTree::TYPath& path)
{
    TCpuClock end = GetCycleCount();
    YASSERT(end >= start);
    Enqueue(path, end - start);
}

////////////////////////////////////////////////////////////////////////////////

TScopedProfiler::TScopedProfiler(const TYPath& pathPrefix)
    : TProfiler(pathPrefix)
{ }

void TScopedProfiler::StartScopedTiming(const TYPath& path)
{
    auto start = StartTiming();
    // Failure here means that another measurement for the same
    // path is already in progress.
    YVERIFY(Starts.insert(MakePair(path, start)).second);
}

void TScopedProfiler::StopScopedTiming(const NYTree::TYPath& path)
{
    auto it = Starts.find(path);
    // Failure here means that there is no active measurement for the
    // given path.
    YASSERT(it != Starts.end()); 
    auto start = it->second;
    Starts.erase(it);
	StopTiming(start, path);
}

////////////////////////////////////////////////////////////////////////////////

TTimingGuard::TTimingGuard(
    TProfiler* profiler,
    const TYPath& path)
    : Profiler(profiler)
    , Path(path)
    , Start(profiler->StartTiming())
{
    YASSERT(profiler);
}

TTimingGuard::~TTimingGuard()
{
    // Don't measure the time if an handled exception was raised.
    if (!std::uncaught_exception()) {
        Profiler->StopTiming(Start, Path);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NProfiling
} // namespace NYT
