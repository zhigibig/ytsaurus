#pragma once

#include "private.h"
#include "scheduling_context.h"

#include <yt/server/lib/scheduler/exec_node_descriptor.h>

#include <yt/ytlib/scheduler/job_resources.h>

#include <deque>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TPackingNodeResourcesSnapshot
{
public:
    DEFINE_BYREF_RO_PROPERTY(TJobResources, Usage);
    DEFINE_BYREF_RO_PROPERTY(TJobResources, Limits);
    DEFINE_BYREF_RO_PROPERTY(i64, DiskQuota);

public:
    TPackingNodeResourcesSnapshot() = default;
    TPackingNodeResourcesSnapshot(const TJobResources& usage, const TJobResources& limits, i64 diskQuota);


    Y_FORCE_INLINE TJobResources Free() const
    {
        return Limits_ - Usage_;
    }
};

class TPackingHeartbeatSnapshot
{
public:
    DEFINE_BYREF_RO_PROPERTY(NProfiling::TCpuInstant, Time);
    DEFINE_BYREF_RO_PROPERTY(TPackingNodeResourcesSnapshot, Resources);

public:
    TPackingHeartbeatSnapshot(NProfiling::TCpuInstant time, const TPackingNodeResourcesSnapshot& resources);

    bool CanSchedule(const TJobResourcesWithQuota& jobResourcesWithQuota) const;
};

TPackingHeartbeatSnapshot CreateHeartbeatSnapshot(const ISchedulingContextPtr& schedulingContext);

////////////////////////////////////////////////////////////////////////////////

class TPackingStatistics
{
public:
    void RecordHeartbeat(
        const TPackingHeartbeatSnapshot& heartbeatSnapshot,
        const TFairShareStrategyPackingConfigPtr& config);
    bool CheckPacking(
        const TPackingHeartbeatSnapshot& heartbeatSnapshot,
        const TJobResourcesWithQuota& jobResourcesWithQuota,
        const TJobResources& totalResourceLimits,
        const TFairShareStrategyPackingConfigPtr& config) const;

private:
    TSpinLock Lock_;
    std::deque<TPackingHeartbeatSnapshot> WindowOfHeartbeats_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
