#include "fair_share_tree_element.h"

#include "fair_share_tree.h"
#include "helpers.h"
#include "resource_tree_element.h"
#include "scheduling_context.h"

#include <yt/yt/ytlib/scheduler/job_resources_helpers.h>

#include <yt/yt/core/profiling/profiler.h>
#include <yt/yt/core/profiling/profile_manager.h>

#include <yt/yt/core/misc/heap.h>
#include <yt/yt/core/misc/finally.h>
#include <yt/yt/core/misc/historic_usage_aggregator.h>

#include <yt/yt/core/profiling/timing.h>

#include <util/generic/ymath.h>

#include <math.h>

namespace NYT::NScheduler {

using namespace NConcurrency;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NYTree;
using namespace NProfiling;
using namespace NControllerAgent;

using NProfiling::CpuDurationToDuration;
using NVectorHdrf::ToJobResources;

////////////////////////////////////////////////////////////////////////////////

int SchedulingIndexToProfilingRangeIndex(int schedulingIndex)
{
    return std::min(
        static_cast<int>((schedulingIndex == 0) ? 0 : (MostSignificantBit(schedulingIndex) + 1)),
        SchedulingIndexProfilingRangeCount);
}

TString FormatProfilingRangeIndex(int rangeIndex)
{
    switch (rangeIndex) {
        case 0:
        case 1:
            return ToString(rangeIndex);
        case SchedulingIndexProfilingRangeCount:
            return Format("%v-inf", 1 << (SchedulingIndexProfilingRangeCount - 1));
        default:
            return Format("%v-%v", 1 << (rangeIndex - 1), (1 << rangeIndex) - 1);
    }
}

////////////////////////////////////////////////////////////////////////////////

static const TString InvalidCustomProfilingTag("invalid");

static const TJobWithPreemptionInfoSet EmptyJobWithPreemptionInfoSet;

////////////////////////////////////////////////////////////////////////////////

TScheduleJobsProfilingCounters::TScheduleJobsProfilingCounters(
    const NProfiling::TProfiler& profiler)
    : PrescheduleJobCount(profiler.Counter("/preschedule_job_count"))
    , UselessPrescheduleJobCount(profiler.Counter("/useless_preschedule_job_count"))
    , PrescheduleJobTime(profiler.Timer("/preschedule_job_time"))
    , TotalControllerScheduleJobTime(profiler.Timer("/controller_schedule_job_time/total"))
    , ExecControllerScheduleJobTime(profiler.Timer("/controller_schedule_job_time/exec"))
    , StrategyScheduleJobTime(profiler.Timer("/strategy_schedule_job_time"))
    , PackingRecordHeartbeatTime(profiler.Timer("/packing_record_heartbeat_time"))
    , PackingCheckTime(profiler.Timer("/packing_check_time"))
    , AnalyzeJobsTime(profiler.Timer("/analyze_jobs_time"))
    , CumulativePrescheduleJobTime(profiler.TimeCounter("/cumulative_preschedule_job_time"))
    , CumulativeTotalControllerScheduleJobTime(profiler.TimeCounter("/cumulative_controller_schedule_job_time/total"))
    , CumulativeExecControllerScheduleJobTime(profiler.TimeCounter("/cumulative_controller_schedule_job_time/exec"))
    , CumulativeStrategyScheduleJobTime(profiler.TimeCounter("/cumulative_strategy_schedule_job_time"))
    , CumulativeAnalyzeJobsTime(profiler.TimeCounter("/cumulative_analyze_jobs_time"))
    , ScheduleJobAttemptCount(profiler.Counter("/schedule_job_attempt_count"))
    , ScheduleJobFailureCount(profiler.Counter("/schedule_job_failure_count"))
    , ControllerScheduleJobCount(profiler.Counter("/controller_schedule_job_count"))
    , ControllerScheduleJobTimedOutCount(profiler.Counter("/controller_schedule_job_timed_out_count"))
{
    for (auto reason : TEnumTraits<NControllerAgent::EScheduleJobFailReason>::GetDomainValues()) {
        ControllerScheduleJobFail[reason] = profiler
            .WithTag("reason", FormatEnum(reason))
            .Counter("/controller_schedule_job_fail");
    }
    for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
        DeactivationCount[reason] = profiler
            .WithTag("reason", FormatEnum(reason))
            .Counter("/deactivation_count");
    }
    for (int rangeIndex = 0; rangeIndex <= SchedulingIndexProfilingRangeCount; ++rangeIndex) {
        SchedulingIndexCounters[rangeIndex] = profiler
            .WithTag("scheduling_index", FormatProfilingRangeIndex(rangeIndex))
            .Counter("/operation_scheduling_index_attempt_count");
        MaxSchedulingIndexCounters[rangeIndex] = profiler
            .WithTag("scheduling_index", FormatProfilingRangeIndex(rangeIndex))
            .Counter("/max_operation_scheduling_index");
    }
}

////////////////////////////////////////////////////////////////////////////////

void FormatValue(TStringBuilderBase* builder, const TJobWithPreemptionInfo& jobInfo, TStringBuf /*format*/)
{
    builder->AppendFormat("{JobId: %v, PreemptionStatus: %v, OperationId: %v}",
        jobInfo.Job->GetId(),
        jobInfo.PreemptionStatus,
        jobInfo.OperationElement->GetId());
}

TString ToString(const TJobWithPreemptionInfo& jobInfo)
{
    return ToStringViaBuilder(jobInfo);
}

////////////////////////////////////////////////////////////////////////////////

void TPersistentAttributes::ResetOnElementEnabled()
{
    // NB: We don't want to reset all attributes.
    auto resetAttributes = TPersistentAttributes();
    resetAttributes.IntegralResourcesState = IntegralResourcesState;
    resetAttributes.LastNonStarvingTime = TInstant::Now();
    resetAttributes.SchedulingSegmentModule = SchedulingSegmentModule;
    *this = resetAttributes;
}

////////////////////////////////////////////////////////////////////////////////

TScheduleJobsContext::TScheduleJobsContext(
    ISchedulingContextPtr schedulingContext,
    std::vector<TSchedulingTagFilter> registeredSchedulingTagFilters,
    bool enableSchedulingInfoLogging,
    const NLogging::TLogger& logger)
    : SchedulingContext_(std::move(schedulingContext))
    , RegisteredSchedulingTagFilters_(std::move(registeredSchedulingTagFilters))
    , EnableSchedulingInfoLogging_(enableSchedulingInfoLogging)
    , Logger(logger)
{ }

EOperationPreemptionPriority TScheduleJobsContext::GetOperationPreemptionPriority(
    const TSchedulerOperationElement* operationElement) const
{
    bool isEligibleForSsdPriorityPreemption = SsdPriorityPreemptionEnabled_ &&
        IsEligibleForSsdPriorityPreemption(operationElement->DiskRequestMedia());
    if (operationElement->GetLowestAggressivelyStarvingAncestor()) {
        return isEligibleForSsdPriorityPreemption
            ? EOperationPreemptionPriority::SsdAggressive
            : EOperationPreemptionPriority::Aggressive;
    }
    if (operationElement->GetLowestStarvingAncestor()) {
        return isEligibleForSsdPriorityPreemption
            ? EOperationPreemptionPriority::SsdRegular
            : EOperationPreemptionPriority::Regular;
    }
    return EOperationPreemptionPriority::None;
}

void TScheduleJobsContext::CountOperationsByPreemptionPriority(const TSchedulerRootElementPtr& rootElement)
{
    rootElement->CountOperationsByPreemptionPriority(this);
    SchedulingStatistics_.OperationCountByPreemptionPriority = OperationCountByPreemptionPriority_;
}

EJobPreemptionLevel TScheduleJobsContext::GetJobPreemptionLevel(const TJobWithPreemptionInfo& jobWithPreemptionInfo) const
{
    const auto& [job, preemptionStatus, operationElement] = jobWithPreemptionInfo;

    bool isEligibleForSsdPriorityPreemption = SsdPriorityPreemptionEnabled_ &&
        IsEligibleForSsdPriorityPreemption(GetDiskQuotaMedia(job->DiskQuota()));
    auto aggressivePreemptionAllowed = operationElement->GetEffectiveAggressivePreemptionAllowed();
    switch (preemptionStatus) {
        case EJobPreemptionStatus::NonPreemptable:
            return isEligibleForSsdPriorityPreemption
                ? EJobPreemptionLevel::SsdNonPreemptable
                : EJobPreemptionLevel::NonPreemptable;
        case EJobPreemptionStatus::AggressivelyPreemptable:
            if (aggressivePreemptionAllowed) {
                return isEligibleForSsdPriorityPreemption
                    ? EJobPreemptionLevel::SsdAggressivelyPreemptable
                    : EJobPreemptionLevel::AggressivelyPreemptable;
            } else {
                return isEligibleForSsdPriorityPreemption
                    ? EJobPreemptionLevel::SsdNonPreemptable
                    : EJobPreemptionLevel::NonPreemptable;
            }
        case EJobPreemptionStatus::Preemptable:
            return EJobPreemptionLevel::Preemptable;
        default:
            YT_ABORT();
    }
}

void TScheduleJobsContext::PrepareForScheduling(const TSchedulerRootElementPtr& rootElement)
{
    YT_VERIFY(StageState_);
    YT_VERIFY(!StageState_->PrescheduleExecuted);

    if (!Initialized_) {
        Initialized_ = true;

        CanSchedule_.reserve(RegisteredSchedulingTagFilters_.size());
        for (const auto& filter : RegisteredSchedulingTagFilters_) {
            CanSchedule_.push_back(SchedulingContext_->CanSchedule(filter));
        }

        if (DynamicAttributesListSnapshot_) {
            DynamicAttributesList_ = DynamicAttributesListSnapshot_->Value;
        } else {
            DynamicAttributesList_.InitializeResourceUsage(
                rootElement.Get(),
                /*resourceUsageSnapshot*/ nullptr,
                SchedulingContext_->GetNow());
        }
    } else {
        DynamicAttributesList_.DeactivateAll();
        ChildHeapMap_.clear();
    }
}

void TScheduleJobsContext::PrescheduleJob(const TSchedulerRootElementPtr& rootElement, EOperationPreemptionPriority targetOperationPreemptionPriority)
{
    TWallTimer prescheduleTimer;

    rootElement->PrescheduleJob(this, targetOperationPreemptionPriority);

    StageState_->PrescheduleDuration = prescheduleTimer.GetElapsedTime();
    StageState_->PrescheduleExecuted = true;
}

void TScheduleJobsContext::PrepareConditionalUsageDiscounts(const TSchedulerRootElementPtr& rootElement, EOperationPreemptionPriority targetOperationPreemptionPriority)
{
    CurrentConditionalDiscount_ = {};

    rootElement->PrepareConditionalUsageDiscounts(this, targetOperationPreemptionPriority);
}

const TJobWithPreemptionInfoSet& TScheduleJobsContext::GetConditionallyPreemptableJobsInPool(const TSchedulerCompositeElement* element) const
{
    auto it = ConditionallyPreemptableJobSetMap_.find(element->GetTreeIndex());
    return it != ConditionallyPreemptableJobSetMap_.end() ? it->second : EmptyJobWithPreemptionInfoSet;
}

TJobResources TScheduleJobsContext::GetLocalUnconditionalUsageDiscountFor(const TSchedulerElement* element) const
{
    int index = element->GetTreeIndex();
    YT_VERIFY(index != UnassignedTreeIndex);

    auto it = LocalUnconditionalUsageDiscountMap_.find(index);
    return it != LocalUnconditionalUsageDiscountMap_.end() ? it->second : TJobResources{};
}

TScheduleJobsContext::TStageState::TStageState(TScheduleJobsStage* schedulingStage)
    : SchedulingStage(schedulingStage)
{ }

void TScheduleJobsContext::StartStage(TScheduleJobsStage* schedulingStage)
{
    YT_VERIFY(!StageState_);

    StageState_.emplace(schedulingStage);

    Timer_ = TWallTimer();
}

EJobSchedulingStage TScheduleJobsContext::GetStageType()
{
    return StageState()->SchedulingStage->Type;
}

void TScheduleJobsContext::ProfileAndLogStatisticsOfStage()
{
    YT_VERIFY(StageState_);

    StageState_->TotalDuration = Timer_.GetElapsedTime();

    ProfileStageStatistics();

    if (StageState_->ScheduleJobAttemptCount > 0 && EnableSchedulingInfoLogging_) {
        LogStageStatistics();
    }
}

void TScheduleJobsContext::FinishStage()
{
    YT_VERIFY(StageState_);

    SchedulingStatistics_.ScheduleJobAttemptCountPerStage[GetStageType()] = StageState_->ScheduleJobAttemptCount;
    ProfileAndLogStatisticsOfStage();

    StageState_ = std::nullopt;
}

void TScheduleJobsContext::ProfileStageStatistics()
{
    if (!Initialized_) {
        return;
    }

    YT_VERIFY(StageState_);

    auto* profilingCounters = &StageState_->SchedulingStage->ProfilingCounters;

    profilingCounters->PrescheduleJobTime.Record(StageState_->PrescheduleDuration);
    profilingCounters->CumulativePrescheduleJobTime.Add(StageState_->PrescheduleDuration);

    if (StageState_->PrescheduleExecuted) {
        profilingCounters->PrescheduleJobCount.Increment();
        if (StageState_->ScheduleJobAttemptCount == 0) {
            profilingCounters->UselessPrescheduleJobCount.Increment();
        }
    }

    auto strategyScheduleJobDuration = StageState_->TotalDuration
        - StageState_->PrescheduleDuration
        - StageState_->TotalScheduleJobDuration;
    profilingCounters->StrategyScheduleJobTime.Record(strategyScheduleJobDuration);
    profilingCounters->CumulativeStrategyScheduleJobTime.Add(strategyScheduleJobDuration);

    profilingCounters->TotalControllerScheduleJobTime.Record(StageState_->TotalScheduleJobDuration);
    profilingCounters->CumulativeTotalControllerScheduleJobTime.Add(StageState_->TotalScheduleJobDuration);
    profilingCounters->ExecControllerScheduleJobTime.Record(StageState_->ExecScheduleJobDuration);
    profilingCounters->CumulativeExecControllerScheduleJobTime.Add(StageState_->ExecScheduleJobDuration);
    profilingCounters->PackingRecordHeartbeatTime.Record(StageState_->PackingRecordHeartbeatDuration);
    profilingCounters->PackingCheckTime.Record(StageState_->PackingCheckDuration);
    profilingCounters->AnalyzeJobsTime.Record(StageState_->AnalyzeJobsDuration);
    profilingCounters->CumulativeAnalyzeJobsTime.Add(StageState_->AnalyzeJobsDuration);

    profilingCounters->ScheduleJobAttemptCount.Increment(StageState_->ScheduleJobAttemptCount);
    profilingCounters->ScheduleJobFailureCount.Increment(StageState_->ScheduleJobFailureCount);
    profilingCounters->ControllerScheduleJobCount.Increment(SchedulingStatistics().ControllerScheduleJobCount);
    profilingCounters->ControllerScheduleJobTimedOutCount.Increment(SchedulingStatistics().ControllerScheduleJobTimedOutCount);

    for (auto reason : TEnumTraits<EScheduleJobFailReason>::GetDomainValues()) {
        profilingCounters->ControllerScheduleJobFail[reason].Increment(StageState_->FailedScheduleJob[reason]);
    }
    for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
        profilingCounters->DeactivationCount[reason].Increment(StageState_->DeactivationReasons[reason]);
    }

    for (auto [schedulingIndex, count] : StageState_->SchedulingIndexToScheduleJobAttemptCount) {
        int rangeIndex = SchedulingIndexToProfilingRangeIndex(schedulingIndex);
        profilingCounters->SchedulingIndexCounters[rangeIndex].Increment(count);
    }
    if (StageState_->MaxSchedulingIndex >= 0) {
        profilingCounters->MaxSchedulingIndexCounters[SchedulingIndexToProfilingRangeIndex(StageState_->MaxSchedulingIndex)].Increment();
    }

}

void TScheduleJobsContext::LogStageStatistics()
{
    if (!Initialized_) {
        return;
    }

    YT_VERIFY(StageState_);

    YT_LOG_DEBUG(
        "Scheduling statistics (SchedulingStage: %v, ActiveTreeSize: %v, ActiveOperationCount: %v, TotalHeapElementCount: %v, "
        "DeactivationReasons: %v, CanStartMoreJobs: %v, Address: %v, SchedulingSegment: %v, MaxSchedulingIndex: %v)",
        StageState_->SchedulingStage->Type,
        StageState_->ActiveTreeSize,
        StageState_->ActiveOperationCount,
        StageState_->TotalHeapElementCount,
        StageState_->DeactivationReasons,
        SchedulingContext_->CanStartMoreJobs(),
        SchedulingContext_->GetNodeDescriptor().Address,
        SchedulingContext_->GetSchedulingSegment(),
        StageState_->MaxSchedulingIndex);
}

bool TScheduleJobsContext::IsEligibleForSsdPriorityPreemption(const THashSet<int>& diskRequestMedia) const
{
    for (const auto& medium : diskRequestMedia) {
        if (SsdPriorityPreemptionMedia_.contains(medium)) {
            return true;
        }
    }
    return false;
}

////////////////////////////////////////////////////////////////////////////////

TSchedulerElementFixedState::TSchedulerElementFixedState(
    ISchedulerStrategyHost* strategyHost,
    IFairShareTreeElementHost* treeElementHost,
    TFairShareStrategyTreeConfigPtr treeConfig,
    TString treeId)
    : StrategyHost_(strategyHost)
    , TreeElementHost_(treeElementHost)
    , TreeConfig_(std::move(treeConfig))
    , TotalResourceLimits_(strategyHost->GetResourceLimits(TreeConfig_->NodesFilter))
    , TreeId_(std::move(treeId))
{ }

////////////////////////////////////////////////////////////////////////////////

void TSchedulerElement::MarkImmutable()
{
    Mutable_ = false;
}

int TSchedulerElement::EnumerateElements(int startIndex, bool isSchedulableValueFilter)
{
    YT_VERIFY(Mutable_);

    if (isSchedulableValueFilter == IsSchedulable()) {
        TreeIndex_ = startIndex++;
    }

    return startIndex;
}

void TSchedulerElement::UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config)
{
    YT_VERIFY(Mutable_);

    TreeConfig_ = config;
}

void TSchedulerElement::PreUpdateBottomUp(NVectorHdrf::TFairShareUpdateContext* context)
{
    YT_VERIFY(Mutable_);

    // NB: The order of computation should be: TotalResourceLimits_, SchedulingTagFilterResourceLimits_, ResourceLimits_.
    TotalResourceLimits_ = context->TotalResourceLimits;
    SchedulingTagFilterResourceLimits_ = ComputeSchedulingTagFilterResourceLimits();
    ResourceLimits_ = ComputeResourceLimits();
    HasSpecifiedResourceLimits_ = GetSpecifiedResourceLimits() != TJobResources::Infinite();

    auto specifiedResourceLimits = GetSpecifiedResourceLimits();
    if (PersistentAttributes_.AppliedResourceLimits != specifiedResourceLimits) {
        std::vector<TResourceTreeElementPtr> descendantOperationElements;
        if (!IsOperation() && PersistentAttributes_.AppliedResourceLimits == TJobResources::Infinite() && specifiedResourceLimits != TJobResources::Infinite()) {
            // NB: this code executed in control thread, therefore tree structure is actual and agreed with tree structure of resource tree.
            CollectResourceTreeOperationElements(&descendantOperationElements);
        }
        ResourceTreeElement_->SetResourceLimits(specifiedResourceLimits, descendantOperationElements);
        PersistentAttributes_.AppliedResourceLimits = specifiedResourceLimits;
    }
}

void TSchedulerElement::UpdatePreemptionAttributes()
{
    YT_VERIFY(Mutable_);

    if (Parent_) {
        EffectiveFairShareStarvationTolerance_ = GetSpecifiedFairShareStarvationTolerance().value_or(
            Parent_->GetEffectiveFairShareStarvationTolerance());

        EffectiveFairShareStarvationTimeout_ = GetSpecifiedFairShareStarvationTimeout().value_or(
            Parent_->GetEffectiveFairShareStarvationTimeout());

        EffectiveAggressivePreemptionAllowed_ = IsAggressivePreemptionAllowed()
            .value_or(Parent_->GetEffectiveAggressivePreemptionAllowed());

        EffectiveAggressiveStarvationEnabled_ = IsAggressiveStarvationEnabled()
            .value_or(Parent_->GetEffectiveAggressiveStarvationEnabled());
    } else { // Root case
        YT_VERIFY(GetSpecifiedFairShareStarvationTolerance().has_value());
        EffectiveFairShareStarvationTolerance_ = *GetSpecifiedFairShareStarvationTolerance();

        YT_VERIFY(GetSpecifiedFairShareStarvationTimeout().has_value());
        EffectiveFairShareStarvationTimeout_ = *GetSpecifiedFairShareStarvationTimeout();

        YT_VERIFY(IsAggressivePreemptionAllowed().has_value());
        EffectiveAggressivePreemptionAllowed_ = *IsAggressivePreemptionAllowed();

        YT_VERIFY(IsAggressiveStarvationEnabled().has_value());
        EffectiveAggressiveStarvationEnabled_ = *IsAggressiveStarvationEnabled();

    }
}

void TSchedulerElement::UpdateSchedulableAttributesFromDynamicAttributes(
    TDynamicAttributesList* dynamicAttributesList,
    TChildHeapMap* childHeapMap)
{
    YT_VERIFY(Mutable_);

    auto& attributes = dynamicAttributesList->AttributesOf(this);

    UpdateDynamicAttributes(dynamicAttributesList, *childHeapMap, /*checkLiveness*/ false);

    Attributes_.SatisfactionRatio = attributes.SatisfactionRatio;
    Attributes_.LocalSatisfactionRatio = ComputeLocalSatisfactionRatio(ResourceUsageAtUpdate_);
    Attributes_.Alive = attributes.Active;
}

const TSchedulingTagFilter& TSchedulerElement::GetSchedulingTagFilter() const
{
    return EmptySchedulingTagFilter;
}

TString TSchedulerElement::GetLoggingAttributesString() const
{
    return Format(
        "Status: %v, "
        "DominantResource: %v, "
        "DemandShare: %.6g, "
        "UsageShare: %.6g, "
        "LimitsShare: %.6g, "
        "StrongGuaranteeShare: %.6g, "
        "FairShare: %.6g, "
        "Satisfaction: %.4lg, "
        "LocalSatisfaction: %.4lg, "
        "PromisedFairShare: %.6g, "
        "StarvationStatus: %v, "
        "Weight: %v, "
        "Volume: %v",
        GetStatus(),
        Attributes_.DominantResource,
        Attributes_.DemandShare,
        Attributes_.UsageShare,
        Attributes_.LimitsShare,
        Attributes_.StrongGuaranteeShare,
        Attributes_.FairShare,
        Attributes_.SatisfactionRatio,
        Attributes_.LocalSatisfactionRatio,
        Attributes_.PromisedFairShare,
        GetStarvationStatus(),
        GetWeight(),
        GetAccumulatedResourceRatioVolume());
}

TString TSchedulerElement::GetLoggingString() const
{
    return Format("Scheduling info for tree %Qv = {%v}", GetTreeId(), GetLoggingAttributesString());
}

TJobResources TSchedulerElement::GetCurrentResourceUsage(const TDynamicAttributesList& dynamicAttributesList) const
{
    if (IsSchedulable()) {
        return dynamicAttributesList.AttributesOf(this).ResourceUsage;
    } else {
        return Attributes_.UnschedulableOperationsResourceUsage;
    }
}

double TSchedulerElement::GetWeight() const
{
    auto specifiedWeight = GetSpecifiedWeight();

    if (auto parent = GetParent();
        parent && parent->IsInferringChildrenWeightsFromHistoricUsageEnabled())
    {
        // TODO(eshcherbin): Make the method of calculating weights from historic usage configurable.
        auto multiplier = Exp2(-1.0 * PersistentAttributes_.HistoricUsageAggregator.GetHistoricUsage());
        auto weight = specifiedWeight ? *specifiedWeight : 1.0;
        return weight * multiplier;
    }

    if (specifiedWeight) {
        return *specifiedWeight;
    }

    if (!TreeConfig_->InferWeightFromGuaranteesShareMultiplier) {
        return 1.0;
    }
    double selfGuaranteeDominantShare = MaxComponent(Attributes().StrongGuaranteeShare) + Attributes().TotalResourceFlowRatio;

    if (selfGuaranteeDominantShare < NVectorHdrf::RatioComputationPrecision) {
        return 1.0;
    }

    double parentGuaranteeDominantShare = 1.0;
    if (GetParent()) {
        parentGuaranteeDominantShare = MaxComponent(GetParent()->Attributes().StrongGuaranteeShare) + GetParent()->Attributes().TotalResourceFlowRatio;
    }

    if (parentGuaranteeDominantShare < NVectorHdrf::RatioComputationPrecision) {
        return 1.0;
    }

    return selfGuaranteeDominantShare *
        (*TreeConfig_->InferWeightFromGuaranteesShareMultiplier) /
        parentGuaranteeDominantShare;
}

TSchedulableAttributes& TSchedulerElement::Attributes()
{
    return Attributes_;
}

const TSchedulableAttributes& TSchedulerElement::Attributes() const
{
    return Attributes_;
}

const TJobResources& TSchedulerElement::GetResourceDemand() const
{
    return ResourceDemand_;
}

const TJobResources& TSchedulerElement::GetResourceUsageAtUpdate() const
{
    return ResourceUsageAtUpdate_;
}

const TJobResources& TSchedulerElement::GetResourceLimits() const
{
    return ResourceLimits_;
}

const NVectorHdrf::TJobResourcesConfig* TSchedulerElement::GetStrongGuaranteeResourcesConfig() const
{
    return nullptr;
}

TJobResources TSchedulerElement::GetSpecifiedStrongGuaranteeResources() const
{
    const auto* guaranteeConfig = GetStrongGuaranteeResourcesConfig();
    YT_VERIFY(guaranteeConfig);
    return NVectorHdrf::ToJobResources(*guaranteeConfig, {});
}

TSchedulerCompositeElement* TSchedulerElement::GetMutableParent()
{
    return Parent_;
}

const TSchedulerCompositeElement* TSchedulerElement::GetParent() const
{
    return Parent_;
}

NVectorHdrf::TElement* TSchedulerElement::GetParentElement() const
{
    return Parent_;
}

TInstant TSchedulerElement::GetStartTime() const
{
    return StartTime_;
}

int TSchedulerElement::GetPendingJobCount() const
{
    return PendingJobCount_;
}

ESchedulableStatus TSchedulerElement::GetStatus(bool /* atUpdate */) const
{
    return ESchedulableStatus::Normal;
}

EStarvationStatus TSchedulerElement::GetStarvationStatus() const
{
    return PersistentAttributes_.StarvationStatus;
}

void TSchedulerElement::SetStarvationStatus(EStarvationStatus starvationStatus)
{
    YT_VERIFY(Mutable_);

    PersistentAttributes_.StarvationStatus = starvationStatus;
}

bool TSchedulerElement::AreResourceLimitsViolated() const
{
    return ResourceTreeElement_->AreResourceLimitsViolated();
}

TJobResources TSchedulerElement::GetInstantResourceUsage() const
{
    auto resourceUsage = TreeConfig_->UseResourceUsageWithPrecommit
        ? ResourceTreeElement_->GetResourceUsageWithPrecommit()
        : ResourceTreeElement_->GetResourceUsage();
    if (resourceUsage.GetUserSlots() > 0 && resourceUsage.GetMemory() == 0) {
        YT_LOG_WARNING("Found usage of schedulable element with non-zero user slots and zero memory (ElementId: %v, Usage: %v)",
            GetId(),
            FormatResources(resourceUsage));
    }
    return resourceUsage;
}

double TSchedulerElement::GetMaxShareRatio() const
{
    return MaxComponent(GetMaxShare());
}

TResourceVector TSchedulerElement::GetResourceUsageShare() const
{
    return TResourceVector::FromJobResources(ResourceUsageAtUpdate_, TotalResourceLimits_);
}

double TSchedulerElement::GetResourceDominantUsageShareAtUpdate() const
{
    return MaxComponent(Attributes_.UsageShare);
}

TString TSchedulerElement::GetTreeId() const
{
    return TreeId_;
}

bool TSchedulerElement::CheckDemand(const TJobResources& delta)
{
    return ResourceTreeElement_->CheckDemand(delta, GetResourceDemand());
}

TJobResources TSchedulerElement::GetLocalAvailableResourceLimits(const TScheduleJobsContext& context) const
{
    if (HasSpecifiedResourceLimits_) {
        return ComputeAvailableResources(
            ResourceLimits_,
            ResourceTreeElement_->GetResourceUsageWithPrecommit(),
            context.GetLocalUnconditionalUsageDiscountFor(this));
    }
    return TJobResources::Infinite();
}

void TSchedulerElement::IncreaseHierarchicalResourceUsage(const TJobResources& delta)
{
    TreeElementHost_->GetResourceTree()->IncreaseHierarchicalResourceUsage(ResourceTreeElement_, delta);
}

TSchedulerElement::TSchedulerElement(
    ISchedulerStrategyHost* strategyHost,
    IFairShareTreeElementHost* treeElementHost,
    TFairShareStrategyTreeConfigPtr treeConfig,
    TString treeId,
    TString id,
    EResourceTreeElementKind elementKind,
    const NLogging::TLogger& logger)
    : TSchedulerElementFixedState(strategyHost, treeElementHost, std::move(treeConfig), std::move(treeId))
    , ResourceTreeElement_(New<TResourceTreeElement>(
        TreeElementHost_->GetResourceTree(),
        id,
        elementKind))
    , Logger(logger)
{
    if (id == RootPoolName) {
        ResourceTreeElement_->MarkInitialized();
    }
}

TSchedulerElement::TSchedulerElement(
    const TSchedulerElement& other,
    TSchedulerCompositeElement* clonedParent)
    : TSchedulerElementFixedState(other)
    , ResourceTreeElement_(other.ResourceTreeElement_)
    , Logger(other.Logger)
{
    Parent_ = clonedParent;
}

ISchedulerStrategyHost* TSchedulerElement::GetHost() const
{
    YT_VERIFY(Mutable_);

    return StrategyHost_;
}

double TSchedulerElement::ComputeLocalSatisfactionRatio(const TJobResources& resourceUsage) const
{
    const auto& fairShare = Attributes_.FairShare.Total;

    // Check for corner cases.
    if (Dominates(TResourceVector::SmallEpsilon(), fairShare)) {
        return InfiniteSatisfactionRatio;
    }

    auto usageShare = TResourceVector::FromJobResources(resourceUsage, TotalResourceLimits_);

    // Check if the element is over-satisfied.
    if (TResourceVector::Any(usageShare, fairShare, [] (double usage, double fair) { return usage > fair; })) {
        double satisfactionRatio = std::min(
            MaxComponent(
                Div(usageShare, fairShare, /* zeroDivByZero */ 0.0, /* oneDivByZero */ InfiniteSatisfactionRatio)),
            InfiniteSatisfactionRatio);
        YT_VERIFY(satisfactionRatio >= 1.0);
        return satisfactionRatio;
    }

    double satisfactionRatio = 0.0;
    if (AreAllResourcesBlocked()) {
        // NB(antonkikh): Using |MaxComponent| would lead to satisfaction ratio being non-monotonous.
        satisfactionRatio = MinComponent(Div(usageShare, fairShare, /* zeroDivByZero */ 1.0, /* oneDivByZero */ 1.0));
    } else {
        satisfactionRatio = 0.0;
        for (auto resourceType : TEnumTraits<EJobResourceType>::GetDomainValues()) {
            if (!IsResourceBlocked(resourceType) && fairShare[resourceType] != 0.0) {
                satisfactionRatio = std::max(satisfactionRatio, usageShare[resourceType] / fairShare[resourceType]);
            }
        }
    }

    YT_VERIFY(satisfactionRatio <= 1.0);
    return satisfactionRatio;
}

bool TSchedulerElement::IsResourceBlocked(EJobResourceType resource) const
{
    // Fair share may be slightly greater than demand share due to precision errors. See: YT-15359.
    return Attributes_.FairShare.Total[resource] >= Attributes_.DemandShare[resource];
}

bool TSchedulerElement::AreAllResourcesBlocked() const
{
    // Fair share may be slightly greater than demand share due to precision errors. See: YT-15359.
    return Dominates(Attributes_.FairShare.Total, Attributes_.DemandShare);
}

// Returns true either if there are non-blocked resources and for any such resource |r|: |lhs[r] > rhs[r]|
// or if all resources are blocked and there is at least one resource |r|: |lhs[r] > rhs[r]|.
// Note that this relation is neither reflective nor irreflective and cannot be used for sorting.
//
// This relation is monotonous in several aspects:
// * First argument monotonicity:
//      If |Dominates(vec2, vec1)| and |IsStrictlyDominatesNonBlocked(vec1, rhs)|,
//      then |IsStrictlyDominatesNonBlocked(vec2, rhs)|.
// * Second argument monotonicity:
//      If |Dominates(vec1, vec2)| and |IsStrictlyDominatesNonBlocked(lhs, vec1)|,
//      then |IsStrictlyDominatesNonBlocked(lsh, vec2)|.
// * Blocked resources monotonicity:
//      If |IsStrictlyDominatesNonBlocked(vec, rhs)| and the set of blocked resources increases,
//      then |IsStrictlyDominatesNonBlocked(vec, rhs)|.
// These properties are important for sensible scheduling.
bool TSchedulerElement::IsStrictlyDominatesNonBlocked(const TResourceVector& lhs, const TResourceVector& rhs) const
{
    if (AreAllResourcesBlocked()) {
        return TResourceVector::Any(lhs, rhs, [] (double x, double y) { return x > y; });
    }

    for (size_t i = 0; i < TResourceVector::Size; i++) {
        if (!IsResourceBlocked(TResourceVector::GetResourceTypeById(i)) && lhs[i] <= rhs[i]) {
            return false;
        }
    }

    return true;
}

ESchedulableStatus TSchedulerElement::GetStatusImpl(double tolerance, bool atUpdate) const
{
    auto usageShare = atUpdate
        ? Attributes_.UsageShare
        : GetResourceUsageShare();

    if (Dominates(Attributes_.FairShare.Total + TResourceVector::Epsilon(), Attributes_.DemandShare)) {
        tolerance = 1.0;
    }

    // Fair share may be slightly greater than demand share due to precision errors. See: YT-15359.
    auto adjustedFairShareBound = TResourceVector::Min(Attributes_.FairShare.Total * tolerance, Attributes_.DemandShare);
    if (IsStrictlyDominatesNonBlocked(adjustedFairShareBound, usageShare)) {
        return ESchedulableStatus::BelowFairShare;
    }

    return ESchedulableStatus::Normal;
}

void TSchedulerElement::CheckForStarvationImpl(
    TDuration fairShareStarvationTimeout,
    TDuration fairShareAggressiveStarvationTimeout,
    TInstant now)
{
    YT_VERIFY(Mutable_);

    auto status = GetStatus();
    switch (status) {
        case ESchedulableStatus::BelowFairShare:
            if (!PersistentAttributes_.BelowFairShareSince) {
                PersistentAttributes_.BelowFairShareSince = now;
            } else if (EffectiveAggressiveStarvationEnabled_ && now > *PersistentAttributes_.BelowFairShareSince + fairShareAggressiveStarvationTimeout) {
                SetStarvationStatus(EStarvationStatus::AggressivelyStarving);
            } else if (now > *PersistentAttributes_.BelowFairShareSince + fairShareStarvationTimeout) {
                SetStarvationStatus(EStarvationStatus::Starving);
            }
            break;

        case ESchedulableStatus::Normal:
            PersistentAttributes_.BelowFairShareSince = std::nullopt;
            SetStarvationStatus(EStarvationStatus::NonStarving);
            break;

        default:
            YT_ABORT();
    }
}

void TSchedulerElement::SetOperationAlert(
    TOperationId operationId,
    EOperationAlertType alertType,
    const TError& alert,
    std::optional<TDuration> timeout)
{
    StrategyHost_->SetOperationAlert(operationId, alertType, alert, timeout);
}

TJobResources TSchedulerElement::ComputeResourceLimits() const
{
    return Min(Min(
        GetSpecifiedResourceLimits(),
        GetSchedulingTagFilterResourceLimits()),
        GetMaxShareResourceLimits());
}

TJobResources TSchedulerElement::ComputeSchedulingTagFilterResourceLimits() const
{
    // Shortcut: if the scheduling tag filter is empty then we just use the resource limits for
    // the tree's nodes filter, which were computed earlier in PreUpdateBottomUp.
    if (GetSchedulingTagFilter() == EmptySchedulingTagFilter) {
        return TotalResourceLimits_;
    }

    auto connectionTime = InstantToCpuInstant(StrategyHost_->GetConnectionTime());
    auto delay = DurationToCpuDuration(TreeConfig_->TotalResourceLimitsConsiderDelay);
    if (GetCpuInstant() < connectionTime + delay) {
        // Return infinity during the cluster startup.
        return TJobResources::Infinite();
    } else {
        return GetHost()->GetResourceLimits(TreeConfig_->NodesFilter & GetSchedulingTagFilter());
    }
}

TJobResources TSchedulerElement::GetSchedulingTagFilterResourceLimits() const
{
    return SchedulingTagFilterResourceLimits_;
}

TJobResources TSchedulerElement::GetTotalResourceLimits() const
{
    return TotalResourceLimits_;
}

TJobResources TSchedulerElement::GetMaxShareResourceLimits() const
{
    return GetTotalResourceLimits() * GetMaxShare();
}

void TSchedulerElement::BuildResourceMetering(
    const std::optional<TMeteringKey>& /*key*/,
    const THashMap<TString, TResourceVolume>& /*poolResourceUsages*/,
    TMeteringMap* /*statistics*/) const
{ }

double TSchedulerElement::GetAccumulatedResourceRatioVolume() const
{
    return PersistentAttributes_.IntegralResourcesState.AccumulatedVolume.GetMinResourceRatio(TotalResourceLimits_);
}

TResourceVolume TSchedulerElement::GetAccumulatedResourceVolume() const
{
    return PersistentAttributes_.IntegralResourcesState.AccumulatedVolume;
}

void TSchedulerElement::InitAccumulatedResourceVolume(TResourceVolume resourceVolume)
{
    YT_VERIFY(PersistentAttributes_.IntegralResourcesState.AccumulatedVolume == TResourceVolume());
    PersistentAttributes_.IntegralResourcesState.AccumulatedVolume = resourceVolume;
}

bool TSchedulerElement::AreDetailedLogsEnabled() const
{
    return false;
}

void TSchedulerElement::UpdateStarvationAttributes(TInstant now, bool enablePoolStarvation)
{
    YT_VERIFY(Mutable_);

    if (enablePoolStarvation || IsOperation()) {
        CheckForStarvation(now);
    }

    if (Parent_) {
        LowestStarvingAncestor_ = GetStarvationStatus() != EStarvationStatus::NonStarving
            ? this
            : Parent_->GetLowestStarvingAncestor();
        LowestAggressivelyStarvingAncestor_ = GetStarvationStatus() == EStarvationStatus::AggressivelyStarving
            ? this
            : Parent_->GetLowestAggressivelyStarvingAncestor();
    } else { // Root case
        LowestStarvingAncestor_ = nullptr;
        LowestAggressivelyStarvingAncestor_ = nullptr;
    }
}

////////////////////////////////////////////////////////////////////////////////

TSchedulerCompositeElement::TSchedulerCompositeElement(
    ISchedulerStrategyHost* strategyHost,
    IFairShareTreeElementHost* treeElementHost,
    TFairShareStrategyTreeConfigPtr treeConfig,
    const TString& treeId,
    const TString& id,
    EResourceTreeElementKind elementKind,
    const NLogging::TLogger& logger)
    : TSchedulerElement(strategyHost, treeElementHost, std::move(treeConfig), treeId, id, elementKind, logger)
{ }

TSchedulerCompositeElement::TSchedulerCompositeElement(
    const TSchedulerCompositeElement& other,
    TSchedulerCompositeElement* clonedParent)
    : TSchedulerElement(other, clonedParent)
    , TSchedulerCompositeElementFixedState(other)
{
    auto cloneChildren = [&] (
        const std::vector<TSchedulerElementPtr>& list,
        THashMap<TSchedulerElementPtr, int>* clonedMap,
        std::vector<TSchedulerElementPtr>* clonedList)
    {
        for (const auto& child : list) {
            auto childClone = child->Clone(this);
            clonedList->push_back(childClone);
            YT_VERIFY(clonedMap->emplace(childClone, clonedList->size() - 1).second);
        }
    };
    cloneChildren(other.EnabledChildren_, &EnabledChildToIndex_, &EnabledChildren_);
    cloneChildren(other.DisabledChildren_, &DisabledChildToIndex_, &DisabledChildren_);
}

void TSchedulerCompositeElement::MarkImmutable()
{
    TSchedulerElement::MarkImmutable();
    for (const auto& child : EnabledChildren_) {
        child->MarkImmutable();
    }
    for (const auto& child : DisabledChildren_) {
        child->MarkImmutable();
    }
}

int TSchedulerCompositeElement::EnumerateElements(int startIndex, bool isSchedulableValueFilter)
{
    YT_VERIFY(Mutable_);

    startIndex = TSchedulerElement::EnumerateElements(startIndex, isSchedulableValueFilter);
    for (const auto& child : EnabledChildren_) {
        startIndex = child->EnumerateElements(startIndex, isSchedulableValueFilter);
    }
    return startIndex;
}

void TSchedulerCompositeElement::DisableNonAliveElements()
{
    std::vector<TSchedulerElementPtr> childrenToDisable;
    for (const auto& child : EnabledChildren_) {
        if (!child->IsAlive()) {
            childrenToDisable.push_back(child);
        }
    }
    for (const auto& child : childrenToDisable) {
        DisableChild(child);
    }
    for (const auto& child : EnabledChildren_) {
        child->DisableNonAliveElements();
    }
}

void TSchedulerCompositeElement::UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config)
{
    YT_VERIFY(Mutable_);

    TSchedulerElement::UpdateTreeConfig(config);

    auto updateChildrenConfig = [&config] (TChildList& list) {
        for (const auto& child : list) {
            child->UpdateTreeConfig(config);
        }
    };

    updateChildrenConfig(EnabledChildren_);
    updateChildrenConfig(DisabledChildren_);
}

void TSchedulerCompositeElement::PreUpdateBottomUp(NVectorHdrf::TFairShareUpdateContext* context)
{
    YT_VERIFY(Mutable_);

    ResourceUsageAtUpdate_ = {};
    ResourceDemand_ = {};

    for (const auto& child : EnabledChildren_) {
        child->PreUpdateBottomUp(context);

        ResourceUsageAtUpdate_ += child->GetResourceUsageAtUpdate();
        ResourceDemand_ += child->GetResourceDemand();
        PendingJobCount_ += child->GetPendingJobCount();

        if (IsInferringChildrenWeightsFromHistoricUsageEnabled()) {
            // NB(eshcherbin): This is a lazy parameters update so it has to be done every time.
            child->PersistentAttributes_.HistoricUsageAggregator.UpdateParameters(
                GetHistoricUsageAggregationParameters());

            // TODO(eshcherbin): Should we use vectors instead of ratios?
            // Yes, but nobody uses this feature yet, so it's not really important.
            auto usageRatio = MaxComponent(child->GetResourceUsageShare());
            child->PersistentAttributes_.HistoricUsageAggregator.UpdateAt(context->Now, usageRatio);
        }
    }

    TSchedulerElement::PreUpdateBottomUp(context);
}

void TSchedulerCompositeElement::PublishFairShareAndUpdatePreemptionSettings()
{
    // This version is global and used to balance preemption lists.
    ResourceTreeElement_->SetFairShare(Attributes_.FairShare.Total);

    UpdatePreemptionAttributes();

    for (const auto& child : EnabledChildren_) {
        child->PublishFairShareAndUpdatePreemptionSettings();
    }
}

void TSchedulerCompositeElement::BuildSchedulableChildrenLists(TFairSharePostUpdateContext* context)
{
    Attributes_.UnschedulableOperationsResourceUsage = TJobResources();
    SchedulableChildren_.clear();
    for (const auto& child : EnabledChildren_) {
        child->BuildSchedulableChildrenLists(context);
        Attributes_.UnschedulableOperationsResourceUsage += child->Attributes().UnschedulableOperationsResourceUsage;
        if (child->IsSchedulable()) {
            SchedulableChildren_.push_back(child);
        }
    }
}

void TSchedulerCompositeElement::UpdateSchedulableAttributesFromDynamicAttributes(
    TDynamicAttributesList* dynamicAttributesList,
    TChildHeapMap* childHeapMap)
{
    for (const auto& child : EnabledChildren_) {
        child->UpdateSchedulableAttributesFromDynamicAttributes(dynamicAttributesList, childHeapMap);
    }

    TSchedulerElement::UpdateSchedulableAttributesFromDynamicAttributes(dynamicAttributesList, childHeapMap);

    EmplaceOrCrash(
        *childHeapMap,
        GetTreeIndex(),
        TChildHeap{
            SchedulableChildren_,
            dynamicAttributesList,
            this,
            Mode_
        });
}

void TSchedulerCompositeElement::UpdateDynamicAttributes(
    TDynamicAttributesList* dynamicAttributesList,
    const TChildHeapMap& childHeapMap,
    bool checkLiveness)
{
    auto& attributes = dynamicAttributesList->AttributesOf(this);

    if (checkLiveness && !IsAlive()) {
        attributes.Active = false;
        return;
    }

    // Satisfaction ratio of a composite element is the minimum of its children's satisfaction ratios.
    // NB(eshcherbin): We initialize with local satisfaction ratio in case all children have no pending jobs
    // and thus are not in the |SchedulableChildren_| list.
    if (Mutable_) {
        attributes.SatisfactionRatio = ComputeLocalSatisfactionRatio(ResourceUsageAtUpdate_);
    } else {
        attributes.SatisfactionRatio = ComputeLocalSatisfactionRatio(attributes.ResourceUsage);
    }

    // Declare the element passive if all children are passive.
    attributes.Active = false;
    attributes.BestLeafDescendant = nullptr;

    while (auto bestChild = GetBestActiveChild(*dynamicAttributesList, childHeapMap)) {
        const auto& bestChildAttributes = dynamicAttributesList->AttributesOf(bestChild);
        auto childBestLeafDescendant = bestChildAttributes.BestLeafDescendant;
        if (checkLiveness && !childBestLeafDescendant->IsAlive()) {
            bestChild->UpdateDynamicAttributes(dynamicAttributesList, childHeapMap, checkLiveness);
            if (!bestChildAttributes.Active) {
                continue;
            }
            childBestLeafDescendant = bestChildAttributes.BestLeafDescendant;
        }

        attributes.SatisfactionRatio = std::min(bestChildAttributes.SatisfactionRatio, attributes.SatisfactionRatio);
        attributes.BestLeafDescendant = childBestLeafDescendant;
        attributes.Active = true;
        break;
    }
}

void TSchedulerCompositeElement::BuildElementMapping(TFairSharePostUpdateContext* context)
{
    for (const auto& child : EnabledChildren_) {
        child->BuildElementMapping(context);
    }
    for (const auto& child : DisabledChildren_) {
        if (child->IsOperation()) {
            child->BuildElementMapping(context);
        }
    }
}

void TSchedulerCompositeElement::IncreaseOperationCount(int delta)
{
    OperationCount_ += delta;

    auto parent = GetMutableParent();
    while (parent) {
        parent->OperationCount() += delta;
        parent = parent->GetMutableParent();
    }
}

void TSchedulerCompositeElement::IncreaseRunningOperationCount(int delta)
{
    RunningOperationCount_ += delta;

    auto parent = GetMutableParent();
    while (parent) {
        parent->RunningOperationCount() += delta;
        parent = parent->GetMutableParent();
    }
}

const TJobResources& TSchedulerCompositeElement::FillResourceUsageInDynamicAttributes(
    TDynamicAttributesList* attributesList,
    const TResourceUsageSnapshotPtr& resourceUsageSnapshot)
{
    auto& attributes = attributesList->AttributesOf(this);

    attributes.ResourceUsage = Attributes_.UnschedulableOperationsResourceUsage;
    for (const auto& child : SchedulableChildren_) {
        attributes.ResourceUsage += child->FillResourceUsageInDynamicAttributes(attributesList, resourceUsageSnapshot);
    }

    return attributes.ResourceUsage;
}

void TSchedulerCompositeElement::PrescheduleJob(TScheduleJobsContext* context, EOperationPreemptionPriority targetOperationPreemptionPriority)
{
    auto& attributes = context->DynamicAttributesFor(this);

    if (!IsAlive()) {
        ++context->StageState()->DeactivationReasons[EDeactivationReason::IsNotAlive];
        YT_VERIFY(!attributes.Active);
        return;
    }

    if (TreeConfig_->EnableSchedulingTags &&
        SchedulingTagFilterIndex_ != EmptySchedulingTagFilterIndex &&
        !context->CanSchedule()[SchedulingTagFilterIndex_])
    {
        ++context->StageState()->DeactivationReasons[EDeactivationReason::UnmatchedSchedulingTag];
        YT_VERIFY(!attributes.Active);
        return;
    }

    for (const auto& child : SchedulableChildren_) {
        child->PrescheduleJob(context, targetOperationPreemptionPriority);
    }

    UpdateDynamicAttributes(&context->DynamicAttributesList(), context->ChildHeapMap(), /*checkLiveness*/ true);

    InitializeChildHeap(context);

    if (attributes.Active) {
        ++context->StageState()->ActiveTreeSize;
    }
}

bool TSchedulerCompositeElement::IsSchedulable() const
{
    return IsRoot() || !SchedulableChildren_.empty();
}

bool TSchedulerCompositeElement::HasAggressivelyStarvingElements(TScheduleJobsContext* context) const
{
    if (PersistentAttributes_.StarvationStatus == EStarvationStatus::AggressivelyStarving) {
        return true;
    }

    for (const auto& child : SchedulableChildren_) {
        if (child->HasAggressivelyStarvingElements(context)) {
            return true;
        }
    }

    return false;
}

void TSchedulerCompositeElement::PrepareConditionalUsageDiscounts(
    TScheduleJobsContext* context,
    EOperationPreemptionPriority targetOperationPreemptionPriority) const
{
    TJobResources deltaConditionalDiscount;
    for (const auto& jobInfo : context->GetConditionallyPreemptableJobsInPool(this)) {
        deltaConditionalDiscount += jobInfo.Job->ResourceUsage();
    }

    context->CurrentConditionalDiscount() += deltaConditionalDiscount;

    for (const auto& child : SchedulableChildren_) {
        child->PrepareConditionalUsageDiscounts(context, targetOperationPreemptionPriority);
    }

    context->CurrentConditionalDiscount() -= deltaConditionalDiscount;
}

void TSchedulerCompositeElement::CountOperationsByPreemptionPriority(TScheduleJobsContext* context) const
{
    for (const auto& child : SchedulableChildren_) {
        child->CountOperationsByPreemptionPriority(context);
    }
}

TFairShareScheduleJobResult TSchedulerCompositeElement::ScheduleJob(TScheduleJobsContext* context, bool ignorePacking)
{
    auto& attributes = context->DynamicAttributesFor(this);

    TSchedulerOperationElement* bestLeafDescendant = nullptr;

    while (bestLeafDescendant == nullptr) {
        if (!attributes.Active) {
            return TFairShareScheduleJobResult(/* finished */ true, /* scheduled */ false);
        }

        bestLeafDescendant = attributes.BestLeafDescendant;
        if (!bestLeafDescendant->IsAlive()) {
            context->DynamicAttributesFor(bestLeafDescendant).Active = false;
            UpdateDynamicAttributes(&context->DynamicAttributesList(), context->ChildHeapMap(), /*checkLiveness*/ true);
            bestLeafDescendant = nullptr;
            continue;
        }
        if (bestLeafDescendant->IsUsageOutdated(context)) {
            bestLeafDescendant->UpdateCurrentResourceUsage(context);
            bestLeafDescendant = nullptr;
            continue;
        }
    }

    auto childResult = bestLeafDescendant->ScheduleJob(context, ignorePacking);
    return TFairShareScheduleJobResult(/* finished */ false, /* scheduled */ childResult.Scheduled);
}

bool TSchedulerCompositeElement::IsExplicit() const
{
    return false;
}

void TSchedulerCompositeElement::AddChild(TSchedulerElement* child, bool enabled)
{
    YT_VERIFY(Mutable_);

    if (enabled) {
        child->PersistentAttributes_.ResetOnElementEnabled();
    }

    auto& map = enabled ? EnabledChildToIndex_ : DisabledChildToIndex_;
    auto& list = enabled ? EnabledChildren_ : DisabledChildren_;
    AddChild(&map, &list, child);
}

void TSchedulerCompositeElement::EnableChild(const TSchedulerElementPtr& child)
{
    YT_VERIFY(Mutable_);

    child->PersistentAttributes_.ResetOnElementEnabled();

    RemoveChild(&DisabledChildToIndex_, &DisabledChildren_, child);
    AddChild(&EnabledChildToIndex_, &EnabledChildren_, child);
}

void TSchedulerCompositeElement::DisableChild(const TSchedulerElementPtr& child)
{
    YT_VERIFY(Mutable_);

    if (EnabledChildToIndex_.find(child) == EnabledChildToIndex_.end()) {
        return;
    }

    RemoveChild(&EnabledChildToIndex_, &EnabledChildren_, child);
    AddChild(&DisabledChildToIndex_, &DisabledChildren_, child);
}

void TSchedulerCompositeElement::RemoveChild(TSchedulerElement* child)
{
    YT_VERIFY(Mutable_);

    bool enabled = ContainsChild(EnabledChildToIndex_, child);
    auto& map = enabled ? EnabledChildToIndex_ : DisabledChildToIndex_;
    auto& list = enabled ? EnabledChildren_ : DisabledChildren_;
    RemoveChild(&map, &list, child);
}

bool TSchedulerCompositeElement::IsEnabledChild(TSchedulerElement* child)
{
    return ContainsChild(EnabledChildToIndex_, child);
}

bool TSchedulerCompositeElement::IsEmpty() const
{
    return EnabledChildren_.empty() && DisabledChildren_.empty();
}

void TSchedulerCompositeElement::CollectOperationSchedulingSegmentContexts(
    THashMap<TOperationId, TOperationSchedulingSegmentContext>* operationContexts) const
{
    for (const auto& child : EnabledChildren_) {
        child->CollectOperationSchedulingSegmentContexts(operationContexts);
    }
}

void TSchedulerCompositeElement::ApplyOperationSchedulingSegmentChanges(
    const THashMap<TOperationId, TOperationSchedulingSegmentContext>& operationContexts)
{
    for (const auto& child : EnabledChildren_) {
        child->ApplyOperationSchedulingSegmentChanges(operationContexts);
    }
}

void TSchedulerCompositeElement::CollectResourceTreeOperationElements(std::vector<TResourceTreeElementPtr>* elements) const
{
    for (const auto& child : EnabledChildren_) {
        child->CollectResourceTreeOperationElements(elements);
    }
}

NVectorHdrf::TElement* TSchedulerCompositeElement::GetChild(int index)
{
    return EnabledChildren_[index].Get();
}

const NVectorHdrf::TElement* TSchedulerCompositeElement::GetChild(int index) const
{
    return EnabledChildren_[index].Get();
}

int TSchedulerCompositeElement::GetChildrenCount() const
{
    return EnabledChildren_.size();
}

std::vector<TSchedulerOperationElement*> TSchedulerCompositeElement::GetChildOperations() const
{
    std::vector<TSchedulerOperationElement*> result;
    result.reserve(std::size(EnabledChildren_) + std::size(DisabledChildren_));

    for (const auto& child : EnabledChildren_) {
        if (child->IsOperation()) {
            result.push_back(static_cast<TSchedulerOperationElement*>(child.Get()));
        }
    }
    for (const auto& child : DisabledChildren_) {
        if (child->IsOperation()) {
            result.push_back(static_cast<TSchedulerOperationElement*>(child.Get()));
        }
    }

    return result;
}

int TSchedulerCompositeElement::GetChildOperationCount() const noexcept
{
    int count = 0;

    for (const auto& child : EnabledChildren_) {
        if (child->IsOperation()) {
            ++count;
        }
    }
    for (const auto& child : DisabledChildren_) {
        if (child->IsOperation()) {
            ++count;
        }
    }

    return count;
}


ESchedulingMode TSchedulerCompositeElement::GetMode() const
{
    return Mode_;
}

bool TSchedulerCompositeElement::HasHigherPriorityInFifoMode(const NVectorHdrf::TElement* lhs, const NVectorHdrf::TElement* rhs) const
{
    const auto* lhsElement = dynamic_cast<const TSchedulerElement*>(lhs);
    const auto* rhsElement = dynamic_cast<const TSchedulerElement*>(rhs);

    YT_VERIFY(lhsElement);
    YT_VERIFY(rhsElement);

    return HasHigherPriorityInFifoMode(lhsElement, rhsElement);
}

std::vector<TSchedulerElementPtr> TSchedulerCompositeElement::GetEnabledChildren()
{
    return EnabledChildren_;
}

std::vector<TSchedulerElementPtr> TSchedulerCompositeElement::GetDisabledChildren()
{
    return DisabledChildren_;
}

TSchedulerElement* TSchedulerCompositeElement::GetBestActiveChild(
    const TDynamicAttributesList& dynamicAttributesList,
    const TChildHeapMap& childHeapMap) const
{

    const auto& childHeapIt = childHeapMap.find(GetTreeIndex());
    if (childHeapIt != childHeapMap.end()) {
        const auto& childHeap = childHeapIt->second;
        auto* element = childHeap.GetTop();
        return dynamicAttributesList.AttributesOf(element).Active
            ? element
            : nullptr;
    }

    switch (Mode_) {
        case ESchedulingMode::Fifo:
            return GetBestActiveChildFifo(dynamicAttributesList);
        case ESchedulingMode::FairShare:
            return GetBestActiveChildFairShare(dynamicAttributesList);
        default:
            YT_ABORT();
    }
}

TSchedulerElement* TSchedulerCompositeElement::GetBestActiveChildFifo(const TDynamicAttributesList& dynamicAttributesList) const
{
    TSchedulerElement* bestChild = nullptr;
    for (const auto& child : SchedulableChildren_) {
        if (child->IsActive(dynamicAttributesList)) {
            if (bestChild && HasHigherPriorityInFifoMode(bestChild, child.Get())) {
                continue;
            }

            bestChild = child.Get();
        }
    }
    return bestChild;
}

TSchedulerElement* TSchedulerCompositeElement::GetBestActiveChildFairShare(const TDynamicAttributesList& dynamicAttributesList) const
{
    TSchedulerElement* bestChild = nullptr;
    double bestChildSatisfactionRatio = InfiniteSatisfactionRatio;
    for (const auto& child : SchedulableChildren_) {
        if (child->IsActive(dynamicAttributesList)) {
            double childSatisfactionRatio = dynamicAttributesList.AttributesOf(child.Get()).SatisfactionRatio;
            if (!bestChild || childSatisfactionRatio < bestChildSatisfactionRatio) {
                bestChild = child.Get();
                bestChildSatisfactionRatio = childSatisfactionRatio;
            }
        }
    }
    return bestChild;
}

void TSchedulerCompositeElement::AddChild(
    TChildMap* map,
    TChildList* list,
    const TSchedulerElementPtr& child)
{
    list->push_back(child);
    YT_VERIFY(map->emplace(child, list->size() - 1).second);
}

void TSchedulerCompositeElement::RemoveChild(
    TChildMap* map,
    TChildList* list,
    const TSchedulerElementPtr& child)
{
    auto it = map->find(child);
    YT_VERIFY(it != map->end());
    if (child == list->back()) {
        list->pop_back();
    } else {
        int index = it->second;
        std::swap((*list)[index], list->back());
        list->pop_back();
        (*map)[(*list)[index]] = index;
    }
    map->erase(it);
}

bool TSchedulerCompositeElement::ContainsChild(
    const TChildMap& map,
    const TSchedulerElementPtr& child)
{
    return map.find(child) != map.end();
}

bool TSchedulerCompositeElement::HasHigherPriorityInFifoMode(const TSchedulerElement* lhs, const TSchedulerElement* rhs) const
{
    for (auto parameter : FifoSortParameters_) {
        switch (parameter) {
            case EFifoSortParameter::Weight:
                if (lhs->GetWeight() != rhs->GetWeight()) {
                    return lhs->GetWeight() > rhs->GetWeight();
                }
                break;
            case EFifoSortParameter::StartTime: {
                const auto& lhsStartTime = lhs->GetStartTime();
                const auto& rhsStartTime = rhs->GetStartTime();
                if (lhsStartTime != rhsStartTime) {
                    return lhsStartTime < rhsStartTime;
                }
                break;
            }
            case EFifoSortParameter::PendingJobCount: {
                int lhsPendingJobCount = lhs->GetPendingJobCount();
                int rhsPendingJobCount = rhs->GetPendingJobCount();
                if (lhsPendingJobCount != rhsPendingJobCount) {
                    return lhsPendingJobCount < rhsPendingJobCount;
                }
                break;
            }
            default:
                YT_ABORT();
        }
    }
    return false;
}

int TSchedulerCompositeElement::GetAvailableRunningOperationCount() const
{
    return std::max(GetMaxRunningOperationCount() - RunningOperationCount_, 0);
}

TResourceVolume TSchedulerCompositeElement::GetIntegralPoolCapacity() const
{
    return TResourceVolume(TotalResourceLimits_ * Attributes_.ResourceFlowRatio, TreeConfig_->IntegralGuarantees->PoolCapacitySaturationPeriod);
}

void TSchedulerCompositeElement::InitializeChildHeap(TScheduleJobsContext* context)
{
    if (std::ssize(SchedulableChildren_) < TreeConfig_->MinChildHeapSize) {
        return;
    }

    context->StageState()->TotalHeapElementCount += std::ssize(SchedulableChildren_);

    context->ChildHeapMap().emplace(
        GetTreeIndex(),
        TChildHeap{
            SchedulableChildren_,
            &context->DynamicAttributesList(),
            this,
            Mode_
        });
}

void TSchedulerCompositeElement::UpdateChild(TChildHeapMap& childHeapMap, TSchedulerElement* child)
{
    auto it = childHeapMap.find(GetTreeIndex());
    if (it != childHeapMap.end()) {
        auto& childHeap = it->second;
        childHeap.Update(child);
    }
}

void TSchedulerCompositeElement::UpdateStarvationAttributes(TInstant now, bool enablePoolStarvation)
{
    YT_VERIFY(Mutable_);

    TSchedulerElement::UpdateStarvationAttributes(now, enablePoolStarvation);

    for (const auto& child : EnabledChildren_) {
        child->UpdateStarvationAttributes(now, enablePoolStarvation);
    }
}

////////////////////////////////////////////////////////////////////////////////

TSchedulerPoolElementFixedState::TSchedulerPoolElementFixedState(TString id)
    : Id_(std::move(id))
{ }

////////////////////////////////////////////////////////////////////////////////

TSchedulerPoolElement::TSchedulerPoolElement(
    ISchedulerStrategyHost* strategyHost,
    IFairShareTreeElementHost* treeElementHost,
    const TString& id,
    TPoolConfigPtr config,
    bool defaultConfigured,
    TFairShareStrategyTreeConfigPtr treeConfig,
    const TString& treeId,
    const NLogging::TLogger& logger)
    : TSchedulerCompositeElement(
        strategyHost,
        treeElementHost,
        std::move(treeConfig),
        treeId,
        id,
        EResourceTreeElementKind::Pool,
        logger.WithTag("PoolId: %v, SchedulingMode: %v",
            id,
            config->Mode))
    , TSchedulerPoolElementFixedState(id)
{
    DoSetConfig(std::move(config));
    DefaultConfigured_ = defaultConfigured;
}

TSchedulerPoolElement::TSchedulerPoolElement(const TSchedulerPoolElement& other, TSchedulerCompositeElement* clonedParent)
    : TSchedulerCompositeElement(other, clonedParent)
    , TSchedulerPoolElementFixedState(other)
    , Config_(other.Config_)
{ }

bool TSchedulerPoolElement::IsDefaultConfigured() const
{
    return DefaultConfigured_;
}

bool TSchedulerPoolElement::IsEphemeralInDefaultParentPool() const
{
    return EphemeralInDefaultParentPool_;
}

void TSchedulerPoolElement::SetUserName(const std::optional<TString>& userName)
{
    UserName_ = userName;
}

const std::optional<TString>& TSchedulerPoolElement::GetUserName() const
{
    return UserName_;
}

TPoolConfigPtr TSchedulerPoolElement::GetConfig() const
{
    return Config_;
}

void TSchedulerPoolElement::SetConfig(TPoolConfigPtr config)
{
    YT_VERIFY(Mutable_);

    DoSetConfig(std::move(config));
    DefaultConfigured_ = false;
}

void TSchedulerPoolElement::SetDefaultConfig()
{
    YT_VERIFY(Mutable_);

    DoSetConfig(New<TPoolConfig>());
    DefaultConfigured_ = true;
}

void TSchedulerPoolElement::SetEphemeralInDefaultParentPool()
{
    YT_VERIFY(Mutable_);

    EphemeralInDefaultParentPool_ = true;
}

std::optional<bool> TSchedulerPoolElement::IsAggressivePreemptionAllowed() const
{
    return Config_->AllowAggressivePreemption;
}

bool TSchedulerPoolElement::IsExplicit() const
{
    // NB: This is no coincidence.
    return !DefaultConfigured_;
}

std::optional<bool> TSchedulerPoolElement::IsAggressiveStarvationEnabled() const
{
    return Config_->EnableAggressiveStarvation;
}

TString TSchedulerPoolElement::GetId() const
{
    return Id_;
}

std::optional<double> TSchedulerPoolElement::GetSpecifiedWeight() const
{
    return Config_->Weight;
}

const NVectorHdrf::TJobResourcesConfig* TSchedulerPoolElement::GetStrongGuaranteeResourcesConfig() const
{
    return Config_->StrongGuaranteeResources.Get();
}

TResourceVector TSchedulerPoolElement::GetMaxShare() const
{
    return TResourceVector::FromDouble(Config_->MaxShareRatio.value_or(1.0));
}

EIntegralGuaranteeType TSchedulerPoolElement::GetIntegralGuaranteeType() const
{
    return Config_->IntegralGuarantees->GuaranteeType;
}

const TIntegralResourcesState& TSchedulerPoolElement::IntegralResourcesState() const
{
    return PersistentAttributes_.IntegralResourcesState;
}

TIntegralResourcesState& TSchedulerPoolElement::IntegralResourcesState()
{
    return PersistentAttributes_.IntegralResourcesState;
}

ESchedulableStatus TSchedulerPoolElement::GetStatus(bool atUpdate) const
{
    return TSchedulerElement::GetStatusImpl(EffectiveFairShareStarvationTolerance_, atUpdate);
}

std::optional<double> TSchedulerPoolElement::GetSpecifiedFairShareStarvationTolerance() const
{
    return Config_->FairShareStarvationTolerance;
}

std::optional<TDuration> TSchedulerPoolElement::GetSpecifiedFairShareStarvationTimeout() const
{
    return Config_->FairShareStarvationTimeout;
}

void TSchedulerPoolElement::SetStarvationStatus(EStarvationStatus starvationStatus)
{
    YT_VERIFY(Mutable_);

    if (starvationStatus != GetStarvationStatus()) {
        YT_LOG_INFO("Pool starvation status changed (Current: %v, New: %v)",
            GetStarvationStatus(),
            starvationStatus);
    }
    TSchedulerElement::SetStarvationStatus(starvationStatus);
}

void TSchedulerPoolElement::CheckForStarvation(TInstant now)
{
    YT_VERIFY(Mutable_);

    TSchedulerElement::CheckForStarvationImpl(
        EffectiveFairShareStarvationTimeout_,
        TreeConfig_->FairShareAggressiveStarvationTimeout,
        now);
}

const TSchedulingTagFilter& TSchedulerPoolElement::GetSchedulingTagFilter() const
{
    return SchedulingTagFilter_;
}

int TSchedulerPoolElement::GetMaxRunningOperationCount() const
{
    return Config_->MaxRunningOperationCount.value_or(TreeConfig_->MaxRunningOperationCountPerPool);
}

int TSchedulerPoolElement::GetMaxOperationCount() const
{
    return Config_->MaxOperationCount.value_or(TreeConfig_->MaxOperationCountPerPool);
}

TPoolIntegralGuaranteesConfigPtr TSchedulerPoolElement::GetIntegralGuaranteesConfig() const
{
    return Config_->IntegralGuarantees;
}

std::vector<EFifoSortParameter> TSchedulerPoolElement::GetFifoSortParameters() const
{
    return FifoSortParameters_;
}

bool TSchedulerPoolElement::AreImmediateOperationsForbidden() const
{
    return Config_->ForbidImmediateOperations;
}

THashSet<TString> TSchedulerPoolElement::GetAllowedProfilingTags() const
{
    return Config_->AllowedProfilingTags;
}

bool TSchedulerPoolElement::IsFairShareTruncationInFifoPoolEnabled() const
{
    return Config_->EnableFairShareTruncationInFifoPool.value_or(
        TreeConfig_->EnableFairShareTruncationInFifoPool);
}

bool TSchedulerPoolElement::IsInferringChildrenWeightsFromHistoricUsageEnabled() const
{
    return Config_->InferChildrenWeightsFromHistoricUsage;
}

THistoricUsageAggregationParameters TSchedulerPoolElement::GetHistoricUsageAggregationParameters() const
{
    return THistoricUsageAggregationParameters(Config_->HistoricUsageConfig);
}

void TSchedulerPoolElement::BuildResourceMetering(
    const std::optional<TMeteringKey>& parentKey,
    const THashMap<TString, TResourceVolume>& poolResourceUsages,
    TMeteringMap* meteringMap) const
{
    std::optional<TMeteringKey> key;
    if (Config_->Abc) {
        key = TMeteringKey{
            .AbcId  = Config_->Abc->Id,
            .TreeId = GetTreeId(),
            .PoolId = GetId(),
            .MeteringTags = Config_->MeteringTags,
        };
    }

    YT_VERIFY(key || parentKey);

    bool isIntegral = Config_->IntegralGuarantees->GuaranteeType != EIntegralGuaranteeType::None;

    TResourceVolume accumulatedResourceUsageVolume;
    {
        auto it = poolResourceUsages.find(GetId());
        if (it != poolResourceUsages.end()) {
            accumulatedResourceUsageVolume = it->second;
        }
    }

    auto meteringStatistics = TMeteringStatistics(
        GetSpecifiedStrongGuaranteeResources(),
        isIntegral ? ToJobResources(Config_->IntegralGuarantees->ResourceFlow, {}) : TJobResources(),
        isIntegral ? ToJobResources(Config_->IntegralGuarantees->BurstGuaranteeResources, {}) : TJobResources(),
        GetResourceUsageAtUpdate(),
        accumulatedResourceUsageVolume);

    if (key) {
        auto insertResult = meteringMap->insert({*key, meteringStatistics});
        YT_VERIFY(insertResult.second);
    } else {
        meteringMap->at(*parentKey).AccountChild(meteringStatistics);
    }

    for (const auto& child : EnabledChildren_) {
        child->BuildResourceMetering(
            /*parentKey*/ key ? key : parentKey,
            poolResourceUsages,
            meteringMap);
    }

    if (key && parentKey) {
        meteringMap->at(*parentKey).DiscountChild(meteringStatistics);
    }
}

TSchedulerElementPtr TSchedulerPoolElement::Clone(TSchedulerCompositeElement* clonedParent)
{
    return New<TSchedulerPoolElement>(*this, clonedParent);
}

void TSchedulerPoolElement::AttachParent(TSchedulerCompositeElement* parent)
{
    YT_VERIFY(Mutable_);
    YT_VERIFY(!Parent_);
    YT_VERIFY(RunningOperationCount_ == 0);
    YT_VERIFY(OperationCount_ == 0);

    parent->AddChild(this);
    Parent_ = parent;
    TreeElementHost_->GetResourceTree()->AttachParent(ResourceTreeElement_, parent->ResourceTreeElement_);

    YT_LOG_DEBUG("Pool %Qv is attached to pool %Qv",
        Id_,
        parent->GetId());
}

const TSchedulerCompositeElement* TSchedulerPoolElement::GetNearestAncestorWithResourceLimits(const TSchedulerCompositeElement* element) const
{
    do {
        if (element->PersistentAttributes().AppliedResourceLimits != TJobResources::Infinite()) {
            return element;
        }
    } while (element = element->GetParent());

    return nullptr;
}

void TSchedulerPoolElement::ChangeParent(TSchedulerCompositeElement* newParent)
{
    YT_VERIFY(Mutable_);
    YT_VERIFY(Parent_);
    YT_VERIFY(newParent);
    YT_VERIFY(Parent_ != newParent);

    auto oldParent = Parent_;
    bool enabled = Parent_->IsEnabledChild(this);

    Parent_->IncreaseOperationCount(-OperationCount());
    Parent_->IncreaseRunningOperationCount(-RunningOperationCount());
    Parent_->RemoveChild(this);

    Parent_ = newParent;

    auto* sourceAncestorWithResourceLimits = GetNearestAncestorWithResourceLimits(oldParent);
    auto* destinationAncestorWithResourceLimits = GetNearestAncestorWithResourceLimits(newParent);

    if (PersistentAttributes_.AppliedResourceLimits == TJobResources::Infinite() &&
        sourceAncestorWithResourceLimits != destinationAncestorWithResourceLimits)
    {
        std::vector<TResourceTreeElementPtr> descendantOperationElements;
        CollectResourceTreeOperationElements(&descendantOperationElements);

        TreeElementHost_->GetResourceTree()->ChangeParent(
            ResourceTreeElement_,
            newParent->ResourceTreeElement_,
            descendantOperationElements);
    } else {
        TreeElementHost_->GetResourceTree()->ChangeParent(
            ResourceTreeElement_,
            newParent->ResourceTreeElement_,
            /*descendantOperationElements*/ std::nullopt);
    }

    Parent_->AddChild(this, enabled);
    Parent_->IncreaseOperationCount(OperationCount());
    Parent_->IncreaseRunningOperationCount(RunningOperationCount());

    YT_LOG_INFO("Parent pool is changed (NewParent: %v, OldParent: %v)",
        newParent->GetId(),
        oldParent->GetId());
}

void TSchedulerPoolElement::DetachParent()
{
    YT_VERIFY(Mutable_);
    YT_VERIFY(Parent_);
    YT_VERIFY(RunningOperationCount() == 0);
    YT_VERIFY(OperationCount() == 0);

    const auto& oldParentId = Parent_->GetId();
    Parent_->RemoveChild(this);
    TreeElementHost_->GetResourceTree()->ScheduleDetachParent(ResourceTreeElement_);

    YT_LOG_DEBUG("Pool is detached (Pool: %v, ParentPool: %v)",
        Id_,
        oldParentId);
}

void TSchedulerPoolElement::DoSetConfig(TPoolConfigPtr newConfig)
{
    YT_VERIFY(Mutable_);

    Config_ = std::move(newConfig);
    FifoSortParameters_ = Config_->FifoSortParameters;
    Mode_ = Config_->Mode;
    SchedulingTagFilter_ = TSchedulingTagFilter(Config_->SchedulingTagFilter);
}

TJobResources TSchedulerPoolElement::GetSpecifiedResourceLimits() const
{
    return ToJobResources(Config_->ResourceLimits, TJobResources::Infinite());
}

void TSchedulerPoolElement::BuildElementMapping(TFairSharePostUpdateContext* context)
{
    context->PoolNameToElement.emplace(GetId(), this);
    TSchedulerCompositeElement::BuildElementMapping(context);
}

double TSchedulerPoolElement::GetSpecifiedBurstRatio() const
{
    if (Config_->IntegralGuarantees->GuaranteeType == EIntegralGuaranteeType::None) {
        return 0;
    }
    return GetMaxResourceRatio(ToJobResources(Config_->IntegralGuarantees->BurstGuaranteeResources, {}), TotalResourceLimits_);
}

double TSchedulerPoolElement::GetSpecifiedResourceFlowRatio() const
{
    if (Config_->IntegralGuarantees->GuaranteeType == EIntegralGuaranteeType::None) {
        return 0;
    }
    return GetMaxResourceRatio(ToJobResources(Config_->IntegralGuarantees->ResourceFlow, {}), TotalResourceLimits_);
}

TResourceVector TSchedulerPoolElement::GetIntegralShareLimitForRelaxedPool() const
{
    YT_VERIFY(GetIntegralGuaranteeType() == EIntegralGuaranteeType::Relaxed);
    auto multiplier = Config_->IntegralGuarantees->RelaxedShareMultiplierLimit.value_or(TreeConfig_->IntegralGuarantees->RelaxedShareMultiplierLimit);
    return TResourceVector::FromDouble(Attributes_.ResourceFlowRatio) * multiplier;
}

bool TSchedulerPoolElement::CanAcceptFreeVolume() const
{
    return Config_->IntegralGuarantees->CanAcceptFreeVolume;
}

bool TSchedulerPoolElement::ShouldDistributeFreeVolumeAmongChildren() const
{
    return Config_->IntegralGuarantees->ShouldDistributeFreeVolumeAmongChildren.value_or(
        TreeConfig_->ShouldDistributeFreeVolumeAmongChildren);
}

bool TSchedulerPoolElement::AreDetailedLogsEnabled() const
{
    return Config_->EnableDetailedLogs;
}

////////////////////////////////////////////////////////////////////////////////

TSchedulerOperationElementFixedState::TSchedulerOperationElementFixedState(
    IOperationStrategyHost* operation,
    TFairShareStrategyOperationControllerConfigPtr controllerConfig,
    TSchedulingTagFilter schedulingTagFilter)
    : OperationId_(operation->GetId())
    , UnschedulableReason_(operation->CheckUnschedulable())
    , Operation_(operation)
    , ControllerConfig_(std::move(controllerConfig))
    , UserName_(operation->GetAuthenticatedUser())
    , SchedulingTagFilter_(std::move(schedulingTagFilter))
{ }

////////////////////////////////////////////////////////////////////////////////

TSchedulerOperationElementSharedState::TSchedulerOperationElementSharedState(
    ISchedulerStrategyHost* strategyHost,
    int updatePreemptableJobsListLoggingPeriod,
    const NLogging::TLogger& logger)
    : StrategyHost_(strategyHost)
    , UpdatePreemptableJobsListLoggingPeriod_(updatePreemptableJobsListLoggingPeriod)
    , Logger(logger)
{ }

TJobResources TSchedulerOperationElementSharedState::Disable()
{
    auto guard = WriterGuard(JobPropertiesMapLock_);

    Enabled_ = false;

    TJobResources resourceUsage;
    for (const auto& [jobId, properties] : JobPropertiesMap_) {
        resourceUsage += properties.ResourceUsage;
    }

    TotalResourceUsage_ = {};
    NonpreemptableResourceUsage_ = {};
    AggressivelyPreemptableResourceUsage_ = {};
    RunningJobCount_ = 0;
    PreemptableJobs_.clear();
    AggressivelyPreemptableJobs_.clear();
    NonpreemptableJobs_.clear();
    JobPropertiesMap_.clear();

    return resourceUsage;
}

void TSchedulerOperationElementSharedState::Enable()
{
    auto guard = WriterGuard(JobPropertiesMapLock_);

    YT_VERIFY(!Enabled_);
    Enabled_ = true;
}

bool TSchedulerOperationElementSharedState::Enabled()
{
    auto guard = ReaderGuard(JobPropertiesMapLock_);
    return Enabled_;
}

void TSchedulerOperationElementSharedState::RecordHeartbeat(
    const TPackingHeartbeatSnapshot& heartbeatSnapshot,
    const TFairShareStrategyPackingConfigPtr& packingConfig)
{
    HeartbeatStatistics_.RecordHeartbeat(heartbeatSnapshot, packingConfig);
}

bool TSchedulerOperationElementSharedState::CheckPacking(
    const TSchedulerOperationElement* operationElement,
    const TPackingHeartbeatSnapshot& heartbeatSnapshot,
    const TJobResourcesWithQuota& jobResources,
    const TJobResources& totalResourceLimits,
    const TFairShareStrategyPackingConfigPtr& packingConfig)
{
    return HeartbeatStatistics_.CheckPacking(
        operationElement,
        heartbeatSnapshot,
        jobResources,
        totalResourceLimits,
        packingConfig);
}

TJobResources TSchedulerOperationElementSharedState::SetJobResourceUsage(
    TJobId jobId,
    const TJobResources& resources)
{
    auto guard = WriterGuard(JobPropertiesMapLock_);

    if (!Enabled_) {
        return {};
    }

    return SetJobResourceUsage(GetJobProperties(jobId), resources);
}

void TSchedulerOperationElementSharedState::UpdatePreemptableJobsList(
    const TResourceVector& fairShare,
    const TJobResources& totalResourceLimits,
    double preemptionSatisfactionThreshold,
    double aggressivePreemptionSatisfactionThreshold,
    int* moveCount,
    TSchedulerOperationElement* operationElement)
{
    auto guard = WriterGuard(JobPropertiesMapLock_);

    auto getUsageShare = [&] (const TJobResources& resourceUsage) -> TResourceVector {
        return TResourceVector::FromJobResources(resourceUsage, totalResourceLimits);
    };

    auto balanceLists = [&] (
        TJobIdList* left,
        TJobIdList* right,
        TJobResources resourceUsage,
        const TResourceVector& fairShareBound,
        const std::function<void(TJobProperties*)>& onMovedLeftToRight,
        const std::function<void(TJobProperties*)>& onMovedRightToLeft)
    {
        // Move from left to right and decrease |resourceUsage| until the next move causes
        // |operationElement->IsStrictlyDominatesNonBlocked(fairShareBound, getUsageShare(nextUsage))| to become true.
        // In particular, even if fair share is slightly less than it should be due to precision errors,
        // we expect no problems, because the job which crosses the fair share boundary belongs to the left list.
        while (!left->empty()) {
            auto jobId = left->back();
            auto* jobProperties = GetJobProperties(jobId);

            auto nextUsage = resourceUsage - jobProperties->ResourceUsage;
            if (operationElement->IsStrictlyDominatesNonBlocked(fairShareBound, getUsageShare(nextUsage))) {
                break;
            }

            left->pop_back();
            right->push_front(jobId);
            jobProperties->JobIdListIterator = right->begin();
            onMovedLeftToRight(jobProperties);

            resourceUsage = nextUsage;
            ++(*moveCount);
        }

        // Move from right to left and increase |resourceUsage|.
        while (!right->empty() &&
            operationElement->IsStrictlyDominatesNonBlocked(fairShareBound, getUsageShare(resourceUsage)))
        {
            auto jobId = right->front();
            auto* jobProperties = GetJobProperties(jobId);

            right->pop_front();
            left->push_back(jobId);
            jobProperties->JobIdListIterator = --left->end();
            onMovedRightToLeft(jobProperties);

            resourceUsage += jobProperties->ResourceUsage;
            ++(*moveCount);
        }

        return resourceUsage;
    };

    auto setPreemptable = [] (TJobProperties* properties) {
        properties->PreemptionStatus = EJobPreemptionStatus::Preemptable;
    };

    auto setAggressivelyPreemptable = [] (TJobProperties* properties) {
        properties->PreemptionStatus = EJobPreemptionStatus::AggressivelyPreemptable;
    };

    auto setNonPreemptable = [] (TJobProperties* properties) {
        properties->PreemptionStatus = EJobPreemptionStatus::NonPreemptable;
    };

    bool enableLogging =
        (UpdatePreemptableJobsListCount_.fetch_add(1) % UpdatePreemptableJobsListLoggingPeriod_) == 0 ||
        operationElement->AreDetailedLogsEnabled();

    YT_LOG_DEBUG_IF(enableLogging,
        "Update preemptable lists inputs (FairShare: %.6g, TotalResourceLimits: %v, "
        "PreemptionSatisfactionThreshold: %v, AggressivePreemptionSatisfactionThreshold: %v)",
        fairShare,
        FormatResources(totalResourceLimits),
        preemptionSatisfactionThreshold,
        aggressivePreemptionSatisfactionThreshold);

    // NB: We need 2 iterations since thresholds may change significantly such that we need
    // to move job from preemptable list to non-preemptable list through aggressively preemptable list.
    for (int iteration = 0; iteration < 2; ++iteration) {
        YT_LOG_DEBUG_IF(enableLogging,
            "Preemptable lists usage bounds before update "
            "(NonpreemptableResourceUsage: %v, AggressivelyPreemptableResourceUsage: %v, PreemtableResourceUsage: %v, Iteration: %v)",
            FormatResources(NonpreemptableResourceUsage_),
            FormatResources(AggressivelyPreemptableResourceUsage_),
            FormatResources(TotalResourceUsage_ - NonpreemptableResourceUsage_ - AggressivelyPreemptableResourceUsage_),
            iteration);

        auto startNonPreemptableAndAggressivelyPreemptableResourceUsage_ = NonpreemptableResourceUsage_ + AggressivelyPreemptableResourceUsage_;

        NonpreemptableResourceUsage_ = balanceLists(
            &NonpreemptableJobs_,
            &AggressivelyPreemptableJobs_,
            NonpreemptableResourceUsage_,
            fairShare * aggressivePreemptionSatisfactionThreshold,
            setAggressivelyPreemptable,
            setNonPreemptable);

        auto nonpreemptableAndAggressivelyPreemptableResourceUsage_ = balanceLists(
            &AggressivelyPreemptableJobs_,
            &PreemptableJobs_,
            startNonPreemptableAndAggressivelyPreemptableResourceUsage_,
            Preemptable_ ? fairShare * preemptionSatisfactionThreshold : TResourceVector::Infinity(),
            setPreemptable,
            setAggressivelyPreemptable);

        AggressivelyPreemptableResourceUsage_ = nonpreemptableAndAggressivelyPreemptableResourceUsage_ - NonpreemptableResourceUsage_;
    }

    YT_LOG_DEBUG_IF(enableLogging,
        "Preemptable lists usage bounds after update "
        "(NonpreemptableResourceUsage: %v, AggressivelyPreemptableResourceUsage: %v, PreemtableResourceUsage: %v)",
        FormatResources(NonpreemptableResourceUsage_),
        FormatResources(AggressivelyPreemptableResourceUsage_),
        FormatResources(TotalResourceUsage_ - NonpreemptableResourceUsage_ - AggressivelyPreemptableResourceUsage_));
}

void TSchedulerOperationElementSharedState::SetPreemptable(bool value)
{
    Preemptable_.store(value);
}

bool TSchedulerOperationElementSharedState::GetPreemptable() const
{
    return Preemptable_;
}

bool TSchedulerOperationElementSharedState::IsJobKnown(TJobId jobId) const
{
    auto guard = ReaderGuard(JobPropertiesMapLock_);

    return JobPropertiesMap_.find(jobId) != JobPropertiesMap_.end();
}

EJobPreemptionStatus TSchedulerOperationElementSharedState::GetJobPreemptionStatus(TJobId jobId) const
{
    auto guard = ReaderGuard(JobPropertiesMapLock_);

    if (!Enabled_) {
        return EJobPreemptionStatus::NonPreemptable;
    }

    return GetJobProperties(jobId)->PreemptionStatus;
}

int TSchedulerOperationElementSharedState::GetRunningJobCount() const
{
    return RunningJobCount_;
}

int TSchedulerOperationElementSharedState::GetPreemptableJobCount() const
{
    auto guard = ReaderGuard(JobPropertiesMapLock_);

    return PreemptableJobs_.size();
}

int TSchedulerOperationElementSharedState::GetAggressivelyPreemptableJobCount() const
{
    auto guard = ReaderGuard(JobPropertiesMapLock_);

    return AggressivelyPreemptableJobs_.size();
}

bool TSchedulerOperationElementSharedState::AddJob(TJobId jobId, const TJobResources& resourceUsage, bool force)
{
    auto guard = WriterGuard(JobPropertiesMapLock_);

    if (!Enabled_ && !force) {
        return false;
    }

    LastScheduleJobSuccessTime_ = TInstant::Now();

    PreemptableJobs_.push_back(jobId);

    auto it = JobPropertiesMap_.emplace(
        jobId,
        TJobProperties{
            .PreemptionStatus = EJobPreemptionStatus::Preemptable,
            .JobIdListIterator = --PreemptableJobs_.end(),
            .ResourceUsage = {}});
    YT_VERIFY(it.second);

    ++RunningJobCount_;

    SetJobResourceUsage(&it.first->second, resourceUsage);

    return true;
}

void TSchedulerOperationElementSharedState::UpdatePreemptionStatusStatistics(EOperationPreemptionStatus status)
{
    auto guard = Guard(PreemptionStatusStatisticsLock_);

    PreemptionStatusStatistics_[status] += 1;
}

TPreemptionStatusStatisticsVector TSchedulerOperationElementSharedState::GetPreemptionStatusStatistics() const
{
    auto guard = Guard(PreemptionStatusStatisticsLock_);

    return PreemptionStatusStatistics_;
}

TJobPreemptionStatusMap TSchedulerOperationElementSharedState::GetJobPreemptionStatusMap() const
{
    TJobPreemptionStatusMap jobPreemptionStatuses;

    auto guard = ReaderGuard(JobPropertiesMapLock_);

    jobPreemptionStatuses.reserve(JobPropertiesMap_.size());
    for (const auto& [jobId, properties] : JobPropertiesMap_) {
        YT_VERIFY(jobPreemptionStatuses.emplace(jobId, properties.PreemptionStatus).second);
    }

    return jobPreemptionStatuses;
}

void TSchedulerOperationElementSharedState::OnMinNeededResourcesUnsatisfied(
    const TScheduleJobsContext& context,
    const TJobResources& availableResources,
    const TJobResources& minNeededResources)
{
    auto& shard = StateShards_[context.SchedulingContext()->GetNodeShardId()];
    #define XX(name, Name) \
        if (availableResources.Get##Name() < minNeededResources.Get##Name()) { \
            ++shard.MinNeededResourcesUnsatisfiedCountLocal[EJobResourceType::Name]; \
        }
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
}

TEnumIndexedVector<EJobResourceType, int> TSchedulerOperationElementSharedState::GetMinNeededResourcesUnsatisfiedCount()
{
    UpdateShardState();

    TEnumIndexedVector<EJobResourceType, int> result;
    for (const auto& shard : StateShards_) {
        for (auto resource : TEnumTraits<EJobResourceType>::GetDomainValues()) {
            result[resource] += shard.MinNeededResourcesUnsatisfiedCount[resource].load();
        }
    }
    return result;
}

void TSchedulerOperationElementSharedState::OnOperationDeactivated(const TScheduleJobsContext& context, EDeactivationReason reason)
{
    auto& shard = StateShards_[context.SchedulingContext()->GetNodeShardId()];
    ++shard.DeactivationReasonsLocal[reason];
    ++shard.DeactivationReasonsFromLastNonStarvingTimeLocal[reason];
}

TEnumIndexedVector<EDeactivationReason, int> TSchedulerOperationElementSharedState::GetDeactivationReasons()
{
    UpdateShardState();

    TEnumIndexedVector<EDeactivationReason, int> result;
    for (const auto& shard : StateShards_) {
        for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
            result[reason] += shard.DeactivationReasons[reason].load();
        }
    }
    return result;
}

TEnumIndexedVector<EDeactivationReason, int> TSchedulerOperationElementSharedState::GetDeactivationReasonsFromLastNonStarvingTime()
{
    UpdateShardState();

    TEnumIndexedVector<EDeactivationReason, int> result;
    for (const auto& shard : StateShards_) {
        for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
            result[reason] += shard.DeactivationReasonsFromLastNonStarvingTime[reason].load();
        }
    }
    return result;
}

void TSchedulerOperationElementSharedState::ResetDeactivationReasonsFromLastNonStarvingTime()
{
    int index = 0;
    for (const auto& invoker : StrategyHost_->GetNodeShardInvokers()) {
        invoker->Invoke(BIND([this, this_=MakeStrong(this), index] {
            auto& shard = StateShards_[index];
            for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
                shard.DeactivationReasonsFromLastNonStarvingTime[reason].store(0);
                shard.DeactivationReasonsFromLastNonStarvingTimeLocal[reason] = 0;
            }
        }));
        ++index;
    }
}

void TSchedulerOperationElementSharedState::UpdateShardState()
{
    auto now = TInstant::Now();
    if (now < LastStateShardsUpdateTime_ + UpdateStateShardsBackoff_) {
        return;
    }
    int index = 0;
    for (const auto& invoker : StrategyHost_->GetNodeShardInvokers()) {
        invoker->Invoke(BIND([this, this_=MakeStrong(this), index] {
            auto& shard = StateShards_[index];
            for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
                shard.DeactivationReasonsFromLastNonStarvingTime[reason].store(
                    shard.DeactivationReasonsFromLastNonStarvingTimeLocal[reason]);
                shard.DeactivationReasons[reason].store(shard.DeactivationReasonsLocal[reason]);
            }
            for (auto resource : TEnumTraits<EJobResourceType>::GetDomainValues()) {
                shard.MinNeededResourcesUnsatisfiedCount[resource].store(
                    shard.MinNeededResourcesUnsatisfiedCountLocal[resource]);
            }
        }));
        ++index;
    }
    LastStateShardsUpdateTime_ = now;
}

TInstant TSchedulerOperationElementSharedState::GetLastScheduleJobSuccessTime() const
{
    auto guard = ReaderGuard(JobPropertiesMapLock_);

    return LastScheduleJobSuccessTime_;
}

TEnumIndexedVector<EJobResourceType, int> TSchedulerOperationElement::GetMinNeededResourcesUnsatisfiedCount() const
{
    return OperationElementSharedState_->GetMinNeededResourcesUnsatisfiedCount();
}

void TSchedulerOperationElement::OnOperationDeactivated(TScheduleJobsContext* context, EDeactivationReason reason)
{
    ++context->StageState()->DeactivationReasons[reason];
    OperationElementSharedState_->OnOperationDeactivated(*context, reason);
}

TEnumIndexedVector<EDeactivationReason, int> TSchedulerOperationElement::GetDeactivationReasons() const
{
    return OperationElementSharedState_->GetDeactivationReasons();
}

TEnumIndexedVector<EDeactivationReason, int> TSchedulerOperationElement::GetDeactivationReasonsFromLastNonStarvingTime() const
{
    return OperationElementSharedState_->GetDeactivationReasonsFromLastNonStarvingTime();
}

std::optional<TString> TSchedulerOperationElement::GetCustomProfilingTag() const
{
    auto tagName = Spec_->CustomProfilingTag;
    if (!tagName) {
        return {};
    }

    if (!GetParent()) {
        return {};
    }

    THashSet<TString> allowedProfilingTags;
    const auto* parent = GetParent();
    while (parent) {
        for (const auto& tag : parent->GetAllowedProfilingTags()) {
            allowedProfilingTags.insert(tag);
        }
        parent = parent->GetParent();
    }

    if (allowedProfilingTags.find(*tagName) == allowedProfilingTags.end() ||
        (TreeConfig_->CustomProfilingTagFilter &&
         NRe2::TRe2::FullMatch(NRe2::StringPiece(*tagName), *TreeConfig_->CustomProfilingTagFilter)))
    {
        tagName = InvalidCustomProfilingTag;
    }

    return tagName;
}

void TSchedulerOperationElement::Disable(bool markAsNonAlive)
{
    YT_LOG_DEBUG("Operation element disabled in strategy");

    OperationElementSharedState_->Disable();
    TreeElementHost_->GetResourceTree()->ReleaseResources(ResourceTreeElement_, markAsNonAlive);
}

void TSchedulerOperationElement::Enable()
{
    YT_LOG_DEBUG("Operation element enabled in strategy");

    return OperationElementSharedState_->Enable();
}

std::optional<TJobResources> TSchedulerOperationElementSharedState::RemoveJob(TJobId jobId)
{
    auto guard = WriterGuard(JobPropertiesMapLock_);

    if (!Enabled_) {
        return std::nullopt;
    }

    auto it = JobPropertiesMap_.find(jobId);
    YT_VERIFY(it != JobPropertiesMap_.end());

    auto* properties = &it->second;
    switch (properties->PreemptionStatus) {
        case EJobPreemptionStatus::Preemptable:
            PreemptableJobs_.erase(properties->JobIdListIterator);
            break;
        case EJobPreemptionStatus::AggressivelyPreemptable:
            AggressivelyPreemptableJobs_.erase(properties->JobIdListIterator);
            break;
        case EJobPreemptionStatus::NonPreemptable:
            NonpreemptableJobs_.erase(properties->JobIdListIterator);
            break;
        default:
            YT_ABORT();
    }

    --RunningJobCount_;

    auto resourceUsage = properties->ResourceUsage;
    SetJobResourceUsage(properties, TJobResources());

    JobPropertiesMap_.erase(it);

    return resourceUsage;
}

std::optional<EDeactivationReason> TSchedulerOperationElement::TryStartScheduleJob(
    const TScheduleJobsContext& context,
    TJobResources* precommittedResourcesOutput,
    TJobResources* availableResourcesOutput)
{
    const auto& minNeededResources = AggregatedMinNeededJobResources_;

    // Do preliminary checks to avoid the overhead of updating and reverting precommit usage.
    if (!Dominates(GetHierarchicalAvailableResources(context), minNeededResources)) {
        return EDeactivationReason::ResourceLimitsExceeded;
    }
    if (!CheckDemand(minNeededResources)) {
        return EDeactivationReason::NoAvailableDemand;
    }

    TJobResources availableResourceLimits;
    auto increaseResult = TryIncreaseHierarchicalResourceUsagePrecommit(
        minNeededResources,
        &availableResourceLimits);

    if (increaseResult == EResourceTreeIncreaseResult::ResourceLimitExceeded) {
        return EDeactivationReason::ResourceLimitsExceeded;
    }
    if (increaseResult == EResourceTreeIncreaseResult::ElementIsNotAlive) {
        return EDeactivationReason::IsNotAlive;
    }

    Controller_->IncreaseConcurrentScheduleJobCalls(context.SchedulingContext()->GetNodeShardId());
    Controller_->IncreaseScheduleJobCallsSinceLastUpdate(context.SchedulingContext()->GetNodeShardId());

    *precommittedResourcesOutput = minNeededResources;
    *availableResourcesOutput = Min(
        availableResourceLimits,
        context.SchedulingContext()->GetNodeFreeResourcesWithDiscountForOperation(OperationId_));
    return std::nullopt;
}

void TSchedulerOperationElement::FinishScheduleJob(const ISchedulingContextPtr& schedulingContext)
{
    Controller_->DecreaseConcurrentScheduleJobCalls(schedulingContext->GetNodeShardId());
}

TJobResources TSchedulerOperationElementSharedState::SetJobResourceUsage(
    TJobProperties* properties,
    const TJobResources& resources)
{
    auto delta = resources - properties->ResourceUsage;
    properties->ResourceUsage = resources;
    TotalResourceUsage_ += delta;
    switch (properties->PreemptionStatus) {
        case EJobPreemptionStatus::Preemptable:
            // Do nothing.
            break;
        case EJobPreemptionStatus::AggressivelyPreemptable:
            AggressivelyPreemptableResourceUsage_ += delta;
            break;
        case EJobPreemptionStatus::NonPreemptable:
            NonpreemptableResourceUsage_ += delta;
            break;
        default:
            YT_ABORT();
    }

    return delta;
}

TSchedulerOperationElementSharedState::TJobProperties* TSchedulerOperationElementSharedState::GetJobProperties(TJobId jobId)
{
    auto it = JobPropertiesMap_.find(jobId);
    YT_ASSERT(it != JobPropertiesMap_.end());
    return &it->second;
}

const TSchedulerOperationElementSharedState::TJobProperties* TSchedulerOperationElementSharedState::GetJobProperties(TJobId jobId) const
{
    auto it = JobPropertiesMap_.find(jobId);
    YT_ASSERT(it != JobPropertiesMap_.end());
    return &it->second;
}

////////////////////////////////////////////////////////////////////////////////

TSchedulerOperationElement::TSchedulerOperationElement(
    TFairShareStrategyTreeConfigPtr treeConfig,
    TStrategyOperationSpecPtr spec,
    TOperationFairShareTreeRuntimeParametersPtr runtimeParameters,
    TFairShareStrategyOperationControllerPtr controller,
    TFairShareStrategyOperationControllerConfigPtr controllerConfig,
    ISchedulerStrategyHost* strategyHost,
    IFairShareTreeElementHost* treeElementHost,
    IOperationStrategyHost* operation,
    const TString& treeId,
    const NLogging::TLogger& logger)
    : TSchedulerElement(
        strategyHost,
        treeElementHost,
        std::move(treeConfig),
        treeId,
        ToString(operation->GetId()),
        EResourceTreeElementKind::Operation,
        logger.WithTag("OperationId: %v", operation->GetId()))
    , TSchedulerOperationElementFixedState(operation, std::move(controllerConfig), TSchedulingTagFilter(spec->SchedulingTagFilter))
    , RuntimeParameters_(std::move(runtimeParameters))
    , Spec_(std::move(spec))
    , OperationElementSharedState_(New<TSchedulerOperationElementSharedState>(
        strategyHost,
        Spec_->UpdatePreemptableJobsListLoggingPeriod,
        Logger))
    , Controller_(std::move(controller))
{ }

TSchedulerOperationElement::TSchedulerOperationElement(
    const TSchedulerOperationElement& other,
    TSchedulerCompositeElement* clonedParent)
    : TSchedulerElement(other, clonedParent)
    , TSchedulerOperationElementFixedState(other)
    , RuntimeParameters_(other.RuntimeParameters_)
    , Spec_(other.Spec_)
    , OperationElementSharedState_(other.OperationElementSharedState_)
    , Controller_(other.Controller_)
{ }

std::optional<double> TSchedulerOperationElement::GetSpecifiedFairShareStarvationTolerance() const
{
    return std::nullopt;
}

std::optional<TDuration> TSchedulerOperationElement::GetSpecifiedFairShareStarvationTimeout() const
{
    return std::nullopt;
}

void TSchedulerOperationElement::DisableNonAliveElements()
{ }

void TSchedulerOperationElement::PreUpdateBottomUp(NVectorHdrf::TFairShareUpdateContext* context)
{
    YT_VERIFY(Mutable_);

    PendingJobCount_ = Controller_->GetPendingJobCount();
    DetailedMinNeededJobResources_ = Controller_->GetDetailedMinNeededJobResources();
    AggregatedMinNeededJobResources_ = Controller_->GetAggregatedMinNeededJobResources();
    TotalNeededResources_ = Controller_->GetNeededResources();

    UnschedulableReason_ = ComputeUnschedulableReason();
    ResourceUsageAtUpdate_ = GetInstantResourceUsage();
    // Must be calculated after ResourceUsageAtUpdate_
    ResourceDemand_ = ComputeResourceDemand();
    Tentative_ = RuntimeParameters_->Tentative;
    StartTime_ = Operation_->GetStartTime();

    // NB: It was moved from regular fair share update for performing split.
    // It can be performed in fair share thread as second step of preupdate.
    if (PersistentAttributes_.LastBestAllocationRatioUpdateTime + TreeConfig_->BestAllocationRatioUpdatePeriod > context->Now) {
        auto allocationLimits = GetAdjustedResourceLimits(
            ResourceDemand_,
            TotalResourceLimits_,
            GetHost()->GetExecNodeMemoryDistribution(SchedulingTagFilter_ & TreeConfig_->NodesFilter));
        PersistentAttributes_.BestAllocationShare = TResourceVector::FromJobResources(allocationLimits, TotalResourceLimits_);
        PersistentAttributes_.LastBestAllocationRatioUpdateTime = context->Now;
    }

    for (const auto& jobResourcesWithQuota : DetailedMinNeededJobResources_) {
        for (auto [index, _] : jobResourcesWithQuota.GetDiskQuota().DiskSpacePerMedium) {
            DiskRequestMedia_.insert(index);
        }
    }

    TSchedulerElement::PreUpdateBottomUp(context);
}

