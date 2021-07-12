#pragma once

#include <yt/yt/server/lib/scheduler/public.h>

#include <yt/yt/ytlib/controller_agent/public.h>

#include <yt/yt/ytlib/scheduler/public.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

using NScheduler::TExecNodeDescriptorMap;
using NScheduler::TRefCountedExecNodeDescriptorMapPtr;
using NScheduler::TIncarnationId;
using NScheduler::TAgentId;
using NScheduler::EOperationAlertType;

////////////////////////////////////////////////////////////////////////////////

struct TJobSummary;
struct TCompletedJobSummary;
struct TAbortedJobSummary;
using TFailedJobSummary = TJobSummary;
struct TRunningJobSummary;

DECLARE_REFCOUNTED_CLASS(TLegacyProgressCounter)
DECLARE_REFCOUNTED_CLASS(TProgressCounter)
DECLARE_REFCOUNTED_STRUCT(IJobSizeConstraints)

////////////////////////////////////////////////////////////////////////////////

constexpr TDuration PrepareYieldPeriod = TDuration::MilliSeconds(100);

////////////////////////////////////////////////////////////////////////////////

extern const TString InputRowCountPath;
extern const TString InputUncompressedDataSizePath;
extern const TString InputCompressedDataSizePath;
extern const TString InputDataWeightPath;
extern const TString InputPipeIdleTimePath;
extern const TString JobProxyCpuUsagePath;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent
