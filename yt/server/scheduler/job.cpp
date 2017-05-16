#include "private.h"
#include "job.h"
#include "exec_node.h"
#include "helpers.h"
#include "operation.h"

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/scheduler/job.pb.h>

#include <yt/core/ytree/fluent.h>

#include <yt/core/misc/enum.h>
#include <yt/core/misc/protobuf_helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NNodeTrackerClient::NProto;
using namespace NYTree;
using namespace NYson;
using namespace NObjectClient;
using namespace NYPath;
using namespace NJobTrackerClient;
using namespace NChunkClient::NProto;
using namespace NProto;
using namespace NProfiling;
using namespace NPhoenix;

////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;

////////////////////////////////////////////////////////////////////

TJob::TJob(
    const TJobId& id,
    EJobType type,
    const TOperationId& operationId,
    TExecNodePtr node,
    TInstant startTime,
    const TJobResources& resourceLimits,
    bool interruptible,
    TJobSpecBuilder specBuilder)
    : Id_(id)
    , Type_(type)
    , OperationId_(operationId)
    , Node_(node)
    , StartTime_(startTime)
    , Interruptible_(interruptible)
    , State_(EJobState::None)
    , ResourceUsage_(resourceLimits)
    , ResourceLimits_(resourceLimits)
    , SpecBuilder_(std::move(specBuilder))
{ }

TDuration TJob::GetDuration() const
{
    return *FinishTime_ - StartTime_;
}

void TJob::AnalyzeBriefStatistics(
    TDuration suspiciousInactivityTimeout,
    i64 suspiciousCpuUsageThreshold,
    double suspiciousInputPipeIdleTimeFraction,
    const TErrorOr<TBriefJobStatisticsPtr>& briefStatisticsOrError)
{
    if (!briefStatisticsOrError.IsOK()) {
        if (BriefStatistics_) {
            // Failures in brief statistics building are normal during job startup,
            // when readers and writers are not built yet. After we successfully built
            // brief statistics once, we shouldn't fail anymore.

            LOG_WARNING(
                briefStatisticsOrError,
                "Failed to build brief job statistics (JobId: %v)",
                Id_);
        }
        return;
    }

    const auto& briefStatistics = briefStatisticsOrError.Value();

    bool wasActive = false;

    if (!BriefStatistics_ || CheckJobActivity(
        BriefStatistics_,
        briefStatistics,
        suspiciousCpuUsageThreshold,
        suspiciousInputPipeIdleTimeFraction))
    {
        wasActive = true;
    }
    BriefStatistics_ = briefStatistics;

    bool wasSuspicious = Suspicious_;
    Suspicious_ = (!wasActive && BriefStatistics_->Timestamp - LastActivityTime_ > suspiciousInactivityTimeout);
    if (!wasSuspicious && Suspicious_) {
        LOG_DEBUG("Found a suspicious job (JobId: %v, LastActivityTime: %v, SuspiciousInactivityTimeout: %v)",
            Id_,
            LastActivityTime_,
            suspiciousInactivityTimeout);
    }

    if (wasActive) {
        LastActivityTime_ = BriefStatistics_->Timestamp;
    }
}

void TJob::SetStatus(TJobStatus* status)
{
    if (status) {
        Status_.Swap(status);
    }
    if (Status_.has_statistics()) {
        StatisticsYson_ = TYsonString(Status_.statistics());
    }
}

const Stroka& TJob::GetStatisticsSuffix() const
{
    auto state = (GetRestarted() && GetState() == EJobState::Completed) ? EJobState::Lost : GetState();
    auto type = GetType();
    return JobHelper.GetStatisticsSuffix(state, type);
}

////////////////////////////////////////////////////////////////////

TJobSummary::TJobSummary()
{ }

TJobSummary::TJobSummary(const TJobPtr& job, TJobStatus* status)
    : Id(job->GetId())
    , State(job->GetState())
    , FinishTime(job->GetFinishTime())
    , ShouldLog(true)
{
    // TODO(ignat): it is hacky way, we should avoid it by saving status in controller.
    if (!status) {
        return;
    }

    Result = status->result();
    if (status->has_prepare_duration()) {
        PrepareDuration = FromProto<TDuration>(status->prepare_duration());
    }
    if (status->has_download_duration()) {
        DownloadDuration = FromProto<TDuration>(status->download_duration());
    }
    if (status->has_exec_duration()) {
        ExecDuration.Emplace();
        FromProto(ExecDuration.GetPtr(), status->exec_duration());
    }
    if (status->has_statistics()) {
        StatisticsYson = TYsonString(status->statistics());
    }
}

TJobSummary::TJobSummary(const TJobId& id, EJobState state)
    : Result()
    , Id(id)
    , State(state)
    , ShouldLog(false)
{ }

void TJobSummary::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Result);
    Persist(context, Id);
    Persist(context, State);
    Persist(context, FinishTime);
    Persist(context, PrepareDuration);
    Persist(context, DownloadDuration);
    Persist(context, ExecDuration);
    Persist(context, Statistics);
    Persist(context, StatisticsYson);
    Persist(context, ShouldLog);
}

////////////////////////////////////////////////////////////////////

TCompletedJobSummary::TCompletedJobSummary(const TJobPtr& job, TJobStatus* status, bool abandoned)
    : TJobSummary(job, status)
    , Abandoned(abandoned)
{
    const auto& schedulerResultExt = Result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
    InterruptReason = job->GetInterruptReason();
    YCHECK((InterruptReason == EInterruptReason::None && schedulerResultExt.unread_input_data_slice_descriptors_size() == 0) ||
        (InterruptReason != EInterruptReason::None && schedulerResultExt.unread_input_data_slice_descriptors_size() != 0));
}

void TCompletedJobSummary::Persist(const TPersistenceContext& context)
{
    TJobSummary::Persist(context);

    using NYT::Persist;

    Persist(context, Abandoned);
    Persist(context, InterruptReason);
    // TODO(max42): now we persist only those completed job summaries that correspond
    // to non-interrupted jobs, because Persist(context, UnreadInputDataSlices) produces
    // lots of ugly template resolution errors. I wasn't able to fix it :(
    YCHECK(InterruptReason == EInterruptReason::None);
    Persist(context, SplitJobCount);
}

////////////////////////////////////////////////////////////////////

TAbortedJobSummary::TAbortedJobSummary(const TJobPtr& job, TJobStatus* status)
    : TJobSummary(job, status)
    , AbortReason(GetAbortReason(Result))
{ }

TAbortedJobSummary::TAbortedJobSummary(const TJobId& id, EAbortReason abortReason)
    : TJobSummary(id, EJobState::Aborted)
    , AbortReason(abortReason)
{ }

TAbortedJobSummary::TAbortedJobSummary(const TJobSummary& other, EAbortReason abortReason)
    : TJobSummary(other)
    , AbortReason(abortReason)
{ }

////////////////////////////////////////////////////////////////////

TRunningJobSummary::TRunningJobSummary(const TJobPtr& job, TJobStatus* status)
    : TJobSummary(job, status)
    , Progress(status->progress())
{ }

////////////////////////////////////////////////////////////////////

TJobStatus JobStatusFromError(const TError& error)
{
    auto status = TJobStatus();
    ToProto(status.mutable_result()->mutable_error(), error);
    return status;
}

////////////////////////////////////////////////////////////////////

TJobStartRequest::TJobStartRequest(
    TJobId id,
    EJobType type,
    const TJobResources& resourceLimits,
    bool interruptible,
    TJobSpecBuilder specBuilder)
    : Id(id)
    , Type(type)
    , ResourceLimits(resourceLimits)
    , Interruptible(interruptible)
    , SpecBuilder(std::move(specBuilder))
{ }
    
////////////////////////////////////////////////////////////////////

void TScheduleJobResult::RecordFail(EScheduleJobFailReason reason)
{
    ++Failed[reason];
}

bool TScheduleJobResult::IsBackoffNeeded() const
{
    return
        !JobStartRequest &&
        Failed[EScheduleJobFailReason::NotEnoughResources] == 0 &&
        Failed[EScheduleJobFailReason::NoLocalJobs] == 0;
}

bool TScheduleJobResult::IsScheduleStopNeeded() const
{
    return
        Failed[EScheduleJobFailReason::NotEnoughChunkLists] > 0 ||
        Failed[EScheduleJobFailReason::JobSpecThrottling] > 0;
}

////////////////////////////////////////////////////////////////////

TJobId MakeJobId(NObjectClient::TCellTag tag, NNodeTrackerClient::TNodeId nodeId)
{
    return MakeId(
        EObjectType::SchedulerJob,
        tag,
        RandomNumber<ui64>(),
        nodeId);
}

NNodeTrackerClient::TNodeId NodeIdFromJobId(const TJobId& jobId)
{
    return jobId.Parts32[0];
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