void TSchedulerOperationElement::PublishFairShareAndUpdatePreemptionSettings()
{
    // This version is global and used to balance preemption lists.
    ResourceTreeElement_->SetFairShare(Attributes_.FairShare.Total);

    UpdatePreemptionAttributes();
}

void TSchedulerOperationElement::BuildSchedulableChildrenLists(TFairSharePostUpdateContext* context)
{
    if (!IsSchedulable()) {
        ++context->UnschedulableReasons[*UnschedulableReason_];
        Attributes_.UnschedulableOperationsResourceUsage = GetInstantResourceUsage();
    }
}

void TSchedulerOperationElement::UpdatePreemptionAttributes()
{
    YT_VERIFY(Mutable_);
    TSchedulerElement::UpdatePreemptionAttributes();

    // If fair share ratio equals demand ratio then we want to explicitly disable preemption.
    // It is necessary since some job's resource usage may increase before the next fair share update,
    //  and in this case we don't want any jobs to become preemptable
    bool isDominantFairShareEqualToDominantDemandShare =
        TResourceVector::Near(Attributes_.FairShare.Total, Attributes_.DemandShare, NVectorHdrf::RatioComparisonPrecision) &&
        !Dominates(TResourceVector::Epsilon(), Attributes_.DemandShare);

    bool newPreemptableValue = !isDominantFairShareEqualToDominantDemandShare;
    bool oldPreemptableValue = OperationElementSharedState_->GetPreemptable();
    if (oldPreemptableValue != newPreemptableValue) {
        YT_LOG_DEBUG("Preemptable status changed %v -> %v", oldPreemptableValue, newPreemptableValue);
        OperationElementSharedState_->SetPreemptable(newPreemptableValue);
    }

    UpdatePreemptableJobsList();
}

bool TSchedulerOperationElement::HasJobsSatisfyingResourceLimits(const TScheduleJobsContext& context) const
{
    for (const auto& jobResources : DetailedMinNeededJobResources_) {
        if (context.SchedulingContext()->CanStartJobForOperation(jobResources, OperationId_)) {
            return true;
        }
    }
    return false;
}

void TSchedulerOperationElement::UpdateDynamicAttributes(
    TDynamicAttributesList* dynamicAttributesList,
    const TChildHeapMap& /* childHeapMap */,
    bool checkLiveness)
{
    auto& attributes = dynamicAttributesList->AttributesOf(this);
    attributes.BestLeafDescendant = this;

    // NB: unset Active attribute we treat as unknown here.
    if (!attributes.Active) {
        if (checkLiveness) {
            attributes.Active = IsAlive() && OperationElementSharedState_->Enabled();
        } else {
            attributes.Active = true;
        }
    }

    if (Mutable_) {
        attributes.SatisfactionRatio = ComputeLocalSatisfactionRatio(ResourceUsageAtUpdate_);
    } else {
        attributes.SatisfactionRatio = ComputeLocalSatisfactionRatio(attributes.ResourceUsage);
    }
}

void TSchedulerOperationElement::UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config)
{
    YT_VERIFY(Mutable_);

    if (TreeConfig_->SchedulingSegments->Mode != config->SchedulingSegments->Mode) {
        InitOrUpdateSchedulingSegment(config->SchedulingSegments->Mode);
    }

    TSchedulerElement::UpdateTreeConfig(config);
}

void TSchedulerOperationElement::UpdateControllerConfig(const TFairShareStrategyOperationControllerConfigPtr& config)
{
    YT_VERIFY(Mutable_);
    ControllerConfig_ = config;
}

void TSchedulerOperationElement::ActualizeResourceUsageInDynamicAttributes(TDynamicAttributesList* attributesList)
{
    attributesList->AttributesOf(this).ResourceUsage = (IsAlive() && OperationElementSharedState_->Enabled())
        ? GetInstantResourceUsage()
        : TJobResources();
}

const TJobResources& TSchedulerOperationElement::FillResourceUsageInDynamicAttributes(
    TDynamicAttributesList* attributesList,
    const TResourceUsageSnapshotPtr& resourceUsageSnapshot)
{
    auto& attributes = attributesList->AttributesOf(this);

    if (resourceUsageSnapshot != nullptr) {
        auto it = resourceUsageSnapshot->OperationIdToResourceUsage.find(OperationId_);
        attributes.ResourceUsage = it != resourceUsageSnapshot->OperationIdToResourceUsage.end()
            ? it->second
            : TJobResources();
    } else {
        ActualizeResourceUsageInDynamicAttributes(attributesList);
    }

    return attributes.ResourceUsage;
}

void TSchedulerOperationElement::UpdateCurrentResourceUsage(TScheduleJobsContext* context)
{
    auto resourceUsageBeforeUpdate = GetCurrentResourceUsage(context->DynamicAttributesList());
    ActualizeResourceUsageInDynamicAttributes(&context->DynamicAttributesList());
    context->DynamicAttributesList().AttributesOf(this).ResourceUsageUpdateTime = context->SchedulingContext()->GetNow();

    UpdateDynamicAttributes(&context->DynamicAttributesList(), context->ChildHeapMap(), /*checkLiveness*/ true);
    auto resourceUsageAfterUpdate = GetCurrentResourceUsage(context->DynamicAttributesList());

    auto resourceUsageDelta = resourceUsageAfterUpdate - resourceUsageBeforeUpdate;

    GetMutableParent()->UpdateChild(context->ChildHeapMap(), this);
    UpdateAncestorsDynamicAttributes(context, resourceUsageDelta);
}

bool TSchedulerOperationElement::IsUsageOutdated(TScheduleJobsContext* context) const
{
    auto now = context->SchedulingContext()->GetNow();
    auto updateTime = context->DynamicAttributesFor(this).ResourceUsageUpdateTime;
    return updateTime + DurationToCpuDuration(TreeConfig_->AllowedResourceUsageStaleness) < now;
}

void TSchedulerOperationElement::PrescheduleJob(TScheduleJobsContext* context, EOperationPreemptionPriority targetOperationPreemptionPriority)
{
    auto& attributes = context->DynamicAttributesFor(this);

    CheckForDeactivation(context, targetOperationPreemptionPriority);
    if (!attributes.Active) {
        return;
    }

    UpdateDynamicAttributes(&context->DynamicAttributesList(), context->ChildHeapMap(), /*checkLiveness*/ true);

    ++context->StageState()->ActiveTreeSize;
    ++context->StageState()->ActiveOperationCount;
}

void TSchedulerOperationElement::CheckForDeactivation(
    TScheduleJobsContext* context,
    EOperationPreemptionPriority targetOperationPreemptionPriority)
{
    auto onOperationDeactivated = [&] (EDeactivationReason reason) {
        OnOperationDeactivated(context, reason);
    };

    auto& attributes = context->DynamicAttributesFor(this);

    // Reset operation element activeness (it can be active after scheduling without preepmtion).
    attributes.Active = false;

    if (!IsAlive()) {
        onOperationDeactivated(EDeactivationReason::IsNotAlive);
        return;
    }

    if (auto blockedReason = CheckBlocked(context->SchedulingContext())) {
        onOperationDeactivated(*blockedReason);
        return;
    }

    if (Spec_->PreemptionMode == EPreemptionMode::Graceful &&
        GetStatus(/* atUpdate */ false) == ESchedulableStatus::Normal)
    {
        onOperationDeactivated(EDeactivationReason::FairShareExceeded);
        return;
    }

    if (TreeConfig_->EnableSchedulingTags &&
        SchedulingTagFilterIndex_ != EmptySchedulingTagFilterIndex &&
        !context->CanSchedule()[SchedulingTagFilterIndex_])
    {
        onOperationDeactivated(EDeactivationReason::UnmatchedSchedulingTag);
        return;
    }

    const auto& nodeModule = TNodeSchedulingSegmentManager::GetNodeModule(
        context->SchedulingContext()->GetNodeDescriptor(),
        TreeConfig_->SchedulingSegments->ModuleType);
    if (!IsSchedulingSegmentCompatibleWithNode(context->SchedulingContext()->GetSchedulingSegment(), nodeModule))
    {
        onOperationDeactivated(EDeactivationReason::IncompatibleSchedulingSegment);
        return;
    }

    if (targetOperationPreemptionPriority != EOperationPreemptionPriority::None &&
        targetOperationPreemptionPriority != context->GetOperationPreemptionPriority(this))
    {
        auto deactivationReason = [&] {
            YT_VERIFY(targetOperationPreemptionPriority != EOperationPreemptionPriority::None);

            // TODO(eshcherbin): Somehow get rid of these deactivation reasons.
            switch (targetOperationPreemptionPriority) {
                case EOperationPreemptionPriority::Regular:
                    return EDeactivationReason::IsNotEligibleForPreemptiveScheduling;
                case EOperationPreemptionPriority::SsdRegular:
                    return EDeactivationReason::IsNotEligibleForSsdPreemptiveScheduling;
                case EOperationPreemptionPriority::Aggressive:
                    return EDeactivationReason::IsNotEligibleForAggressivelyPreemptiveScheduling;
                case EOperationPreemptionPriority::SsdAggressive:
                    return EDeactivationReason::IsNotEligibleForSsdAggressivelyPreemptiveScheduling;
                default:
                    YT_ABORT();
            }
        }();
        onOperationDeactivated(deactivationReason);
        return;
    }

    if (Tentative_ &&
        Controller_->IsSaturatedInTentativeTree(
            context->SchedulingContext()->GetNow(),
            TreeId_,
            TreeConfig_->TentativeTreeSaturationDeactivationPeriod))
    {
        onOperationDeactivated(EDeactivationReason::SaturatedInTentativeTree);
        return;
    }

    // NB: we explicitly set Active flag to avoid another call to IsAlive().
    attributes.Active = true;
}

bool TSchedulerOperationElement::HasAggressivelyStarvingElements(TScheduleJobsContext* /*context*/) const
{
    return PersistentAttributes_.StarvationStatus == EStarvationStatus::AggressivelyStarving;
}

TString TSchedulerOperationElement::GetLoggingString() const
{
    return Format(
        "Scheduling info for tree %Qv = {%v, "
        "PendingJobs: %v, AggregatedMinNeededResources: %v, SchedulingSegment: %v, SchedulingSegmentModule: %v, "
        "PreemptableRunningJobs: %v, AggressivelyPreemptableRunningJobs: %v, PreemptionStatusStatistics: %v, "
        "DeactivationReasons: %v, MinNeededResourcesUnsatisfiedCount: %v}",
        GetTreeId(),
        GetLoggingAttributesString(),
        PendingJobCount_,
        AggregatedMinNeededJobResources_,
        SchedulingSegment(),
        PersistentAttributes_.SchedulingSegmentModule,
        GetPreemptableJobCount(),
        GetAggressivelyPreemptableJobCount(),
        GetPreemptionStatusStatistics(),
        GetDeactivationReasons(),
        GetMinNeededResourcesUnsatisfiedCount());
}

void TSchedulerOperationElement::UpdateAncestorsDynamicAttributes(
    TScheduleJobsContext* context,
    const TJobResources& resourceUsageDelta,
    bool checkAncestorsActiveness)
{
    auto* parent = GetMutableParent();
    while (parent) {
        bool activeBefore = context->DynamicAttributesFor(parent).Active;
        if (checkAncestorsActiveness) {
            YT_VERIFY(activeBefore);
        }

        context->DynamicAttributesFor(parent).ResourceUsage += resourceUsageDelta;

        parent->UpdateDynamicAttributes(&context->DynamicAttributesList(), context->ChildHeapMap(), /*checkLiveness*/ true);

        bool activeAfter = context->DynamicAttributesFor(parent).Active;
        if (activeBefore && !activeAfter) {
            ++context->StageState()->DeactivationReasons[EDeactivationReason::NoBestLeafDescendant];
        }

        if (parent->GetMutableParent()) {
            parent->GetMutableParent()->UpdateChild(context->ChildHeapMap(), parent);
        }

        parent = parent->GetMutableParent();
    }
}

void TSchedulerOperationElement::DeactivateOperation(TScheduleJobsContext* context, EDeactivationReason reason)
{
    auto& attributes = context->DynamicAttributesList().AttributesOf(this);
    YT_VERIFY(attributes.Active);
    attributes.Active = false;
    GetMutableParent()->UpdateChild(context->ChildHeapMap(), this);
    UpdateAncestorsDynamicAttributes(context, /* deltaResourceUsage */ TJobResources());
    OnOperationDeactivated(context, reason);
}

void TSchedulerOperationElement::ActivateOperation(TScheduleJobsContext* context)
{
    auto& attributes = context->DynamicAttributesList().AttributesOf(this);
    YT_VERIFY(!attributes.Active);
    attributes.Active = true;
    GetMutableParent()->UpdateChild(context->ChildHeapMap(), this);
    UpdateAncestorsDynamicAttributes(context, /* deltaResourceUsage */ TJobResources(), /* checkAncestorsActiveness */ false);
}

void TSchedulerOperationElement::RecordHeartbeat(const TPackingHeartbeatSnapshot& heartbeatSnapshot)
{
    OperationElementSharedState_->RecordHeartbeat(heartbeatSnapshot, GetPackingConfig());
}

bool TSchedulerOperationElement::CheckPacking(const TPackingHeartbeatSnapshot& heartbeatSnapshot) const
{
    // NB: We expect DetailedMinNeededResources_ to be of size 1 most of the time.
    TJobResourcesWithQuota packingJobResourcesWithQuota;
    if (DetailedMinNeededJobResources_.empty()) {
        // Refuse packing if no information about resource requirements is provided.
        return false;
    } else if (DetailedMinNeededJobResources_.size() == 1) {
        packingJobResourcesWithQuota = DetailedMinNeededJobResources_[0];
    } else {
        auto idx = RandomNumber<ui32>(static_cast<ui32>(DetailedMinNeededJobResources_.size()));
        packingJobResourcesWithQuota = DetailedMinNeededJobResources_[idx];
    }

    return OperationElementSharedState_->CheckPacking(
        /* operationElement */ this,
        heartbeatSnapshot,
        packingJobResourcesWithQuota,
        TotalResourceLimits_,
        GetPackingConfig());
}

TFairShareScheduleJobResult TSchedulerOperationElement::ScheduleJob(TScheduleJobsContext* context, bool ignorePacking)
{
    YT_VERIFY(IsActive(context->DynamicAttributesList()));

    YT_ELEMENT_LOG_DETAILED(this,
        "Trying to schedule job "
        "(SatisfactionRatio: %v, NodeId: %v, NodeResourceUsage: %v, "
        "UsageDiscount: {Total: %v, Unconditional: %v, Conditional: %v}, StageType: %v)",
        context->DynamicAttributesFor(this).SatisfactionRatio,
        context->SchedulingContext()->GetNodeDescriptor().Id,
        FormatResourceUsage(context->SchedulingContext()->ResourceUsage(), context->SchedulingContext()->ResourceLimits()),
        FormatResources(context->SchedulingContext()->UnconditionalResourceUsageDiscount() +
            context->SchedulingContext()->GetConditionalDiscountForOperation(OperationId_)),
        FormatResources(context->SchedulingContext()->UnconditionalResourceUsageDiscount()),
        FormatResources(context->SchedulingContext()->GetConditionalDiscountForOperation(OperationId_)),
        context->GetStageType());

    auto deactivateOperationElement = [&] (EDeactivationReason reason) {
        YT_ELEMENT_LOG_DETAILED(this,
            "Failed to schedule job, operation deactivated "
            "(DeactivationReason: %v, NodeResourceUsage: %v)",
            FormatEnum(reason),
            FormatResourceUsage(context->SchedulingContext()->ResourceUsage(), context->SchedulingContext()->ResourceLimits()));
        DeactivateOperation(context, reason);
    };

    auto recordHeartbeatWithTimer = [&] (const auto& heartbeatSnapshot) {
        NProfiling::TWallTimer timer;
        RecordHeartbeat(heartbeatSnapshot);
        context->StageState()->PackingRecordHeartbeatDuration += timer.GetElapsedTime();
    };

    int schedulingIndex = GetSchedulingIndex();
    YT_VERIFY(schedulingIndex != UndefinedSchedulingIndex);
    ++context->StageState()->SchedulingIndexToScheduleJobAttemptCount[schedulingIndex];
    context->StageState()->MaxSchedulingIndex = std::max(context->StageState()->MaxSchedulingIndex, schedulingIndex);

    if (auto blockedReason = CheckBlocked(context->SchedulingContext())) {
        deactivateOperationElement(*blockedReason);
        return TFairShareScheduleJobResult(/* finished */ true, /* scheduled */ false);
    }

    if (!OperationElementSharedState_->Enabled()) {
        deactivateOperationElement(EDeactivationReason::IsNotAlive);
        return TFairShareScheduleJobResult(/* finished */ true, /* scheduled */ false);
    }

    if (!HasJobsSatisfyingResourceLimits(*context)) {
        YT_ELEMENT_LOG_DETAILED(this,
            "No pending jobs can satisfy available resources on node ("
            "FreeResources: %v, DiscountResources: {Total: %v, Unconditional: %v, Conditional: %v}, "
            "MinNeededResources: %v, DetailedMinNeededResources: %v, "
            "Address: %v)",
            FormatResources(context->SchedulingContext()->GetNodeFreeResourcesWithoutDiscount()),
            FormatResources(context->SchedulingContext()->UnconditionalResourceUsageDiscount() +
                context->SchedulingContext()->GetConditionalDiscountForOperation(OperationId_)),
            FormatResources(context->SchedulingContext()->UnconditionalResourceUsageDiscount()),
            FormatResources(context->SchedulingContext()->GetConditionalDiscountForOperation(OperationId_)),
            FormatResources(AggregatedMinNeededJobResources_),
            MakeFormattableView(
                DetailedMinNeededJobResources_,
                [&] (TStringBuilderBase* builder, const TJobResourcesWithQuota& resources) {
                    builder->AppendFormat("%v",
                        StrategyHost_->FormatResources(resources));
                }),
            context->SchedulingContext()->GetNodeDescriptor().Address);

        OperationElementSharedState_->OnMinNeededResourcesUnsatisfied(
            *context,
            context->SchedulingContext()->GetNodeFreeResourcesWithDiscountForOperation(OperationId_),
            AggregatedMinNeededJobResources_);
        deactivateOperationElement(EDeactivationReason::MinNeededResourcesUnsatisfied);
        return TFairShareScheduleJobResult(/* finished */ true, /* scheduled */ false);
    }

    TJobResources precommittedResources;
    TJobResources availableResources;

    auto deactivationReason = TryStartScheduleJob(*context, &precommittedResources, &availableResources);
    if (deactivationReason) {
        deactivateOperationElement(*deactivationReason);
        return TFairShareScheduleJobResult(/* finished */ true, /* scheduled */ false);
    }

    std::optional<TPackingHeartbeatSnapshot> heartbeatSnapshot;
    if (GetPackingConfig()->Enable && !ignorePacking) {
        heartbeatSnapshot = CreateHeartbeatSnapshot(context->SchedulingContext());

        bool acceptPacking;
        {
            NProfiling::TWallTimer timer;
            acceptPacking = CheckPacking(*heartbeatSnapshot);
            context->StageState()->PackingCheckDuration += timer.GetElapsedTime();
        }

        if (!acceptPacking) {
            recordHeartbeatWithTimer(*heartbeatSnapshot);
            TreeElementHost_->GetResourceTree()->IncreaseHierarchicalResourceUsagePrecommit(ResourceTreeElement_, -precommittedResources);
            deactivateOperationElement(EDeactivationReason::BadPacking);
            context->BadPackingOperations().emplace_back(this);
            FinishScheduleJob(context->SchedulingContext());
            return TFairShareScheduleJobResult(/* finished */ true, /* scheduled */ false);
        }
    }

    TControllerScheduleJobResultPtr scheduleJobResult;
    {
        NProfiling::TWallTimer timer;
        scheduleJobResult = DoScheduleJob(context, availableResources, &precommittedResources);
        auto scheduleJobDuration = timer.GetElapsedTime();
        context->StageState()->TotalScheduleJobDuration += scheduleJobDuration;
        context->StageState()->ExecScheduleJobDuration += scheduleJobResult->Duration;
    }

    if (!scheduleJobResult->StartDescriptor) {
        for (auto reason : TEnumTraits<EScheduleJobFailReason>::GetDomainValues()) {
            context->StageState()->FailedScheduleJob[reason] += scheduleJobResult->Failed[reason];
        }

        ++context->StageState()->ScheduleJobFailureCount;
        deactivateOperationElement(EDeactivationReason::ScheduleJobFailed);

        Controller_->OnScheduleJobFailed(
            context->SchedulingContext()->GetNow(),
            TreeId_,
            scheduleJobResult);

        if (OperationElementSharedState_->Enabled()) {
            TreeElementHost_->GetResourceTree()->IncreaseHierarchicalResourceUsagePrecommit(ResourceTreeElement_, -precommittedResources);
        }

        FinishScheduleJob(context->SchedulingContext());

        return TFairShareScheduleJobResult(/* finished */ true, /* scheduled */ false);
    }

    const auto& startDescriptor = *scheduleJobResult->StartDescriptor;
    if (!OnJobStarted(startDescriptor.Id, startDescriptor.ResourceLimits.ToJobResources(), precommittedResources)) {
        Controller_->AbortJob(startDescriptor.Id, EAbortReason::SchedulingOperationDisabled, TreeId_);
        deactivateOperationElement(EDeactivationReason::OperationDisabled);
        if (OperationElementSharedState_->Enabled()) {
            TreeElementHost_->GetResourceTree()->IncreaseHierarchicalResourceUsagePrecommit(ResourceTreeElement_, -precommittedResources);
        }
        FinishScheduleJob(context->SchedulingContext());
        return TFairShareScheduleJobResult(/* finished */ true, /* scheduled */ false);
    }

    context->SchedulingContext()->StartJob(
        GetTreeId(),
        OperationId_,
        scheduleJobResult->IncarnationId,
        scheduleJobResult->ControllerEpoch,
        startDescriptor,
        Spec_->PreemptionMode,
        schedulingIndex,
        context->GetStageType());

    UpdateCurrentResourceUsage(context);

    if (heartbeatSnapshot) {
        recordHeartbeatWithTimer(*heartbeatSnapshot);
    }

    FinishScheduleJob(context->SchedulingContext());

    YT_ELEMENT_LOG_DETAILED(this,
        "Scheduled a job (SatisfactionRatio: %v, NodeId: %v, JobId: %v, JobResourceLimits: %v)",
        context->DynamicAttributesFor(this).SatisfactionRatio,
        context->SchedulingContext()->GetNodeDescriptor().Id,
        startDescriptor.Id,
        StrategyHost_->FormatResources(startDescriptor.ResourceLimits));
    return TFairShareScheduleJobResult(/* finished */ true, /* scheduled */ true);
}

TString TSchedulerOperationElement::GetId() const
{
    return ToString(OperationId_);
}

std::optional<bool> TSchedulerOperationElement::IsAggressivePreemptionAllowed() const
{
    if (IsGang() && !TreeConfig_->AllowAggressivePreemptionForGangOperations) {
        return false;
    }

    return std::nullopt;
}

std::optional<bool> TSchedulerOperationElement::IsAggressiveStarvationEnabled() const
{
    return Spec_->EnableAggressiveStarvation;
}

std::optional<double> TSchedulerOperationElement::GetSpecifiedWeight() const
{
    return RuntimeParameters_->Weight;
}

const NVectorHdrf::TJobResourcesConfig* TSchedulerOperationElement::GetStrongGuaranteeResourcesConfig() const
{
    return Spec_->StrongGuaranteeResources.Get();
}

TResourceVector TSchedulerOperationElement::GetMaxShare() const
{
    return TResourceVector::FromDouble(Spec_->MaxShareRatio.value_or(1.0));
}

const TSchedulingTagFilter& TSchedulerOperationElement::GetSchedulingTagFilter() const
{
    return SchedulingTagFilter_;
}

ESchedulableStatus TSchedulerOperationElement::GetStatus(bool atUpdate) const
{
    if (UnschedulableReason_) {
        return ESchedulableStatus::Normal;
    }

    return TSchedulerElement::GetStatusImpl(EffectiveFairShareStarvationTolerance_, atUpdate);
}

void TSchedulerOperationElement::SetStarvationStatus(EStarvationStatus starvationStatus)
{
    YT_VERIFY(Mutable_);

    if (starvationStatus == EStarvationStatus::NonStarving) {
        PersistentAttributes_.LastNonStarvingTime = TInstant::Now();
    }

    if (starvationStatus != GetStarvationStatus()) {
        YT_LOG_INFO("Operation starvation status changed (Current: %v, New: %v)",
            GetStarvationStatus(),
            starvationStatus);
    } else {
        return;
    }

    if (GetStarvationStatus() == EStarvationStatus::NonStarving && starvationStatus != EStarvationStatus::NonStarving) {
        OperationElementSharedState_->ResetDeactivationReasonsFromLastNonStarvingTime();
    }
    TSchedulerElement::SetStarvationStatus(starvationStatus);
}

void TSchedulerOperationElement::CheckForStarvation(TInstant now)
{
    YT_VERIFY(Mutable_);

    auto fairShareStarvationTimeout = EffectiveFairShareStarvationTimeout_;
    auto fairShareAggressiveStarvationTimeout = TreeConfig_->FairShareAggressiveStarvationTimeout;

    double jobCountRatio = GetPendingJobCount() / TreeConfig_->JobCountPreemptionTimeoutCoefficient;
    if (jobCountRatio < 1.0) {
        fairShareStarvationTimeout *= jobCountRatio;
        fairShareAggressiveStarvationTimeout *= jobCountRatio;
    }

    TSchedulerElement::CheckForStarvationImpl(
        fairShareStarvationTimeout,
        fairShareAggressiveStarvationTimeout,
        now);
}

const TSchedulerElement* TSchedulerOperationElement::FindPreemptionBlockingAncestor(
    EOperationPreemptionPriority targetOperationPreemptionPriority,
    const TDynamicAttributesList& dynamicAttributesList,
    const TFairShareStrategyTreeConfigPtr& config) const
{
    if (Spec_->PreemptionMode == EPreemptionMode::Graceful) {
        return this;
    }

    int maxUnpreemptableJobCount = config->MaxUnpreemptableRunningJobCount;
    if (Spec_->MaxUnpreemptableRunningJobCount) {
        maxUnpreemptableJobCount = std::min(maxUnpreemptableJobCount, *Spec_->MaxUnpreemptableRunningJobCount);
    }

    int jobCount = GetRunningJobCount();
    if (jobCount <= maxUnpreemptableJobCount) {
        OperationElementSharedState_->UpdatePreemptionStatusStatistics(EOperationPreemptionStatus::ForbiddenSinceLowJobCount);
        return this;
    }

    const TSchedulerElement* element = this;
    while (element && !element->IsRoot()) {
        // NB(eshcherbin): A bit strange that we check for starvation here and then for satisfaction later.
        // Maybe just satisfaction is enough?
        if (config->PreemptionCheckStarvation && element->GetStarvationStatus() != EStarvationStatus::NonStarving) {
            OperationElementSharedState_->UpdatePreemptionStatusStatistics(element == this
                ? EOperationPreemptionStatus::ForbiddenSinceStarving
                : EOperationPreemptionStatus::AllowedConditionally);
            return element;
        }

        bool useAggressiveThreshold = element->GetEffectiveAggressivePreemptionAllowed() &&
            targetOperationPreemptionPriority >= EOperationPreemptionPriority::Aggressive;
        auto threshold = useAggressiveThreshold
            ? config->AggressivePreemptionSatisfactionThreshold
            : config->PreemptionSatisfactionThreshold;

        // NB: We want to use *local* satisfaction ratio here.
        double localSatisfactionRatio = element->ComputeLocalSatisfactionRatio(element->GetCurrentResourceUsage(dynamicAttributesList));
        if (config->PreemptionCheckSatisfaction && localSatisfactionRatio < threshold + NVectorHdrf::RatioComparisonPrecision) {
            OperationElementSharedState_->UpdatePreemptionStatusStatistics(element == this
                ? EOperationPreemptionStatus::ForbiddenSinceUnsatisfied
                : EOperationPreemptionStatus::AllowedConditionally);
            return element;
        }

        element = element->GetParent();
    }

    OperationElementSharedState_->UpdatePreemptionStatusStatistics(EOperationPreemptionStatus::AllowedUnconditionally);
    return {};
}

void TSchedulerOperationElement::SetJobResourceUsage(TJobId jobId, const TJobResources& resources)
{
    auto delta = OperationElementSharedState_->SetJobResourceUsage(jobId, resources);
    IncreaseHierarchicalResourceUsage(delta);

    UpdatePreemptableJobsList();
}

bool TSchedulerOperationElement::IsJobKnown(TJobId jobId) const
{
    return OperationElementSharedState_->IsJobKnown(jobId);
}

bool TSchedulerOperationElement::IsJobPreemptable(TJobId jobId, bool aggressivePreemptionEnabled) const
{
    auto jobPreemptionStatus = GetJobPreemptionStatus(jobId);
    return aggressivePreemptionEnabled
        ? jobPreemptionStatus != EJobPreemptionStatus::NonPreemptable
        : jobPreemptionStatus == EJobPreemptionStatus::Preemptable;
}

TJobPreemptionStatusMap TSchedulerOperationElement::GetJobPreemptionStatusMap() const
{
    return OperationElementSharedState_->GetJobPreemptionStatusMap();
}

int TSchedulerOperationElement::GetRunningJobCount() const
{
    return OperationElementSharedState_->GetRunningJobCount();
}

int TSchedulerOperationElement::GetPreemptableJobCount() const
{
    return OperationElementSharedState_->GetPreemptableJobCount();
}

int TSchedulerOperationElement::GetAggressivelyPreemptableJobCount() const
{
    return OperationElementSharedState_->GetAggressivelyPreemptableJobCount();
}

EJobPreemptionStatus TSchedulerOperationElement::GetJobPreemptionStatus(TJobId jobId) const
{
    return OperationElementSharedState_->GetJobPreemptionStatus(jobId);
}

TPreemptionStatusStatisticsVector TSchedulerOperationElement::GetPreemptionStatusStatistics() const
{
    return OperationElementSharedState_->GetPreemptionStatusStatistics();
}

TInstant TSchedulerOperationElement::GetLastNonStarvingTime() const
{
    return PersistentAttributes_.LastNonStarvingTime;
}

TInstant TSchedulerOperationElement::GetLastScheduleJobSuccessTime() const
{
    return OperationElementSharedState_->GetLastScheduleJobSuccessTime();
}

int TSchedulerOperationElement::GetSlotIndex() const
{
    return SlotIndex_;
}

TString TSchedulerOperationElement::GetUserName() const
{
    return UserName_;
}

TResourceVector TSchedulerOperationElement::GetBestAllocationShare() const
{
    return PersistentAttributes_.BestAllocationShare;
}

bool TSchedulerOperationElement::IsGang() const
{
    return Spec_->IsGang;
}

bool TSchedulerOperationElement::OnJobStarted(
    TJobId jobId,
    const TJobResources& resourceUsage,
    const TJobResources& precommittedResources,
    bool force)
{
    YT_ELEMENT_LOG_DETAILED(this, "Adding job to strategy (JobId: %v)", jobId);

    if (OperationElementSharedState_->AddJob(jobId, resourceUsage, force)) {
        TreeElementHost_->GetResourceTree()->CommitHierarchicalResourceUsage(ResourceTreeElement_, resourceUsage, precommittedResources);
        UpdatePreemptableJobsList();
        return true;
    } else {
        return false;
    }
}

void TSchedulerOperationElement::OnJobFinished(TJobId jobId)
{
    YT_ELEMENT_LOG_DETAILED(this, "Removing job from strategy (JobId: %v)", jobId);

    auto delta = OperationElementSharedState_->RemoveJob(jobId);
    if (delta) {
        IncreaseHierarchicalResourceUsage(-(*delta));
        UpdatePreemptableJobsList();
    }
}

void TSchedulerOperationElement::BuildElementMapping(TFairSharePostUpdateContext* context)
{
    if (Parent_->IsEnabledChild(this)) {
        context->EnabledOperationIdToElement.emplace(OperationId_, this);
    } else {
        context->DisabledOperationIdToElement.emplace(OperationId_, this);
    }
}

TSchedulerElementPtr TSchedulerOperationElement::Clone(TSchedulerCompositeElement* clonedParent)
{
    return New<TSchedulerOperationElement>(*this, clonedParent);
}

bool TSchedulerOperationElement::IsSchedulable() const
{
    return !UnschedulableReason_;
}

std::optional<EUnschedulableReason> TSchedulerOperationElement::ComputeUnschedulableReason() const
{
    auto result = Operation_->CheckUnschedulable();
    if (!result && IsMaxScheduleJobCallsViolated()) {
        result = EUnschedulableReason::MaxScheduleJobCallsViolated;
    }
    return result;
}

bool TSchedulerOperationElement::IsMaxScheduleJobCallsViolated() const
{
    bool result = false;
    Controller_->CheckMaxScheduleJobCallsOverdraft(
        Spec_->MaxConcurrentControllerScheduleJobCalls.value_or(
            ControllerConfig_->MaxConcurrentControllerScheduleJobCalls),
        &result);
    return result;
}

bool TSchedulerOperationElement::IsMaxConcurrentScheduleJobCallsPerNodeShardViolated(
    const ISchedulingContextPtr& schedulingContext) const
{
    return Controller_->IsMaxConcurrentScheduleJobCallsPerNodeShardViolated(
        schedulingContext,
        Controller_->GetMaxConcurrentControllerScheduleJobCallsPerNodeShard());
}

bool TSchedulerOperationElement::HasRecentScheduleJobFailure(NProfiling::TCpuInstant now) const
{
    return Controller_->HasRecentScheduleJobFailure(now);
}

std::optional<EDeactivationReason> TSchedulerOperationElement::CheckBlocked(
    const ISchedulingContextPtr& schedulingContext) const
{
    if (IsMaxConcurrentScheduleJobCallsPerNodeShardViolated(schedulingContext)) {
        return EDeactivationReason::MaxConcurrentScheduleJobCallsPerNodeShardViolated;
    }

    if (HasRecentScheduleJobFailure(schedulingContext->GetNow())) {
        return EDeactivationReason::RecentScheduleJobFailed;
    }

    return std::nullopt;
}

TJobResources TSchedulerOperationElement::GetHierarchicalAvailableResources(const TScheduleJobsContext& context) const
{
    auto availableResources = TJobResources::Infinite();
    const TSchedulerElement* element = this;
    while (element) {
        availableResources = Min(availableResources, element->GetLocalAvailableResourceLimits(context));
        element = element->GetParent();
    }

    return availableResources;
}

TControllerScheduleJobResultPtr TSchedulerOperationElement::DoScheduleJob(
    TScheduleJobsContext* context,
    const TJobResources& availableResources,
    TJobResources* precommittedResources)
{
    ++context->SchedulingStatistics().ControllerScheduleJobCount;

    auto scheduleJobResult = Controller_->ScheduleJob(
        context->SchedulingContext(),
        availableResources,
        ControllerConfig_->ScheduleJobTimeLimit,
        GetTreeId(),
        TreeConfig_);

    // Discard the job in case of resource overcommit.
    if (scheduleJobResult->StartDescriptor) {
        const auto& startDescriptor = *scheduleJobResult->StartDescriptor;
        // Note: |resourceDelta| might be negative.
        const auto resourceDelta = startDescriptor.ResourceLimits.ToJobResources() - *precommittedResources;
        // NB: If the element is disabled, we still choose the success branch. This is kind of a hotfix. See: YT-16070.
        auto increaseResult = EResourceTreeIncreaseResult::Success;
        if (OperationElementSharedState_->Enabled()) {
            increaseResult = TryIncreaseHierarchicalResourceUsagePrecommit(resourceDelta);
        }
        switch (increaseResult) {
            case EResourceTreeIncreaseResult::Success: {
                *precommittedResources += resourceDelta;
                break;
            }
            case EResourceTreeIncreaseResult::ResourceLimitExceeded: {
                auto jobId = scheduleJobResult->StartDescriptor->Id;
                // NB(eshcherbin): GetHierarchicalAvailableResource will never return infinite resources here,
                // because ResourceLimitExceeded could only be triggered if there's an ancestor with specified limits.
                const auto availableDelta = GetHierarchicalAvailableResources(*context);
                YT_LOG_DEBUG("Aborting job with resource overcommit (JobId: %v, Limits: %v, JobResources: %v)",
                    jobId,
                    FormatResources(*precommittedResources + availableDelta),
                    FormatResources(startDescriptor.ResourceLimits.ToJobResources()));

                Controller_->AbortJob(jobId, EAbortReason::SchedulingResourceOvercommit, GetTreeId());

                // Reset result.
                scheduleJobResult = New<TControllerScheduleJobResult>();
                scheduleJobResult->RecordFail(EScheduleJobFailReason::ResourceOvercommit);
                break;
            }
            case EResourceTreeIncreaseResult::ElementIsNotAlive: {
                auto jobId = scheduleJobResult->StartDescriptor->Id;
                YT_LOG_DEBUG("Aborting job as operation is not alive in tree anymore (JobId: %v)", jobId);

                Controller_->AbortJob(jobId, EAbortReason::SchedulingOperationIsNotAlive, GetTreeId());

                scheduleJobResult = New<TControllerScheduleJobResult>();
                scheduleJobResult->RecordFail(EScheduleJobFailReason::OperationIsNotAlive);
                break;
            }
            default:
                YT_ABORT();
        }
    } else if (scheduleJobResult->Failed[EScheduleJobFailReason::Timeout] > 0) {
        YT_LOG_WARNING("Job scheduling timed out");

        ++context->SchedulingStatistics().ControllerScheduleJobTimedOutCount;

        SetOperationAlert(
            OperationId_,
            EOperationAlertType::ScheduleJobTimedOut,
            TError("Job scheduling timed out: either scheduler is under heavy load or operation is too heavy"),
            ControllerConfig_->ScheduleJobTimeoutAlertResetTime);
    }

    return scheduleJobResult;
}

TJobResources TSchedulerOperationElement::ComputeResourceDemand() const
{
    auto maybeUnschedulableReason = Operation_->CheckUnschedulable();
    if (maybeUnschedulableReason == EUnschedulableReason::IsNotRunning || maybeUnschedulableReason == EUnschedulableReason::Suspended) {
        return ResourceUsageAtUpdate_;
    }
    return ResourceUsageAtUpdate_ + TotalNeededResources_;
}

TJobResources TSchedulerOperationElement::GetSpecifiedResourceLimits() const
{
    return ToJobResources(RuntimeParameters_->ResourceLimits, TJobResources::Infinite());
}

void TSchedulerOperationElement::UpdatePreemptableJobsList()
{
    TWallTimer timer;
    int moveCount = 0;

    OperationElementSharedState_->UpdatePreemptableJobsList(
        GetFairShare(),
        TotalResourceLimits_,
        TreeConfig_->PreemptionSatisfactionThreshold,
        TreeConfig_->AggressivePreemptionSatisfactionThreshold,
        &moveCount,
        this);

    auto elapsed = timer.GetElapsedTime();

    if (elapsed > TreeConfig_->UpdatePreemptableListDurationLoggingThreshold) {
        YT_LOG_DEBUG("Preemptable list update is too long (Duration: %v, MoveCount: %v)",
            elapsed.MilliSeconds(),
            moveCount);
    }
}

EResourceTreeIncreaseResult TSchedulerOperationElement::TryIncreaseHierarchicalResourceUsagePrecommit(
    const TJobResources& delta,
    TJobResources* availableResourceLimitsOutput)
{
    return TreeElementHost_->GetResourceTree()->TryIncreaseHierarchicalResourceUsagePrecommit(
        ResourceTreeElement_,
        delta,
        availableResourceLimitsOutput);
}

void TSchedulerOperationElement::AttachParent(TSchedulerCompositeElement* newParent, int slotIndex)
{
    YT_VERIFY(Mutable_);
    YT_VERIFY(!Parent_);

    Parent_ = newParent;
    SlotIndex_ = slotIndex;
    TreeElementHost_->GetResourceTree()->AttachParent(ResourceTreeElement_, newParent->ResourceTreeElement_);

    newParent->IncreaseOperationCount(1);
    newParent->AddChild(this, /* enabled */ false);

    YT_LOG_DEBUG("Operation attached to pool (Pool: %v)", newParent->GetId());
}

void TSchedulerOperationElement::ChangeParent(TSchedulerCompositeElement* parent, int slotIndex)
{
    YT_VERIFY(Mutable_);
    YT_VERIFY(Parent_);

    SlotIndex_ = slotIndex;

    auto oldParentId = Parent_->GetId();
    if (RunningInThisPoolTree_) {
        Parent_->IncreaseRunningOperationCount(-1);
    }
    Parent_->IncreaseOperationCount(-1);
    bool enabled = Parent_->IsEnabledChild(this);
    Parent_->RemoveChild(this);

    Parent_ = parent;
    TreeElementHost_->GetResourceTree()->ChangeParent(
        ResourceTreeElement_,
        parent->ResourceTreeElement_,
        /*descendantOperationElements*/ std::nullopt);

    RunningInThisPoolTree_ = false;  // for consistency
    Parent_->IncreaseOperationCount(1);
    Parent_->AddChild(this, enabled);

    YT_LOG_DEBUG("Operation changed pool (OldPool: %v, NewPool: %v)",
        oldParentId,
        parent->GetId());
}

void TSchedulerOperationElement::DetachParent()
{
    YT_VERIFY(Mutable_);
    YT_VERIFY(Parent_);

    auto parentId = Parent_->GetId();
    if (RunningInThisPoolTree_) {
        Parent_->IncreaseRunningOperationCount(-1);
    }
    Parent_->IncreaseOperationCount(-1);
    Parent_->RemoveChild(this);

    Parent_ = nullptr;
    TreeElementHost_->GetResourceTree()->ScheduleDetachParent(ResourceTreeElement_);

    YT_LOG_DEBUG("Operation detached from pool (Pool: %v)", parentId);
}

void TSchedulerOperationElement::MarkOperationRunningInPool()
{
    Parent_->IncreaseRunningOperationCount(1);
    RunningInThisPoolTree_ = true;
    PendingByPool_.reset();

    YT_LOG_INFO("Operation is running in pool (Pool: %v)", Parent_->GetId());
}

bool TSchedulerOperationElement::IsOperationRunningInPool() const
{
    return RunningInThisPoolTree_;
}

TFairShareStrategyPackingConfigPtr TSchedulerOperationElement::GetPackingConfig() const
{
    return TreeConfig_->Packing;
}

void TSchedulerOperationElement::MarkPendingBy(TSchedulerCompositeElement* violatedPool)
{
    violatedPool->PendingOperationIds().push_back(OperationId_);
    PendingByPool_ = violatedPool->GetId();

    YT_LOG_DEBUG("Operation is pending since max running operation count is violated (OperationId: %v, Pool: %v, Limit: %v)",
        OperationId_,
        violatedPool->GetId(),
        violatedPool->GetMaxRunningOperationCount());
}

void TSchedulerOperationElement::InitOrUpdateSchedulingSegment(ESegmentedSchedulingMode mode)
{
    auto maybeInitialMinNeededResources = Operation_->GetInitialAggregatedMinNeededResources();
    auto segment = Spec_->SchedulingSegment.value_or(
        TStrategySchedulingSegmentManager::GetSegmentForOperation(
            mode,
            maybeInitialMinNeededResources.value_or(TJobResources{})));

    if (SchedulingSegment() != segment) {
        YT_LOG_DEBUG("Setting new scheduling segment for operation (Segment: %v, Mode: %v, InitialMinNeededResources: %v, SpecifiedSegment: %v)",
            segment,
            mode,
            maybeInitialMinNeededResources,
            Spec_->SchedulingSegment);

        SchedulingSegment() = segment;
        SpecifiedSchedulingSegmentModules() = Spec_->SchedulingSegmentModules;
        if (!IsModuleAwareSchedulingSegment(segment)) {
            PersistentAttributes_.SchedulingSegmentModule.reset();
        }
    }
}

bool TSchedulerOperationElement::IsLimitingAncestorCheckEnabled() const
{
    return Spec_->EnableLimitingAncestorCheck;
}

bool TSchedulerOperationElement::IsSchedulingSegmentCompatibleWithNode(
    ESchedulingSegment nodeSegment,
    const TSchedulingSegmentModule& nodeModule) const
{
    if (TreeConfig_->SchedulingSegments->Mode == ESegmentedSchedulingMode::Disabled) {
        return true;
    }

    if (!SchedulingSegment()) {
        return false;
    }

    if (IsModuleAwareSchedulingSegment(*SchedulingSegment())) {
        if (!PersistentAttributes_.SchedulingSegmentModule) {
            // We have not decided on the operation's module yet.
            return false;
        }

        return SchedulingSegment() == nodeSegment && PersistentAttributes_.SchedulingSegmentModule == nodeModule;
    }

    YT_VERIFY(!PersistentAttributes_.SchedulingSegmentModule);

    return *SchedulingSegment() == nodeSegment;
}

void TSchedulerOperationElement::PrepareConditionalUsageDiscounts(TScheduleJobsContext* context, EOperationPreemptionPriority targetOperationPreemptionPriority) const
{
    if (context->GetOperationPreemptionPriority(this) != targetOperationPreemptionPriority) {
        return;
    }

    context->SchedulingContext()->SetConditionalDiscountForOperation(OperationId_, context->CurrentConditionalDiscount());
}

void TSchedulerOperationElement::CountOperationsByPreemptionPriority(TScheduleJobsContext* context) const
{
    if (GetStarvationStatus() == EStarvationStatus::NonStarving) {
        return;
    }

    ++context->OperationCountByPreemptionPriority()[context->GetOperationPreemptionPriority(this)];
}

void TSchedulerOperationElement::CollectOperationSchedulingSegmentContexts(
    THashMap<TOperationId, TOperationSchedulingSegmentContext>* operationContexts) const
{
    YT_VERIFY(operationContexts->emplace(
        OperationId_,
        TOperationSchedulingSegmentContext{
            .ResourceDemand = ResourceDemand_,
            .ResourceUsage = ResourceUsageAtUpdate_,
            .DemandShare = Attributes_.DemandShare,
            .FairShare = Attributes_.FairShare.Total,
            .Segment = SchedulingSegment(),
            .Module = PersistentAttributes_.SchedulingSegmentModule,
            .SpecifiedModules = SpecifiedSchedulingSegmentModules(),
            .FailingToScheduleAtModuleSince = PersistentAttributes_.FailingToScheduleAtModuleSince,
        }).second);
}

void TSchedulerOperationElement::ApplyOperationSchedulingSegmentChanges(
    const THashMap<TOperationId, TOperationSchedulingSegmentContext>& operationContexts)
{
    const auto& context = GetOrCrash(operationContexts, OperationId_);
    PersistentAttributes_.SchedulingSegmentModule = context.Module;
    PersistentAttributes_.FailingToScheduleAtModuleSince = context.FailingToScheduleAtModuleSince;
}

void TSchedulerOperationElement::CollectResourceTreeOperationElements(std::vector<TResourceTreeElementPtr>* elements) const
{
    elements->push_back(ResourceTreeElement_);
}

////////////////////////////////////////////////////////////////////////////////

TSchedulerRootElement::TSchedulerRootElement(
    ISchedulerStrategyHost* strategyHost,
    IFairShareTreeElementHost* treeElementHost,
    TFairShareStrategyTreeConfigPtr treeConfig,
    const TString& treeId,
    const NLogging::TLogger& logger)
    : TSchedulerCompositeElement(
        strategyHost,
        treeElementHost,
        treeConfig,
        treeId,
        RootPoolName,
        EResourceTreeElementKind::Root,
        logger.WithTag("PoolId: %v, SchedulingMode: %v",
            RootPoolName,
            ESchedulingMode::FairShare))
{
    Mode_ = ESchedulingMode::FairShare;
}

TSchedulerRootElement::TSchedulerRootElement(const TSchedulerRootElement& other)
    : TSchedulerCompositeElement(other, nullptr)
    , TSchedulerRootElementFixedState(other)
{ }

void TSchedulerRootElement::UpdateTreeConfig(const TFairShareStrategyTreeConfigPtr& config)
{
    TSchedulerCompositeElement::UpdateTreeConfig(config);
}

void TSchedulerRootElement::PreUpdate(NVectorHdrf::TFairShareUpdateContext* context)
{
    YT_VERIFY(Mutable_);

    TForbidContextSwitchGuard contextSwitchGuard;

    DisableNonAliveElements();

    PreUpdateBottomUp(context);
}

/// Steps of fair share post update:
///
/// 1. Publish the computed fair share to the shared resource tree and update the operations' preemptable job lists.
///
/// 2. Update dynamic attributes based on the calculated fair share (for orchid).
///
/// 3. Manage scheduling segments.
///    We build the tree's scheduling segment state and assign eligible operations in module-aware segments to modules.
void TSchedulerRootElement::PostUpdate(
    TFairSharePostUpdateContext* postUpdateContext,
	TManageTreeSchedulingSegmentsContext* manageSegmentsContext)
{
    VERIFY_INVOKER_AFFINITY(StrategyHost_->GetFairShareUpdateInvoker());

    YT_VERIFY(Mutable_);

    PublishFairShareAndUpdatePreemptionSettings();

    BuildSchedulableChildrenLists(postUpdateContext);

    // Calculate tree sizes.
    SchedulableElementCount_ = EnumerateElements(/* startIndex */ 0, /* isSchedulableValueFilter*/ true);
    TreeSize_ = EnumerateElements(/* startIndex */ SchedulableElementCount_, /* isSchedulableValueFilter*/ false);

    // We calculate SatisfactionRatio by computing dynamic attributes using the same algorithm as during the scheduling phase.
    TDynamicAttributesList dynamicAttributesList(TreeSize_);
    TChildHeapMap childHeapMap;
    UpdateSchedulableAttributesFromDynamicAttributes(&dynamicAttributesList, &childHeapMap);

    BuildSchedulableIndices(&dynamicAttributesList, &childHeapMap);

    ManageSchedulingSegments(manageSegmentsContext);

    BuildElementMapping(postUpdateContext);

    // NB(eshcherbin): This method should be called after |BuildElementMapping|.
    UpdateCachedJobPreemptionStatusesIfNecessary(postUpdateContext);
}

const TSchedulingTagFilter& TSchedulerRootElement::GetSchedulingTagFilter() const
{
    return EmptySchedulingTagFilter;
}

TString TSchedulerRootElement::GetId() const
{
    return RootPoolName;
}

std::optional<double> TSchedulerRootElement::GetSpecifiedWeight() const
{
    return std::nullopt;
}

TJobResources TSchedulerRootElement::GetSpecifiedStrongGuaranteeResources() const
{
    return TotalResourceLimits_;
}

TResourceVector TSchedulerRootElement::GetMaxShare() const
{
    return TResourceVector::Ones();
}

std::optional<double> TSchedulerRootElement::GetSpecifiedFairShareStarvationTolerance() const
{
    return TreeConfig_->FairShareStarvationTolerance;
}

std::optional<TDuration> TSchedulerRootElement::GetSpecifiedFairShareStarvationTimeout() const
{
    return TreeConfig_->FairShareStarvationTimeout;
}

std::optional<bool> TSchedulerRootElement::IsAggressivePreemptionAllowed() const
{
    return true;
}

std::optional<bool> TSchedulerRootElement::IsAggressiveStarvationEnabled() const
{
    return TreeConfig_->EnableAggressiveStarvation;
}

void TSchedulerRootElement::CheckForStarvation(TInstant /*now*/)
{ }

int TSchedulerRootElement::GetMaxRunningOperationCount() const
{
    return TreeConfig_->MaxRunningOperationCount;
}

int TSchedulerRootElement::GetMaxOperationCount() const
{
    return TreeConfig_->MaxOperationCount;
}

TPoolIntegralGuaranteesConfigPtr TSchedulerRootElement::GetIntegralGuaranteesConfig() const
{
    return New<TPoolIntegralGuaranteesConfig>();
}

std::vector<EFifoSortParameter> TSchedulerRootElement::GetFifoSortParameters() const
{
    YT_ABORT();
}

bool TSchedulerRootElement::AreImmediateOperationsForbidden() const
{
    return TreeConfig_->ForbidImmediateOperationsInRoot;
}

THashSet<TString> TSchedulerRootElement::GetAllowedProfilingTags() const
{
    return {};
}

bool TSchedulerRootElement::IsFairShareTruncationInFifoPoolEnabled() const
{
    return TreeConfig_->EnableFairShareTruncationInFifoPool;
}

bool TSchedulerRootElement::CanAcceptFreeVolume() const
{
    // This value is not used.
    return false;
}

bool TSchedulerRootElement::ShouldDistributeFreeVolumeAmongChildren() const
{
    return false;
}

bool TSchedulerRootElement::IsInferringChildrenWeightsFromHistoricUsageEnabled() const
{
    return false;
}

TJobResources TSchedulerRootElement::GetSpecifiedResourceLimits() const
{
    return TJobResources::Infinite();
}

THistoricUsageAggregationParameters TSchedulerRootElement::GetHistoricUsageAggregationParameters() const
{
    return THistoricUsageAggregationParameters(EHistoricUsageAggregationMode::None);
}

void TSchedulerRootElement::BuildResourceMetering(
    const std::optional<TMeteringKey>& /*parentKey*/,
    const THashMap<TString, TResourceVolume>& poolResourceUsages,
    TMeteringMap* meteringMap) const
{
    auto key = TMeteringKey{
        .AbcId = StrategyHost_->GetDefaultAbcId(),
        .TreeId = GetTreeId(),
        .PoolId = GetId(),
    };

    TResourceVolume accumulatedResourceUsageVolume;
    {
        auto it = poolResourceUsages.find(GetId());
        if (it != poolResourceUsages.end()) {
            accumulatedResourceUsageVolume = it->second;
        }
    }

    TJobResources TotalStrongGuaranteeResources;
    for (const auto& child : EnabledChildren_) {
        TotalStrongGuaranteeResources += child->GetSpecifiedStrongGuaranteeResources();
    }

    auto insertResult = meteringMap->insert({
        key,
        TMeteringStatistics(
            /* strongGuaranteeResources */ TotalStrongGuaranteeResources,
            /* resourceFlow */ {},
            /* burstGuaranteResources */ {},
            GetResourceUsageAtUpdate(),
            accumulatedResourceUsageVolume)});
    YT_VERIFY(insertResult.second);

    for (const auto& child : EnabledChildren_) {
        child->BuildResourceMetering(/* parentKey */ key, poolResourceUsages, meteringMap);
    }
}

TSchedulerElementPtr TSchedulerRootElement::Clone(TSchedulerCompositeElement* /*clonedParent*/)
{
    YT_ABORT();
}

TSchedulerRootElementPtr TSchedulerRootElement::Clone()
{
    return New<TSchedulerRootElement>(*this);
}

bool TSchedulerRootElement::IsDefaultConfigured() const
{
    return false;
}

TResourceDistributionInfo TSchedulerRootElement::GetResourceDistributionInfo() const
{
    double maxDistributedIntegralRatio = std::max(Attributes_.TotalBurstRatio, Attributes_.TotalResourceFlowRatio);
    double undistributedResourceFlowRatio = std::max(Attributes_.TotalBurstRatio - Attributes_.TotalResourceFlowRatio, 0.0);
    double undistributedBurstGuaranteeRatio = std::max(Attributes_.TotalResourceFlowRatio - Attributes_.TotalBurstRatio, 0.0);

    TResourceDistributionInfo info;
    for (const auto& child : EnabledChildren_) {
        info.DistributedStrongGuaranteeResources += child->GetSpecifiedStrongGuaranteeResources();
    }
    info.DistributedResourceFlow = TotalResourceLimits_ * Attributes_.TotalResourceFlowRatio;
    info.DistributedBurstGuaranteeResources = TotalResourceLimits_ * Attributes_.TotalBurstRatio;
    info.DistributedResources = info.DistributedStrongGuaranteeResources + TotalResourceLimits_ * maxDistributedIntegralRatio;
    info.UndistributedResources = TotalResourceLimits_ - info.DistributedResources;
    info.UndistributedResourceFlow = TotalResourceLimits_ * undistributedResourceFlowRatio;
    info.UndistributedBurstGuaranteeResources = TotalResourceLimits_ * undistributedBurstGuaranteeRatio;

    return info;
}

void TSchedulerRootElement::BuildResourceDistributionInfo(TFluentMap fluent) const
{
    auto info = GetResourceDistributionInfo();
    fluent
        .Item("distributed_strong_guarantee_resources").Value(info.DistributedStrongGuaranteeResources)
        .Item("distributed_resource_flow").Value(info.DistributedResourceFlow)
        .Item("distributed_burst_guarantee_resources").Value(info.DistributedBurstGuaranteeResources)
        .Item("distributed_resources").Value(info.DistributedResources)
        .Item("undistributed_resources").Value(info.UndistributedResources)
        .Item("undistributed_resource_flow").Value(info.UndistributedResourceFlow)
        .Item("undistributed_burst_guarantee_resources").Value(info.UndistributedBurstGuaranteeResources);
}

void TSchedulerRootElement::BuildSchedulableIndices(TDynamicAttributesList* dynamicAttributesList, TChildHeapMap* childHeapMap)
{
    auto& attributes = dynamicAttributesList->AttributesOf(this);

    int schedulingIndex = 0;

    TSchedulerOperationElement* bestLeafDescendant = nullptr;
    while (true) {
        if (!attributes.Active) {
            break;
        }

        bestLeafDescendant = attributes.BestLeafDescendant;
        bestLeafDescendant->SetSchedulingIndex(schedulingIndex++);

        dynamicAttributesList->AttributesOf(bestLeafDescendant).Active = false;

        TSchedulerElement* current = bestLeafDescendant;
        while (auto* parent = current->GetMutableParent()) {
            parent->UpdateChild(*childHeapMap, current);
            parent->UpdateDynamicAttributes(dynamicAttributesList, *childHeapMap, /*checkLiveness*/ false);
            current = parent;
        }
    }
}

void TSchedulerRootElement::ManageSchedulingSegments(TManageTreeSchedulingSegmentsContext* manageSegmentsContext)
{
    auto mode = manageSegmentsContext->TreeConfig->SchedulingSegments->Mode;
    if (mode != ESegmentedSchedulingMode::Disabled) {
        CollectOperationSchedulingSegmentContexts(&(manageSegmentsContext->Operations));
    }

    TStrategySchedulingSegmentManager::ManageSegmentsInTree(manageSegmentsContext, TreeId_);

    if (mode != ESegmentedSchedulingMode::Disabled) {
        ApplyOperationSchedulingSegmentChanges(manageSegmentsContext->Operations);
    }
}

void TSchedulerRootElement::UpdateCachedJobPreemptionStatusesIfNecessary(TFairSharePostUpdateContext* context) const
{
    if (context->Now < context->CachedJobPreemptionStatuses.UpdateTime + TreeConfig_->CachedJobPreemptionStatusesUpdatePeriod) {
        return;
    }

    auto jobPreemptionStatuses = New<TRefCountedJobPreemptionStatusMapPerOperation>();
    auto collectJobPreemptionStatuses = [&] (const auto& operationMap) {
        for (auto [operationId, operationElement] : operationMap) {
            YT_VERIFY(jobPreemptionStatuses->emplace(operationId, operationElement->GetJobPreemptionStatusMap()).second);
        }
    };

    collectJobPreemptionStatuses(context->EnabledOperationIdToElement);
    collectJobPreemptionStatuses(context->DisabledOperationIdToElement);

    context->CachedJobPreemptionStatuses = {
        .Value = std::move(jobPreemptionStatuses),
        .UpdateTime = context->Now,
    };
}

double TSchedulerRootElement::GetSpecifiedBurstRatio() const
{
    return 0.0;
}

double TSchedulerRootElement::GetSpecifiedResourceFlowRatio() const
{
    return 0.0;
}

////////////////////////////////////////////////////////////////////////////////

TChildHeap::TChildHeap(
    const std::vector<TSchedulerElementPtr>& children,
    TDynamicAttributesList* dynamicAttributesList,
    const TSchedulerCompositeElement* owningElement,
    ESchedulingMode mode)
    : DynamicAttributesList_(*dynamicAttributesList)
    , OwningElement_(owningElement)
    , Mode_(mode)
{
    ChildHeap_.reserve(children.size());
    for (const auto& child : children) {
        ChildHeap_.push_back(child.Get());
    }
    MakeHeap(
        ChildHeap_.begin(),
        ChildHeap_.end(),
        [&] (const TSchedulerElement* lhs, const TSchedulerElement* rhs) {
            return Comparator(lhs, rhs);
        });

    for (size_t index = 0; index < ChildHeap_.size(); ++index) {
        DynamicAttributesList_.AttributesOf(ChildHeap_[index]).HeapIndex = index;
    }
}

TSchedulerElement* TChildHeap::GetTop() const
{
    YT_VERIFY(!ChildHeap_.empty());
    return ChildHeap_.front();
}

void TChildHeap::Update(TSchedulerElement* child)
{
    int heapIndex = DynamicAttributesList_.AttributesOf(child).HeapIndex;
    YT_VERIFY(heapIndex != InvalidHeapIndex);
    AdjustHeapItem(
        ChildHeap_.begin(),
        ChildHeap_.end(),
        ChildHeap_.begin() + heapIndex,
        [&] (const TSchedulerElement* lhs, const TSchedulerElement* rhs) {
            return Comparator(lhs, rhs);
        },
        [&] (size_t offset) {
            DynamicAttributesList_.AttributesOf(ChildHeap_[offset]).HeapIndex = offset;
        });
}

const std::vector<TSchedulerElement*>& TChildHeap::GetHeap() const
{
    return ChildHeap_;
}

bool TChildHeap::Comparator(const TSchedulerElement* lhs, const TSchedulerElement* rhs) const
{
    const auto& lhsAttributes = DynamicAttributesList_.AttributesOf(lhs);
    const auto& rhsAttributes = DynamicAttributesList_.AttributesOf(rhs);

    if (lhsAttributes.Active != rhsAttributes.Active) {
        return rhsAttributes.Active < lhsAttributes.Active;
    }

    switch (Mode_) {
        case ESchedulingMode::Fifo:
            return OwningElement_->HasHigherPriorityInFifoMode(lhs, rhs);
        case ESchedulingMode::FairShare:
            return lhsAttributes.SatisfactionRatio < rhsAttributes.SatisfactionRatio;
        default:
            YT_ABORT();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
