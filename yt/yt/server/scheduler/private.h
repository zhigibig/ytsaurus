#pragma once

#include "public.h"
#include "exec_node.h"
#include "job.h"
#include "operation.h"

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

#include <yt/client/scheduler/private.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TSchedulerElement)
DECLARE_REFCOUNTED_CLASS(TOperationElement)
DECLARE_REFCOUNTED_CLASS(TOperationElementSharedState)
DECLARE_REFCOUNTED_CLASS(TCompositeSchedulerElement)
DECLARE_REFCOUNTED_CLASS(TPool)
DECLARE_REFCOUNTED_CLASS(TRootElement)

DECLARE_REFCOUNTED_STRUCT(ISchedulerTree)

DECLARE_REFCOUNTED_CLASS(TResourceTree)
DECLARE_REFCOUNTED_CLASS(TResourceTreeElement)
DECLARE_REFCOUNTED_STRUCT(IFairShareTreeHost)

DECLARE_REFCOUNTED_CLASS(TFairShareStrategyOperationController)
DECLARE_REFCOUNTED_STRUCT(IFairShareTreeSnapshot)

struct ISchedulerTreeHost;

class TFairShareContext;

class TJobMetrics;

using TJobCounter = THashMap<std::tuple<EJobType, EJobState>, i64>;
using TAbortedJobCounter = THashMap<std::tuple<EJobType, EJobState, EAbortReason>, i64>;
using TCompletedJobCounter = THashMap<std::tuple<EJobType, EJobState, EInterruptReason>, i64>;

DEFINE_ENUM(ESchedulableStatus,
    (Normal)
    (BelowFairShare)
);

DEFINE_ENUM(EJobRevivalPhase,
    (RevivingControllers)
    (ConfirmingJobs)
    (Finished)
);

DEFINE_ENUM(EResourceTreeIncreaseResult,
    (Success)
    (ElementIsNotAlive)
    (ResourceLimitExceeded)
);

extern const NLogging::TLogger SchedulerEventLogger;
extern const NLogging::TLogger SchedulerResourceMeteringLogger;

extern const NProfiling::TProfiler SchedulerProfiler;

static constexpr int MaxNodesWithoutPoolTreeToAlert = 10;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler

