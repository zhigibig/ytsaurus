#include "fair_share_strategy.h"
#include "fair_share_tree_element.h"
#include "public.h"
#include "config.h"
#include "scheduler_strategy.h"
#include "scheduling_context.h"
#include "fair_share_strategy_operation_controller.h"

#include <yt/ytlib/scheduler/job_resources.h>

#include <yt/core/concurrency/async_rw_lock.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/thread_pool.h>

#include <yt/core/misc/algorithm_helpers.h>
#include <yt/core/misc/finally.h>

#include <yt/core/profiling/profile_manager.h>
#include <yt/core/profiling/timing.h>

#include <util/string/split.h>

namespace NYT {
namespace NScheduler {

using namespace NConcurrency;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NYson;
using namespace NYTree;
using namespace NProfiling;
using namespace NControllerAgent;

////////////////////////////////////////////////////////////////////////////////

static const auto& Profiler = SchedulerProfiler;

////////////////////////////////////////////////////////////////////////////////

namespace {

TTagIdList GetFailReasonProfilingTags(EScheduleJobFailReason reason)
{
    static THashMap<EScheduleJobFailReason, TTagId> tagId;

    auto it = tagId.find(reason);
    if (it == tagId.end()) {
        it = tagId.emplace(
            reason,
            TProfileManager::Get()->RegisterTag("reason", FormatEnum(reason))
        ).first;
    }
    return {it->second};
};

TTagId GetSlotIndexProfilingTag(int slotIndex)
{
    static THashMap<int, TTagId> slotIndexToTagIdMap;

    auto it = slotIndexToTagIdMap.find(slotIndex);
    if (it == slotIndexToTagIdMap.end()) {
        it = slotIndexToTagIdMap.emplace(
            slotIndex,
            TProfileManager::Get()->RegisterTag("slot_index", ToString(slotIndex))
        ).first;
    }
    return it->second;
};

class TFairShareStrategyOperationState
    : public TIntrinsicRefCounted
{
public:
    using TTreeIdToPoolIdMap = THashMap<TString, TPoolName>;

    DEFINE_BYVAL_RO_PROPERTY(IOperationStrategyHost*, Host);
    DEFINE_BYVAL_RO_PROPERTY(TFairShareStrategyOperationControllerPtr, Controller);
    DEFINE_BYVAL_RW_PROPERTY(bool, Active);
    DEFINE_BYREF_RW_PROPERTY(TTreeIdToPoolIdMap, TreeIdToPoolIdMap);
    DEFINE_BYREF_RW_PROPERTY(std::vector<TString>, ErasedTrees);

public:
    TFairShareStrategyOperationState(IOperationStrategyHost* host)
        : Host_(host)
        , Controller_(New<TFairShareStrategyOperationController>(host))
    { }

    TPoolName GetPoolIdByTreeId(const TString& treeId) const
    {
        auto it = TreeIdToPoolIdMap_.find(treeId);
        YCHECK(it != TreeIdToPoolIdMap_.end());
        return it->second;
    }

    void EraseTree(const TString& treeId)
    {
        ErasedTrees_.push_back(treeId);
        YCHECK(TreeIdToPoolIdMap_.erase(treeId) == 1);
    }
};

using TFairShareStrategyOperationStatePtr = TIntrusivePtr<TFairShareStrategyOperationState>;

struct TOperationUnregistrationResult
{
    std::vector<TOperationId> OperationsToActivate;
};

struct TPoolsUpdateResult
{
    TError Error;
    bool Updated;
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

//! Thread affinity: any
struct IFairShareTreeSnapshot
    : public TIntrinsicRefCounted
{
    virtual TFuture<void> ScheduleJobs(const ISchedulingContextPtr& schedulingContext) = 0;
    virtual void ProcessUpdatedJob(const TOperationId& operationId, const TJobId& jobId, const TJobResources& delta) = 0;
    virtual void ProcessFinishedJob(const TOperationId& operationId, const TJobId& jobId) = 0;
    virtual bool HasOperation(const TOperationId& operationId) const = 0;
    virtual void ApplyJobMetricsDelta(const TOperationId& operationId, const TJobMetrics& jobMetricsDelta) = 0;
    virtual const TSchedulingTagFilter& GetNodesFilter() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IFairShareTreeSnapshot);

////////////////////////////////////////////////////////////////////////////////

class TFairShareTree
    : public TIntrinsicRefCounted
{
public:
    TFairShareTree(
        TFairShareStrategyTreeConfigPtr config,
        TFairShareStrategyOperationControllerConfigPtr controllerConfig,
        ISchedulerStrategyHost* host,
        const std::vector<IInvokerPtr>& feasibleInvokers,
        const TString& treeId)
        : Config(config)
        , ControllerConfig(controllerConfig)
        , Host(host)
        , FeasibleInvokers(feasibleInvokers)
        , TreeId(treeId)
        , TreeIdProfilingTag(TProfileManager::Get()->RegisterTag("tree", TreeId))
        , Logger(NLogging::TLogger(SchedulerLogger)
            .AddTag("TreeId: %v", treeId))
        , NonPreemptiveProfilingCounters("/non_preemptive", {TreeIdProfilingTag})
        , PreemptiveProfilingCounters("/preemptive", {TreeIdProfilingTag})
        , FairShareUpdateTimeCounter("/fair_share_update_time", {TreeIdProfilingTag})
        , FairShareLogTimeCounter("/fair_share_log_time", {TreeIdProfilingTag})
        , AnalyzePreemptableJobsTimeCounter("/analyze_preemptable_jobs_time", {TreeIdProfilingTag})
    {
        RootElement = New<TRootElement>(Host, config, GetPoolProfilingTag(RootPoolName), TreeId);
    }

    IFairShareTreeSnapshotPtr CreateSnapshot()
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        return New<TFairShareTreeSnapshot>(this, RootElementSnapshot, Logger);
    }

    TFuture<void> ValidateOperationPoolsCanBeUsed(const IOperationStrategyHost* operation, const TPoolName& poolPair)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        return BIND(&TFairShareTree::DoValidateOperationPoolsCanBeUsed, MakeStrong(this))
            .AsyncVia(GetCurrentInvoker())
            .Run(operation, poolPair);
    }

    void ValidatePoolLimits(const IOperationStrategyHost* operation, const TPoolName& poolName)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        ValidateOperationCountLimit(operation, poolName);
        ValidateEphemeralPoolLimit(operation, poolName);
    }

    void ValidatePoolLimitsOnPoolChange(const IOperationStrategyHost* operation, const TPoolName& newPoolName)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        ValidateEphemeralPoolLimit(operation, newPoolName);
        ValidateAllOperationsCountsOnPoolChange(operation->GetId(), newPoolName);
    }

    void ValidateAllOperationsCountsOnPoolChange(const TOperationId& operationId, const TPoolName& newPoolName)
    {
        auto operationElement = GetOperationElement(operationId);
        std::vector<TString> oldPools;
        TCompositeSchedulerElement* pool = operationElement->GetParent();
        while (pool) {
            oldPools.push_back(pool->GetId());
            pool = pool->GetParent();
        }

        std::vector<TString> newPools;
        pool = GetPoolOrParent(newPoolName).Get();
        while (pool) {
            newPools.push_back(pool->GetId());
            pool = pool->GetParent();
        }

        while (!newPools.empty() && !oldPools.empty() && newPools.back() == oldPools.back()) {
            newPools.pop_back();
            oldPools.pop_back();
        }

        for (const auto& newPool : newPools) {
            auto currentPool = GetPool(newPool);
            if (currentPool->OperationCount() >= currentPool->GetMaxOperationCount()) {
                THROW_ERROR_EXCEPTION("Max operation count of pool %Qv violated", newPool);
            }
            if (currentPool->RunningOperationCount() >= currentPool->GetMaxRunningOperationCount()) {
                THROW_ERROR_EXCEPTION("Max running operation count of pool %Qv violated", newPool);
            }
        }
    }

    bool RegisterOperation(
        const TFairShareStrategyOperationStatePtr& state,
        const TStrategyOperationSpecPtr& spec,
        const TOperationFairShareTreeRuntimeParametersPtr& runtimeParams)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto operationId = state->GetHost()->GetId();

        auto rootSchedulingTagFilter = spec->SchedulingTagFilter;

        auto clonedSpec = CloneYsonSerializable(spec);
        auto optionsIt = spec->SchedulingOptionsPerPoolTree.find(TreeId);
        if (optionsIt != spec->SchedulingOptionsPerPoolTree.end()) {
            const auto& options = optionsIt->second;
            ReconfigureYsonSerializable(clonedSpec, ConvertToNode(options));
            if (!rootSchedulingTagFilter.IsEmpty()) {
                clonedSpec->SchedulingTagFilter = rootSchedulingTagFilter;
            }
        }

        auto operationElement = New<TOperationElement>(
            Config,
            clonedSpec,
            runtimeParams,
            state->GetController(),
            ControllerConfig,
            Host,
            state->GetHost(),
            TreeId);

        int index = RegisterSchedulingTagFilter(TSchedulingTagFilter(clonedSpec->SchedulingTagFilter));
        operationElement->SetSchedulingTagFilterIndex(index);

        YCHECK(OperationIdToElement.insert(std::make_pair(operationId, operationElement)).second);

        auto poolName = state->GetPoolIdByTreeId(TreeId);

        if (!AttachOperation(state, operationElement, poolName)) {
            WaitingOperationQueue.push_back(operationId);
            return false;
        }
        return true;
    }

    // Attaches operation to tree and returns if it can be activated (pools limits are satisfied)
    bool AttachOperation(
        const TFairShareStrategyOperationStatePtr& state,
        TOperationElementPtr& operationElement,
        const TPoolName& poolName)
    {
        auto operationId = state->GetHost()->GetId();

        auto pool = FindPool(poolName.GetPool());
        if (!pool) {
            pool = New<TPool>(
                Host,
                poolName.GetPool(),
                New<TPoolConfig>(),
                /* defaultConfigured */ true,
                Config,
                GetPoolProfilingTag(poolName.GetPool()),
                TreeId);

            const auto& userName = state->GetHost()->GetAuthenticatedUser();
            pool->SetUserName(userName);
            UserToEphemeralPools[userName].insert(poolName.GetPool());
            RegisterPool(pool);
        }
        if (!pool->GetParent()) {
            if (poolName.GetParentPool()) {
                SetPoolParent(pool, GetPool(poolName.GetParentPool().Get()));
            } else {
                SetPoolDefaultParent(pool);
            }
        }

        pool->IncreaseOperationCount(1);

        pool->AddChild(operationElement, false);
        pool->IncreaseHierarchicalResourceUsage(operationElement->GetLocalResourceUsage());
        operationElement->SetParent(pool.Get());

        AllocateOperationSlotIndex(state, poolName.GetPool());

        auto violatedPool = FindPoolViolatingMaxRunningOperationCount(pool.Get());
        if (!violatedPool) {
            AddOperationToPool(operationId);
            return true;
        }

        LOG_DEBUG("Max running operation count violated (OperationId: %v, Pool: %v, Limit: %v)",
            operationId,
            violatedPool->GetId(),
            violatedPool->GetMaxRunningOperationCount());
        Host->SetOperationAlert(
            operationId,
            EOperationAlertType::OperationPending,
            TError("Max running operation count violated")
                << TErrorAttribute("pool", violatedPool->GetId())
                << TErrorAttribute("limit", violatedPool->GetMaxRunningOperationCount())
            );
        return false;
    }

    TOperationUnregistrationResult UnregisterOperation(
        const TFairShareStrategyOperationStatePtr& state)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto operationId = state->GetHost()->GetId();
        const auto& operationElement = FindOperationElement(operationId);
        auto wasActive = DetachOperation(state, operationElement);

        UnregisterSchedulingTagFilter(operationElement->GetSchedulingTagFilterIndex());

        operationElement->Disable();
        YCHECK(OperationIdToElement.erase(operationId) == 1);
        operationElement->SetAlive(false);

        // Operation can be missing in this map.
        OperationIdToActivationTime_.erase(operationId);

        TOperationUnregistrationResult result;
        if (wasActive) {
            TryActivateOperationsFromQueue(&result.OperationsToActivate);
        }
        return result;
    }

    // Detaches operation element from tree but leaves it eligible to be attached in another place in the same tree.
    // Removes operation from waiting queue if operation wasn't active. Returns true if operation was active.
    bool DetachOperation(const TFairShareStrategyOperationStatePtr& state, const TOperationElementPtr& operationElement)
    {
        auto operationId = state->GetHost()->GetId();
        auto* pool = static_cast<TPool*>(operationElement->GetParent());

        ReleaseOperationSlotIndex(state, pool->GetId());

        pool->RemoveChild(operationElement);
        pool->IncreaseOperationCount(-1);
        pool->IncreaseHierarchicalResourceUsage(-operationElement->GetLocalResourceUsage());

        LOG_INFO("Operation removed from pool (OperationId: %v, Pool: %v)",
            operationId,
            pool->GetId());

        bool wasActive = true;
        for (auto it = WaitingOperationQueue.begin(); it != WaitingOperationQueue.end(); ++it) {
            if (*it == operationId) {
                wasActive = false;
                WaitingOperationQueue.erase(it);
                break;
            }
        }

        if (wasActive) {
            pool->IncreaseRunningOperationCount(-1);
        }

        if (pool->IsEmpty() && pool->IsDefaultConfigured()) {
            UnregisterPool(pool);
        }

        return wasActive;
    }

    void DisableOperation(const TFairShareStrategyOperationStatePtr& state)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto operationElement = GetOperationElement(state->GetHost()->GetId());
        auto usage = operationElement->GetLocalResourceUsage();
        operationElement->Disable();

        auto* parent = operationElement->GetParent();
        parent->IncreaseHierarchicalResourceUsage(-usage);
        parent->DisableChild(operationElement);
    }

    void EnableOperation(const TFairShareStrategyOperationStatePtr& state)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto operationId = state->GetHost()->GetId();
        auto operationElement = GetOperationElement(operationId);

        auto* parent = operationElement->GetParent();
        parent->EnableChild(operationElement);

        operationElement->Enable();
    }

    TPoolsUpdateResult UpdatePools(const INodePtr& poolsNode)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        if (LastPoolsNodeUpdate && AreNodesEqual(LastPoolsNodeUpdate, poolsNode)) {
            LOG_INFO("Pools are not changed, skipping update");
            return {LastPoolsNodeUpdateError, false};
        }

        LastPoolsNodeUpdate = poolsNode;

        std::vector<TError> errors;

        try {
            // Build the set of potential orphans.
            THashSet<TString> orphanPoolIds;
            for (const auto& pair : Pools) {
                YCHECK(orphanPoolIds.insert(pair.first).second);
            }

            // Track ids appearing in various branches of the tree.
            THashMap<TString, TYPath> poolIdToPath;

            // NB: std::function is needed by parseConfig to capture itself.
            std::function<void(INodePtr, TCompositeSchedulerElementPtr)> parseConfig =
                [&] (INodePtr configNode, TCompositeSchedulerElementPtr parent) {
                    auto configMap = configNode->AsMap();
                    for (const auto& pair : configMap->GetChildren()) {
                        const auto& childId = pair.first;
                        const auto& childNode = pair.second;
                        auto childPath = childNode->GetPath();
                        if (!poolIdToPath.insert(std::make_pair(childId, childPath)).second) {
                            errors.emplace_back(
                                "Pool %Qv is defined both at %v and %v; skipping second occurrence",
                                childId,
                                poolIdToPath[childId],
                                childPath);
                            continue;
                        }

                        // Parse config.
                        auto poolConfigNode = ConvertToNode(childNode->Attributes());
                        TPoolConfigPtr poolConfig;
                        try {
                            poolConfig = ConvertTo<TPoolConfigPtr>(poolConfigNode);
                        } catch (const std::exception& ex) {
                            errors.emplace_back(
                                TError(
                                    "Error parsing configuration of pool %Qv; using defaults",
                                    childPath)
                                << ex);
                            poolConfig = New<TPoolConfig>();
                        }

                        try {
                            poolConfig->Validate();
                        } catch (const std::exception& ex) {
                            errors.emplace_back(
                                TError(
                                    "Misconfiguration of pool %Qv found",
                                    childPath)
                                << ex);
                        }

                        auto pool = FindPool(childId);
                        if (pool) {
                            // Reconfigure existing pool.
                            ReconfigurePool(pool, poolConfig);
                            YCHECK(orphanPoolIds.erase(childId) == 1);
                        } else {
                            // Create new pool.
                            pool = New<TPool>(
                                Host,
                                childId,
                                poolConfig,
                                /* defaultConfigured */ false,
                                Config,
                                GetPoolProfilingTag(childId),
                                TreeId);
                            RegisterPool(pool, parent);
                        }
                        SetPoolParent(pool, parent);

                        if (parent->GetMode() == ESchedulingMode::Fifo) {
                            parent->SetMode(ESchedulingMode::FairShare);
                            errors.emplace_back(
                                TError(
                                    "Pool %Qv cannot have subpools since it is in %Qlv mode",
                                    parent->GetId(),
                                    ESchedulingMode::Fifo));
                        }

                        // Parse children.
                        parseConfig(childNode, pool.Get());
                    }
                };

            // Run recursive descent parsing.
            parseConfig(poolsNode, RootElement);

            // Unregister orphan pools.
            for (const auto& id : orphanPoolIds) {
                auto pool = GetPool(id);
                if (pool->IsEmpty()) {
                    UnregisterPool(pool);
                } else {
                    pool->SetDefaultConfig();
                    SetPoolDefaultParent(pool);
                }
            }

            ResetTreeIndexes();
            RootElement->Update(GlobalDynamicAttributes_);
            RootElementSnapshot = CreateRootElementSnapshot();
        } catch (const std::exception& ex) {
            auto error = TError("Error updating pools in tree %Qv", TreeId)
                << ex;
            LastPoolsNodeUpdateError = error;
            return {error, true};
        }

        if (!errors.empty()) {
            auto combinedError = TError("Found pool configuration issues in tree %Qv", TreeId)
                << std::move(errors);
            LastPoolsNodeUpdateError = combinedError;
            return {combinedError, true};
        }

        LastPoolsNodeUpdateError = TError();

        return {LastPoolsNodeUpdateError, true};
    }

    bool ChangeOperationPool(
        const TOperationId& operationId,
        const TFairShareStrategyOperationStatePtr& state,
        const TPoolName& newPool)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto element = FindOperationElement(operationId);
        if (!element) {
            THROW_ERROR_EXCEPTION("Operation element for operation %Qv not found", operationId);
        }

        LOG_INFO("Operation is changing operation pool (OperationId: %v, OldPool: %v NewPool: %v)",
            operationId,
            element->GetParent()->GetId(),
            newPool.GetPool());

        auto wasActive = DetachOperation(state, element);
        YCHECK(AttachOperation(state, element, newPool));
        return wasActive;
    }

    TError CheckOperationUnschedulable(
        const TOperationId& operationId,
        TDuration safeTimeout,
        int minScheduleJobCallAttempts)
    {
        // TODO(ignat): Could we guarantee that operation must be in tree?
        auto element = FindOperationElement(operationId);
        if (!element) {
            return TError();
        }

        auto now = TInstant::Now();
        TInstant activationTime;

        auto it = OperationIdToActivationTime_.find(operationId);
        if (!GetGlobalDynamicAttributes(element).Active) {
            if (it != OperationIdToActivationTime_.end()) {
                it->second = TInstant::Max();
            }
            return TError();
        } else {
            if (it == OperationIdToActivationTime_.end()) {
                activationTime = now;
                OperationIdToActivationTime_.emplace(operationId, now);
            } else {
                it->second = std::min(it->second, now);
                activationTime = it->second;
            }
        }

        int deactivationCount = 0;
        auto deactivationReasons = element->GetDeactivationReasons();
        for (auto reason : TEnumTraits<EDeactivationReason>::GetDomainValues()) {
            deactivationCount += deactivationReasons[reason];
        }

        if (element->GetScheduledJobCount() == 0 &&
            activationTime + safeTimeout < now &&
            deactivationCount > minScheduleJobCallAttempts)
        {
            return TError("Operation has no successfull scheduled jobs for a long period")
                << TErrorAttribute("period", safeTimeout)
                << TErrorAttribute("unsuccessfull_schedule_job_calls", deactivationCount);
        }

        return TError();
    }

    void UpdateOperationRuntimeParameters(
        const TOperationId& operationId,
        const TOperationFairShareTreeRuntimeParametersPtr& runtimeParams)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& element = FindOperationElement(operationId);
        if (element) {
            element->SetRuntimeParams(runtimeParams);
        }
    }

    void UpdateConfig(const TFairShareStrategyTreeConfigPtr& config)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        Config = config;
        RootElement->UpdateTreeConfig(Config);
    }

    void UpdateControllerConfig(const TFairShareStrategyOperationControllerConfigPtr& config)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        ControllerConfig = config;

        for (const auto& pair : OperationIdToElement) {
            const auto& element = pair.second;
            element->UpdateControllerConfig(config);
        }
    }

    void BuildOperationAttributes(const TOperationId& operationId, TFluentMap fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& element = GetOperationElement(operationId);
        auto serializedParams = ConvertToAttributes(element->GetRuntimeParams());
        fluent
            .Items(*serializedParams)
            .Item("pool").Value(element->GetParent()->GetId());
    }

    void BuildOperationProgress(const TOperationId& operationId, TFluentMap fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& element = FindOperationElement(operationId);
        if (!element) {
            return;
        }

        auto* parent = element->GetParent();
        fluent
            .Item("pool").Value(parent->GetId())
            .Item("slot_index").Value(element->GetSlotIndex())
            .Item("start_time").Value(element->GetStartTime())
            .Item("preemptable_job_count").Value(element->GetPreemptableJobCount())
            .Item("aggressively_preemptable_job_count").Value(element->GetAggressivelyPreemptableJobCount())
            .Item("fifo_index").Value(element->Attributes().FifoIndex)
            .Item("deactivation_reasons").Value(element->GetDeactivationReasons())
            .Do(std::bind(&TFairShareTree::BuildElementYson, this, element, std::placeholders::_1));
    }

    void BuildBriefOperationProgress(const TOperationId& operationId, TFluentMap fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& element = FindOperationElement(operationId);
        if (!element) {
            return;
        }

        auto* parent = element->GetParent();
        const auto& attributes = element->Attributes();
        fluent
            .Item("pool").Value(parent->GetId())
            .Item("weight").Value(element->GetWeight())
            .Item("fair_share_ratio").Value(attributes.FairShareRatio);
    }

    void BuildUserToEphemeralPools(TFluentAny fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        fluent
            .DoMapFor(UserToEphemeralPools, [] (TFluentMap fluent, const auto& value) {
                fluent
                    .Item(value.first).Value(value.second);
            });
    }

    // NB: This function is public for testing purposes.
    TError OnFairShareUpdateAt(TInstant now)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        TError error;

        // Run periodic update.
        PROFILE_AGGREGATED_TIMING(FairShareUpdateTimeCounter) {
            // The root element gets the whole cluster.
            ResetTreeIndexes();
            RootElement->Update(GlobalDynamicAttributes_);

            // Collect alerts after update.
            std::vector<TError> alerts;

            for (const auto& pair : Pools) {
                const auto& poolAlerts = pair.second->UpdateFairShareAlerts();
                alerts.insert(alerts.end(), poolAlerts.begin(), poolAlerts.end());
            }

            const auto& rootElementAlerts = RootElement->UpdateFairShareAlerts();
            alerts.insert(alerts.end(), rootElementAlerts.begin(), rootElementAlerts.end());

            if (!alerts.empty()) {
                error = TError("Found pool configuration issues during fair share update in tree %Qv", TreeId)
                    << std::move(alerts);
            }

            // Update starvation flags for all operations.
            for (const auto& pair : OperationIdToElement) {
                pair.second->CheckForStarvation(now);
            }

            // Update starvation flags for all pools.
            if (Config->EnablePoolStarvation) {
                for (const auto& pair : Pools) {
                    pair.second->CheckForStarvation(now);
                }
            }

            RootElementSnapshot = CreateRootElementSnapshot();
        }

        return error;
    }

    void ProfileFairShare() const
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        for (const auto& pair : Pools) {
            ProfileCompositeSchedulerElement(pair.second);
        }
        ProfileCompositeSchedulerElement(RootElement);
        if (Config->EnableOperationsProfiling) {
            for (const auto& pair : OperationIdToElement) {
                ProfileOperationElement(pair.second);
            }
        }
    }

    void ResetTreeIndexes()
    {
        for (const auto& pair : OperationIdToElement) {
            auto& element = pair.second;
            element->SetTreeIndex(UnassignedTreeIndex);
        }
    }

    void LogOperationsInfo()
    {
        for (const auto& pair : OperationIdToElement) {
            const auto& operationId = pair.first;
            const auto& element = pair.second;
            LOG_DEBUG("FairShareInfo: %v (OperationId: %v)",
                element->GetLoggingString(GlobalDynamicAttributes_),
                operationId);
        }
    }

    void LogPoolsInfo()
    {
        for (const auto& pair : Pools) {
            const auto& poolName = pair.first;
            const auto& pool = pair.second;
            LOG_DEBUG("FairShareInfo: %v (Pool: %v)",
                pool->GetLoggingString(GlobalDynamicAttributes_),
                poolName);
        }
    }

    // NB: This function is public for testing purposes.
    void OnFairShareLoggingAt(TInstant now)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        PROFILE_AGGREGATED_TIMING(FairShareLogTimeCounter) {
            // Log pools information.
            Host->LogEventFluently(ELogEventType::FairShareInfo, now)
                .Item("tree_id").Value(TreeId)
                .Do(BIND(&TFairShareTree::BuildFairShareInfo, Unretained(this)));

            LogOperationsInfo();
        }
    }

    // NB: This function is public for testing purposes.
    void OnFairShareEssentialLoggingAt(TInstant now)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        PROFILE_AGGREGATED_TIMING(FairShareLogTimeCounter) {
            // Log pools information.
            Host->LogEventFluently(ELogEventType::FairShareInfo, now)
                .Item("tree_id").Value(TreeId)
                .Do(BIND(&TFairShareTree::BuildEssentialFairShareInfo, Unretained(this)));

            LogOperationsInfo();
        }
    }

    void RegisterJobs(const TOperationId& operationId, const std::vector<TJobPtr>& jobs)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& element = FindOperationElement(operationId);
        for (const auto& job : jobs) {
            element->OnJobStarted(job->GetId(), job->ResourceUsage(), /* force */ true);
        }
    }

    void BuildPoolsInformation(TFluentMap fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto buildPoolInfo = [=] (const TCompositeSchedulerElementPtr pool, TFluentMap fluent) {
            const auto& id = pool->GetId();
            fluent
                .Item(id).BeginMap()
                    .Item("mode").Value(pool->GetMode())
                    .Item("running_operation_count").Value(pool->RunningOperationCount())
                    .Item("operation_count").Value(pool->OperationCount())
                    .Item("max_running_operation_count").Value(pool->GetMaxRunningOperationCount())
                    .Item("max_operation_count").Value(pool->GetMaxOperationCount())
                    .Item("aggressive_starvation_enabled").Value(pool->IsAggressiveStarvationEnabled())
                    .Item("forbid_immediate_operations").Value(pool->AreImmediateOperationsForbidden())
                    .DoIf(pool->GetMode() == ESchedulingMode::Fifo, [&] (TFluentMap fluent) {
                        fluent
                            .Item("fifo_sort_parameters").Value(pool->GetFifoSortParameters());
                    })
                    .DoIf(pool->GetParent(), [&] (TFluentMap fluent) {
                        fluent
                            .Item("parent").Value(pool->GetParent()->GetId());
                    })
                    .Do(std::bind(&TFairShareTree::BuildElementYson, this, pool, std::placeholders::_1))
                .EndMap();
        };

        fluent
            .Item("pools").BeginMap()
                .DoFor(Pools, [&] (TFluentMap fluent, const TPoolMap::value_type& pair) {
                    buildPoolInfo(pair.second, fluent);
                })
                .Do(std::bind(buildPoolInfo, RootElement, std::placeholders::_1))
            .EndMap();
    }

    void BuildStaticPoolsInformation(TFluentAny fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        fluent
            .DoMapFor(Pools, [&] (TFluentMap fluent, const auto& pair) {
                const auto& id = pair.first;
                const auto& pool = pair.second;
                fluent
                    .Item(id).Value(pool->GetConfig());
            });
    }

    void BuildOrchid(TFluentMap fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        fluent
            .Item("resource_usage").Value(RootElement->GetLocalResourceUsage());
    }

    void BuildFairShareInfo(TFluentMap fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        fluent
            .Do(BIND(&TFairShareTree::BuildPoolsInformation, Unretained(this)))
            .Item("operations").DoMapFor(
                OperationIdToElement,
                [=] (TFluentMap fluent, const TOperationElementPtrByIdMap::value_type& pair) {
                    const auto& operationId = pair.first;
                    fluent
                        .Item(ToString(operationId)).BeginMap()
                            .Do(BIND(&TFairShareTree::BuildOperationProgress, Unretained(this), operationId))
                        .EndMap();
                });
    }

    void BuildEssentialFairShareInfo(TFluentMap fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        fluent
            .Do(BIND(&TFairShareTree::BuildEssentialPoolsInformation, Unretained(this)))
            .Item("operations").DoMapFor(
                OperationIdToElement,
                [=] (TFluentMap fluent, const TOperationElementPtrByIdMap::value_type& pair) {
                    const auto& operationId = pair.first;
                    fluent
                        .Item(ToString(operationId)).BeginMap()
                            .Do(BIND(&TFairShareTree::BuildEssentialOperationProgress, Unretained(this), operationId))
                        .EndMap();
                });
    }

    void ResetState()
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        LastPoolsNodeUpdate.Reset();
        LastPoolsNodeUpdateError = TError();
    }

    const TSchedulingTagFilter& GetNodesFilter() const
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        return Config->NodesFilter;
    }

    TPoolName MakeAppropriatePoolName(const TNullable<TString>& specPool, const TString& user)
    {
        if (!specPool) {
            return TPoolName(user, Null);
        }
        TPoolPtr pool = FindPool(specPool.Get());
        if (pool && pool->GetConfig()->CreateEphemeralSubpools) {
            return TPoolName(user, specPool.Get());
        }
        return TPoolName(specPool.Get(), Null);
    };

private:
    TFairShareStrategyTreeConfigPtr Config;
    TFairShareStrategyOperationControllerConfigPtr ControllerConfig;
    ISchedulerStrategyHost* const Host;

    std::vector<IInvokerPtr> FeasibleInvokers;

    INodePtr LastPoolsNodeUpdate;
    TError LastPoolsNodeUpdateError;

    const TString TreeId;
    const TTagId TreeIdProfilingTag;

    const NLogging::TLogger Logger;

    using TPoolMap = THashMap<TString, TPoolPtr>;
    TPoolMap Pools;

    THashMap<TString, NProfiling::TTagId> PoolIdToProfilingTagId;

    THashMap<TString, THashSet<TString>> UserToEphemeralPools;

    THashMap<TString, THashSet<int>> PoolToSpareSlotIndices;
    THashMap<TString, int> PoolToMinUnusedSlotIndex;

    using TOperationElementPtrByIdMap = THashMap<TOperationId, TOperationElementPtr>;
    TOperationElementPtrByIdMap OperationIdToElement;

    THashMap<TOperationId, TInstant> OperationIdToActivationTime_;

    std::list<TOperationId> WaitingOperationQueue;

    TReaderWriterSpinLock NodeIdToLastPreemptiveSchedulingTimeLock;
    THashMap<TNodeId, TCpuInstant> NodeIdToLastPreemptiveSchedulingTime;

    std::vector<TSchedulingTagFilter> RegisteredSchedulingTagFilters;
    std::vector<int> FreeSchedulingTagFilterIndexes;
    struct TSchedulingTagFilterEntry
    {
        int Index;
        int Count;
    };
    THashMap<TSchedulingTagFilter, TSchedulingTagFilterEntry> SchedulingTagFilterToIndexAndCount;

    TRootElementPtr RootElement;

    struct TRootElementSnapshot
        : public TIntrinsicRefCounted
    {
        TRootElementPtr RootElement;
        TOperationElementByIdMap OperationIdToElement;
        TFairShareStrategyTreeConfigPtr Config;
        std::vector<TSchedulingTagFilter> RegisteredSchedulingTagFilters;

        TOperationElement* FindOperationElement(const TOperationId& operationId) const
        {
            auto it = OperationIdToElement.find(operationId);
            return it != OperationIdToElement.end() ? it->second : nullptr;
        }
    };

    typedef TIntrusivePtr<TRootElementSnapshot> TRootElementSnapshotPtr;
    TRootElementSnapshotPtr RootElementSnapshot;

    class TFairShareTreeSnapshot
        : public IFairShareTreeSnapshot
    {
    public:
        TFairShareTreeSnapshot(TFairShareTreePtr tree, TRootElementSnapshotPtr rootElementSnapshot, const NLogging::TLogger& logger)
            : Tree(std::move(tree))
            , RootElementSnapshot(std::move(rootElementSnapshot))
            , Logger(logger)
            , NodesFilter(Tree->GetNodesFilter())
        { }

        virtual TFuture<void> ScheduleJobs(const ISchedulingContextPtr& schedulingContext) override
        {
            return BIND(&TFairShareTree::DoScheduleJobs,
                Tree,
                schedulingContext,
                RootElementSnapshot)
                .AsyncVia(GetCurrentInvoker())
                .Run();
        }

        virtual void ProcessUpdatedJob(const TOperationId& operationId, const TJobId& jobId, const TJobResources& delta)
        {
            // XXX(ignat): remove before deploy on production clusters.
            LOG_DEBUG("Processing updated job (OperationId: %v, JobId: %v)", operationId, jobId);
            auto* operationElement = RootElementSnapshot->FindOperationElement(operationId);
            if (operationElement) {
                operationElement->IncreaseJobResourceUsage(jobId, delta);
            }
        }

        virtual void ProcessFinishedJob(const TOperationId& operationId, const TJobId& jobId) override
        {
            // XXX(ignat): remove before deploy on production clusters.
            LOG_DEBUG("Processing finished job (OperationId: %v, JobId: %v)", operationId, jobId);
            auto* operationElement = RootElementSnapshot->FindOperationElement(operationId);
            if (operationElement) {
                operationElement->OnJobFinished(jobId);
            }
        }

        virtual void ApplyJobMetricsDelta(const TOperationId& operationId, const TJobMetrics& jobMetricsDelta) override
        {
            auto* operationElement = RootElementSnapshot->FindOperationElement(operationId);
            if (operationElement) {
                operationElement->ApplyJobMetricsDelta(jobMetricsDelta);
            }
        }

        virtual bool HasOperation(const TOperationId& operationId) const override
        {
            auto* operationElement = RootElementSnapshot->FindOperationElement(operationId);
            return operationElement != nullptr;
        }

        virtual const TSchedulingTagFilter& GetNodesFilter() const override
        {
            return NodesFilter;
        }

    private:
        const TIntrusivePtr<TFairShareTree> Tree;
        const TRootElementSnapshotPtr RootElementSnapshot;
        const NLogging::TLogger Logger;
        const TSchedulingTagFilter NodesFilter;
    };

    TDynamicAttributesList GlobalDynamicAttributes_;

    struct TProfilingCounters
    {
        TProfilingCounters(const TString& prefix, const TTagId& treeIdProfilingTag)
            : PrescheduleJobTime(prefix + "/preschedule_job_time", {treeIdProfilingTag})
            , TotalControllerScheduleJobTime(prefix + "/controller_schedule_job_time/total", {treeIdProfilingTag})
            , ExecControllerScheduleJobTime(prefix + "/controller_schedule_job_time/exec", {treeIdProfilingTag})
            , StrategyScheduleJobTime(prefix + "/strategy_schedule_job_time", {treeIdProfilingTag})
            , ScheduleJobCount(prefix + "/schedule_job_count", {treeIdProfilingTag})
            , ScheduleJobFailureCount(prefix + "/schedule_job_failure_count", {treeIdProfilingTag})
        {
            for (auto reason : TEnumTraits<EScheduleJobFailReason>::GetDomainValues())
            {
                auto tags = GetFailReasonProfilingTags(reason);
                tags.push_back(treeIdProfilingTag);

                ControllerScheduleJobFail[reason] = TMonotonicCounter(
                    prefix + "/controller_schedule_job_fail",
                    tags);
            }
        }

        TAggregateGauge PrescheduleJobTime;
        TAggregateGauge TotalControllerScheduleJobTime;
        TAggregateGauge ExecControllerScheduleJobTime;
        TAggregateGauge StrategyScheduleJobTime;
        TMonotonicCounter ScheduleJobCount;
        TMonotonicCounter ScheduleJobFailureCount;
        TEnumIndexedVector<TMonotonicCounter, EScheduleJobFailReason> ControllerScheduleJobFail;
    };

    TProfilingCounters NonPreemptiveProfilingCounters;
    TProfilingCounters PreemptiveProfilingCounters;

    TAggregateGauge FairShareUpdateTimeCounter;
    TAggregateGauge FairShareLogTimeCounter;
    TAggregateGauge AnalyzePreemptableJobsTimeCounter;

    TCpuInstant LastSchedulingInformationLoggedTime_ = 0;

    TDynamicAttributes GetGlobalDynamicAttributes(const TSchedulerElementPtr& element) const
    {
        int index = element->GetTreeIndex();
        if (index == UnassignedTreeIndex) {
            return TDynamicAttributes();
        } else {
            YCHECK(index < GlobalDynamicAttributes_.size());
            return GlobalDynamicAttributes_[index];
        }
    }

    void DoScheduleJobsWithoutPreemption(
        const TRootElementSnapshotPtr& rootElementSnapshot,
        TFairShareContext* context,
        TCpuInstant startTime,
        const std::function<void(TProfilingCounters&, int, TDuration)> profileTimings,
        const std::function<void(TStringBuf)> logAndCleanSchedulingStatistics)
    {
        auto& rootElement = rootElementSnapshot->RootElement;

        {
            LOG_TRACE("Scheduling new jobs");

            bool prescheduleExecuted = false;
            TDuration prescheduleDuration;

            TWallTimer scheduleTimer;
            while (context->SchedulingContext->CanStartMoreJobs() &&
                GetCpuInstant() < startTime + DurationToCpuDuration(ControllerConfig->ScheduleJobsTimeout))
            {
                if (!prescheduleExecuted) {
                    TWallTimer prescheduleTimer;
                    context->Initialize(rootElement->GetTreeSize(), RegisteredSchedulingTagFilters);
                    rootElement->PrescheduleJob(context, /*starvingOnly*/ false, /*aggressiveStarvationEnabled*/ false);
                    prescheduleDuration = prescheduleTimer.GetElapsedTime();
                    Profiler.Update(NonPreemptiveProfilingCounters.PrescheduleJobTime, DurationToCpuDuration(prescheduleDuration));
                    prescheduleExecuted = true;
                    context->PrescheduledCalled = true;
                }
                ++context->SchedulingStatistics.NonPreemptiveScheduleJobAttempts;
                if (!rootElement->ScheduleJob(context)) {
                    break;
                }
            }
            profileTimings(
                NonPreemptiveProfilingCounters,
                context->SchedulingStatistics.NonPreemptiveScheduleJobAttempts,
                scheduleTimer.GetElapsedTime() - prescheduleDuration - context->TotalScheduleJobDuration);

            if (context->SchedulingStatistics.NonPreemptiveScheduleJobAttempts > 0) {
                logAndCleanSchedulingStatistics(AsStringBuf("Non preemptive"));
            }
        }
    }

    void DoScheduleJobsWithPreemption(
        const TRootElementSnapshotPtr& rootElementSnapshot,
        TFairShareContext* context,
        TCpuInstant startTime,
        const std::function<void(TProfilingCounters&, int, TDuration)>& profileTimings,
        const std::function<void(TStringBuf)>& logAndCleanSchedulingStatistics)
    {
        auto& rootElement = rootElementSnapshot->RootElement;
        auto& config = rootElementSnapshot->Config;

        if (!context->Initialized) {
            context->Initialize(rootElement->GetTreeSize(), RegisteredSchedulingTagFilters);
        }

        if (!context->PrescheduledCalled) {
            context->SchedulingStatistics.HasAggressivelyStarvingNodes = rootElement->HasAggressivelyStarvingNodes(context, false);
        }

        // Compute discount to node usage.
        LOG_TRACE("Looking for preemptable jobs");
        THashSet<TCompositeSchedulerElementPtr> discountedPools;
        std::vector<TJobPtr> preemptableJobs;
        PROFILE_AGGREGATED_TIMING(AnalyzePreemptableJobsTimeCounter) {
            for (const auto& job : context->SchedulingContext->RunningJobs()) {
                auto* operationElement = rootElementSnapshot->FindOperationElement(job->GetOperationId());
                if (!operationElement || !operationElement->IsJobKnown(job->GetId())) {
                    LOG_DEBUG("Dangling running job found (JobId: %v, OperationId: %v)",
                        job->GetId(),
                        job->GetOperationId());
                    continue;
                }

                if (!operationElement->IsPreemptionAllowed(*context, config)) {
                    continue;
                }

                bool aggressivePreemptionEnabled = context->SchedulingStatistics.HasAggressivelyStarvingNodes &&
                    operationElement->IsAggressiveStarvationPreemptionAllowed();
                if (operationElement->IsJobPreemptable(job->GetId(), aggressivePreemptionEnabled)) {
                    auto* parent = operationElement->GetParent();
                    while (parent) {
                        discountedPools.insert(parent);
                        context->DynamicAttributes(parent).ResourceUsageDiscount += job->ResourceUsage();
                        parent = parent->GetParent();
                    }
                    context->SchedulingContext->ResourceUsageDiscount() += job->ResourceUsage();
                    preemptableJobs.push_back(job);
                }
            }
        }

        context->SchedulingStatistics.ResourceUsageDiscount = context->SchedulingContext->ResourceUsageDiscount();

        int startedBeforePreemption = context->SchedulingContext->StartedJobs().size();

        // NB: Schedule at most one job with preemption.
        TJobPtr jobStartedUsingPreemption;
        {
            LOG_TRACE("Scheduling new jobs with preemption");

            // Clean data from previous profiling.
            context->TotalScheduleJobDuration = TDuration::Zero();
            context->ExecScheduleJobDuration = TDuration::Zero();
            context->ScheduleJobFailureCount = 0;
            std::fill(context->FailedScheduleJob.begin(), context->FailedScheduleJob.end(), 0);

            bool prescheduleExecuted = false;
            TDuration prescheduleDuration;

            TWallTimer timer;
            while (context->SchedulingContext->CanStartMoreJobs() &&
                GetCpuInstant() < startTime + DurationToCpuDuration(ControllerConfig->ScheduleJobsTimeout))
            {
                if (!prescheduleExecuted) {
                    TWallTimer prescheduleTimer;
                    rootElement->PrescheduleJob(context, /*starvingOnly*/ true, /*aggressiveStarvationEnabled*/ false);
                    prescheduleDuration = prescheduleTimer.GetElapsedTime();
                    Profiler.Update(PreemptiveProfilingCounters.PrescheduleJobTime, DurationToCpuDuration(prescheduleDuration));
                    prescheduleExecuted = true;
                }

                ++context->SchedulingStatistics.PreemptiveScheduleJobAttempts;
                if (!rootElement->ScheduleJob(context)) {
                    break;
                }
                if (context->SchedulingContext->StartedJobs().size() > startedBeforePreemption) {
                    jobStartedUsingPreemption = context->SchedulingContext->StartedJobs().back();
                    break;
                }
            }
            profileTimings(
                PreemptiveProfilingCounters,
                context->SchedulingStatistics.PreemptiveScheduleJobAttempts,
                timer.GetElapsedTime() - prescheduleDuration - context->TotalScheduleJobDuration);
            if (context->SchedulingStatistics.PreemptiveScheduleJobAttempts > 0) {
                logAndCleanSchedulingStatistics(AsStringBuf("Preemptive"));
            }
        }

        int startedAfterPreemption = context->SchedulingContext->StartedJobs().size();

        context->SchedulingStatistics.ScheduledDuringPreemption = startedAfterPreemption - startedBeforePreemption;

        // Reset discounts.
        context->SchedulingContext->ResourceUsageDiscount() = ZeroJobResources();
        for (const auto& pool : discountedPools) {
            context->DynamicAttributes(pool.Get()).ResourceUsageDiscount = ZeroJobResources();
        }

        // Preempt jobs if needed.
        std::sort(
            preemptableJobs.begin(),
            preemptableJobs.end(),
            [] (const TJobPtr& lhs, const TJobPtr& rhs) {
                return lhs->GetStartTime() > rhs->GetStartTime();
            });

        auto findPoolWithViolatedLimitsForJob = [&] (const TJobPtr& job) -> TCompositeSchedulerElement* {
            auto* operationElement = rootElementSnapshot->FindOperationElement(job->GetOperationId());
            if (!operationElement) {
                return nullptr;
            }

            auto* parent = operationElement->GetParent();
            while (parent) {
                if (!Dominates(parent->ResourceLimits(), parent->GetLocalResourceUsage())) {
                    return parent;
                }
                parent = parent->GetParent();
            }
            return nullptr;
        };

        auto findOperationElementForJob = [&] (const TJobPtr& job) -> TOperationElement* {
            auto operationElement = rootElementSnapshot->FindOperationElement(job->GetOperationId());
            if (!operationElement || !operationElement->IsJobKnown(job->GetId())) {
                LOG_DEBUG("Dangling preemptable job found (JobId: %v, OperationId: %v)",
                    job->GetId(),
                    job->GetOperationId());

                return nullptr;
            }

            return operationElement;
        };

        context->SchedulingStatistics.PreemptableJobCount = preemptableJobs.size();

        int currentJobIndex = 0;
        for (; currentJobIndex < preemptableJobs.size(); ++currentJobIndex) {
            if (Dominates(context->SchedulingContext->ResourceLimits(), context->SchedulingContext->ResourceUsage())) {
                break;
            }

            const auto& job = preemptableJobs[currentJobIndex];
            auto operationElement = findOperationElementForJob(job);
            if (!operationElement) {
                continue;
            }

            if (jobStartedUsingPreemption) {
                job->SetPreemptionReason(Format("Preempted to start job %v of operation %v",
                    jobStartedUsingPreemption->GetId(),
                    jobStartedUsingPreemption->GetOperationId()));
            } else {
                job->SetPreemptionReason(Format("Node resource limits violated"));
            }
            PreemptJob(job, operationElement, context);
        }

        for (; currentJobIndex < preemptableJobs.size(); ++currentJobIndex) {
            const auto& job = preemptableJobs[currentJobIndex];

            auto operationElement = findOperationElementForJob(job);
            if (!operationElement) {
                continue;
            }

            if (!Dominates(operationElement->ResourceLimits(), operationElement->GetLocalResourceUsage())) {
                job->SetPreemptionReason(Format("Preempted due to violation of resource limits of operation %v",
                    operationElement->GetId()));
                PreemptJob(job, operationElement, context);
                continue;
            }

            auto violatedPool = findPoolWithViolatedLimitsForJob(job);
            if (violatedPool) {
                job->SetPreemptionReason(Format("Preempted due to violation of limits on pool %v",
                    violatedPool->GetId()));
                PreemptJob(job, operationElement, context);
            }
        }
    }

    void DoScheduleJobs(
        const ISchedulingContextPtr& schedulingContext,
        const TRootElementSnapshotPtr& rootElementSnapshot)
    {
        TFairShareContext context(schedulingContext);

        auto profileTimings = [&] (
            TProfilingCounters& counters,
            int scheduleJobCount,
            TDuration scheduleJobDurationWithoutControllers)
        {
            Profiler.Update(
                counters.StrategyScheduleJobTime,
                scheduleJobDurationWithoutControllers.MicroSeconds());

            Profiler.Update(
                counters.TotalControllerScheduleJobTime,
                context.TotalScheduleJobDuration.MicroSeconds());

            Profiler.Update(
                counters.ExecControllerScheduleJobTime,
                context.ExecScheduleJobDuration.MicroSeconds());

            Profiler.Increment(counters.ScheduleJobCount, scheduleJobCount);
            Profiler.Increment(counters.ScheduleJobFailureCount, context.ScheduleJobFailureCount);

            for (auto reason : TEnumTraits<EScheduleJobFailReason>::GetDomainValues()) {
                Profiler.Increment(
                    counters.ControllerScheduleJobFail[reason],
                    context.FailedScheduleJob[reason]);
            }
        };

        bool enableSchedulingInfoLogging = false;
        auto now = GetCpuInstant();
        const auto& config = rootElementSnapshot->Config;
        if (LastSchedulingInformationLoggedTime_ + DurationToCpuDuration(config->HeartbeatTreeSchedulingInfoLogBackoff) < now) {
            enableSchedulingInfoLogging = true;
            LastSchedulingInformationLoggedTime_ = now;
        }

        auto logAndCleanSchedulingStatistics = [&] (TStringBuf stageName) {
            if (!enableSchedulingInfoLogging) {
                return;
            }
            LOG_DEBUG("%v scheduling statistics (ActiveTreeSize: %v, ActiveOperationCount: %v, DeactivationReasons: %v, CanStartMoreJobs: %v, Address: %v)",
                stageName,
                context.ActiveTreeSize,
                context.ActiveOperationCount,
                context.DeactivationReasons,
                schedulingContext->CanStartMoreJobs(),
                schedulingContext->GetNodeDescriptor().Address);
            context.ActiveTreeSize = 0;
            context.ActiveOperationCount = 0;
            std::fill(context.DeactivationReasons.begin(), context.DeactivationReasons.end(), 0);
        };

        DoScheduleJobsWithoutPreemption(rootElementSnapshot, &context, now, profileTimings, logAndCleanSchedulingStatistics);

        auto nodeId = schedulingContext->GetNodeDescriptor().Id;

        bool scheduleJobsWithPreemption = false;
        {
            bool nodeIsMissing = false;
            {
                TReaderGuard guard(NodeIdToLastPreemptiveSchedulingTimeLock);
                auto it = NodeIdToLastPreemptiveSchedulingTime.find(nodeId);
                if (it == NodeIdToLastPreemptiveSchedulingTime.end()) {
                    nodeIsMissing = true;
                    scheduleJobsWithPreemption = true;
                } else if (it->second + DurationToCpuDuration(config->PreemptiveSchedulingBackoff) <= now) {
                    scheduleJobsWithPreemption = true;
                    it->second = now;
                }
            }
            if (nodeIsMissing) {
                TWriterGuard guard(NodeIdToLastPreemptiveSchedulingTimeLock);
                NodeIdToLastPreemptiveSchedulingTime[nodeId] = now;
            }
        }

        if (scheduleJobsWithPreemption) {
            DoScheduleJobsWithPreemption(rootElementSnapshot, &context, now, profileTimings, logAndCleanSchedulingStatistics);
        } else {
            LOG_DEBUG("Skip preemptive scheduling");
        }

        schedulingContext->SetSchedulingStatistics(context.SchedulingStatistics);
    }

    void PreemptJob(
        const TJobPtr& job,
        const TOperationElementPtr& operationElement,
        TFairShareContext* context) const
    {
        context->SchedulingContext->ResourceUsage() -= job->ResourceUsage();
        operationElement->IncreaseJobResourceUsage(job->GetId(), -job->ResourceUsage());
        job->ResourceUsage() = ZeroJobResources();

        context->SchedulingContext->PreemptJob(job);
    }

    TCompositeSchedulerElement* FindPoolViolatingMaxRunningOperationCount(TCompositeSchedulerElement* pool)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        while (pool) {
            if (pool->RunningOperationCount() >= pool->GetMaxRunningOperationCount()) {
                return pool;
            }
            pool = pool->GetParent();
        }
        return nullptr;
    }

    TCompositeSchedulerElementPtr FindPoolWithViolatedOperationCountLimit(const TCompositeSchedulerElementPtr& element)
    {
        auto current = element;
        while (current) {
            if (current->OperationCount() >= current->GetMaxOperationCount()) {
                return current;
            }
            current = current->GetParent();
        }
        return nullptr;
    }

    void AddOperationToPool(const TOperationId& operationId)
    {
        TForbidContextSwitchGuard contextSwitchGuard;

        const auto& operationElement = GetOperationElement(operationId);
        auto* parent = operationElement->GetParent();
        parent->IncreaseRunningOperationCount(1);

        LOG_INFO("Operation added to pool (OperationId: %v, Pool: %v)",
            operationId,
            parent->GetId());
    }

    void DoRegisterPool(const TPoolPtr& pool)
    {
        int index = RegisterSchedulingTagFilter(pool->GetSchedulingTagFilter());
        pool->SetSchedulingTagFilterIndex(index);
        YCHECK(Pools.insert(std::make_pair(pool->GetId(), pool)).second);
        YCHECK(PoolToMinUnusedSlotIndex.insert(std::make_pair(pool->GetId(), 0)).second);
    }

    void RegisterPool(const TPoolPtr& pool)
    {
        DoRegisterPool(pool);

        LOG_INFO("Pool registered (Pool: %v)", pool->GetId());
    }

    void RegisterPool(const TPoolPtr& pool, const TCompositeSchedulerElementPtr& parent)
    {
        DoRegisterPool(pool);

        pool->SetParent(parent.Get());
        parent->AddChild(pool);

        LOG_INFO("Pool registered (Pool: %v, Parent: %v)",
            pool->GetId(),
            parent->GetId());
    }

    void ReconfigurePool(const TPoolPtr& pool, const TPoolConfigPtr& config)
    {
        auto oldSchedulingTagFilter = pool->GetSchedulingTagFilter();
        pool->SetConfig(config);
        auto newSchedulingTagFilter = pool->GetSchedulingTagFilter();
        if (oldSchedulingTagFilter != newSchedulingTagFilter) {
            UnregisterSchedulingTagFilter(oldSchedulingTagFilter);
            int index = RegisterSchedulingTagFilter(newSchedulingTagFilter);
            pool->SetSchedulingTagFilterIndex(index);
        }
    }

    void UnregisterPool(const TPoolPtr& pool)
    {
        auto userName = pool->GetUserName();
        if (userName) {
            YCHECK(UserToEphemeralPools[*userName].erase(pool->GetId()) == 1);
        }

        UnregisterSchedulingTagFilter(pool->GetSchedulingTagFilterIndex());

        YCHECK(PoolToMinUnusedSlotIndex.erase(pool->GetId()) == 1);
        YCHECK(PoolToSpareSlotIndices.erase(pool->GetId()) <= 1);
        YCHECK(Pools.erase(pool->GetId()) == 1);

        pool->SetAlive(false);
        auto parent = pool->GetParent();
        SetPoolParent(pool, nullptr);

        LOG_INFO("Pool unregistered (Pool: %v, Parent: %v)",
            pool->GetId(),
            parent->GetId());
    }

    bool TryAllocatePoolSlotIndex(const TString& poolName, int slotIndex)
    {
        auto minUnusedIndexIt = PoolToMinUnusedSlotIndex.find(poolName);
        YCHECK(minUnusedIndexIt != PoolToMinUnusedSlotIndex.end());

        auto& spareSlotIndices = PoolToSpareSlotIndices[poolName];

        if (slotIndex >= minUnusedIndexIt->second) {
            for (int index = minUnusedIndexIt->second; index < slotIndex; ++index) {
                spareSlotIndices.insert(index);
            }

            minUnusedIndexIt->second = slotIndex + 1;

            return true;
        } else {
            return spareSlotIndices.erase(slotIndex) == 1;
        }
    }

    void AllocateOperationSlotIndex(const TFairShareStrategyOperationStatePtr& state, const TString& poolName)
    {
        auto it = PoolToSpareSlotIndices.find(poolName);
        auto slotIndex = state->GetHost()->FindSlotIndex(TreeId);

        if (slotIndex) {
            // Revive case
            if (TryAllocatePoolSlotIndex(poolName, *slotIndex)) {
                return;
            }
            LOG_ERROR("Failed to reuse slot index during revive (OperationId: %v, SlotIndex: %v)",
                state->GetHost()->GetId(),
                *slotIndex);
        }

        if (it == PoolToSpareSlotIndices.end() || it->second.empty()) {
            auto minUnusedIndexIt = PoolToMinUnusedSlotIndex.find(poolName);
            YCHECK(minUnusedIndexIt != PoolToMinUnusedSlotIndex.end());
            slotIndex = minUnusedIndexIt->second;
            ++minUnusedIndexIt->second;
        } else {
            auto spareIndexIt = it->second.begin();
            slotIndex = *spareIndexIt;
            it->second.erase(spareIndexIt);
        }

        state->GetHost()->SetSlotIndex(TreeId, *slotIndex);

        LOG_DEBUG("Operation slot index allocated (OperationId: %v, SlotIndex: %v)",
            state->GetHost()->GetId(),
            *slotIndex);
    }

    void ReleaseOperationSlotIndex(const TFairShareStrategyOperationStatePtr& state, const TString& poolName)
    {
        auto slotIndex = state->GetHost()->FindSlotIndex(TreeId);
        YCHECK(slotIndex);

        auto it = PoolToSpareSlotIndices.find(poolName);
        if (it == PoolToSpareSlotIndices.end()) {
            YCHECK(PoolToSpareSlotIndices.insert(std::make_pair(poolName, THashSet<int>{*slotIndex})).second);
        } else {
            it->second.insert(*slotIndex);
        }

        LOG_DEBUG("Operation slot index released (OperationId: %v, SlotIndex: %v)",
            state->GetHost()->GetId(),
            *slotIndex);
    }

    void TryActivateOperationsFromQueue(std::vector<TOperationId>* operationsToActivate)
    {
        // Try to run operations from queue.
        auto it = WaitingOperationQueue.begin();
        while (it != WaitingOperationQueue.end() && RootElement->RunningOperationCount() < Config->MaxRunningOperationCount) {
            const auto& operationId = *it;
            auto* operationPool = GetOperationElement(operationId)->GetParent();
            if (FindPoolViolatingMaxRunningOperationCount(operationPool) == nullptr) {
                operationsToActivate->push_back(operationId);
                AddOperationToPool(operationId);
                auto toRemove = it++;
                WaitingOperationQueue.erase(toRemove);
            } else {
                ++it;
            }
        }
    }

    void BuildEssentialOperationProgress(const TOperationId& operationId, TFluentMap fluent)
    {
        const auto& element = FindOperationElement(operationId);
        if (!element) {
            return;
        }

        fluent
            .Do(BIND(&TFairShareTree::BuildEssentialOperationElementYson, Unretained(this), element));
    }

    int RegisterSchedulingTagFilter(const TSchedulingTagFilter& filter)
    {
        if (filter.IsEmpty()) {
            return EmptySchedulingTagFilterIndex;
        }
        auto it = SchedulingTagFilterToIndexAndCount.find(filter);
        if (it == SchedulingTagFilterToIndexAndCount.end()) {
            int index;
            if (FreeSchedulingTagFilterIndexes.empty()) {
                index = RegisteredSchedulingTagFilters.size();
                RegisteredSchedulingTagFilters.push_back(filter);
            } else {
                index = FreeSchedulingTagFilterIndexes.back();
                RegisteredSchedulingTagFilters[index] = filter;
                FreeSchedulingTagFilterIndexes.pop_back();
            }
            SchedulingTagFilterToIndexAndCount.emplace(filter, TSchedulingTagFilterEntry({index, 1}));
            return index;
        } else {
            ++it->second.Count;
            return it->second.Index;
        }
    }

    void UnregisterSchedulingTagFilter(int index)
    {
        if (index == EmptySchedulingTagFilterIndex) {
            return;
        }
        UnregisterSchedulingTagFilter(RegisteredSchedulingTagFilters[index]);
    }

    void UnregisterSchedulingTagFilter(const TSchedulingTagFilter& filter)
    {
        if (filter.IsEmpty()) {
            return;
        }
        auto it = SchedulingTagFilterToIndexAndCount.find(filter);
        YCHECK(it != SchedulingTagFilterToIndexAndCount.end());
        --it->second.Count;
        if (it->second.Count == 0) {
            RegisteredSchedulingTagFilters[it->second.Index] = EmptySchedulingTagFilter;
            FreeSchedulingTagFilterIndexes.push_back(it->second.Index);
            SchedulingTagFilterToIndexAndCount.erase(it);
        }
    }

    void SetPoolParent(const TPoolPtr& pool, const TCompositeSchedulerElementPtr& parent)
    {
        if (pool->GetParent() == parent) {
            return;
        }

        auto* oldParent = pool->GetParent();
        if (oldParent) {
            oldParent->IncreaseHierarchicalResourceUsage(-pool->GetLocalResourceUsage());
            oldParent->IncreaseOperationCount(-pool->OperationCount());
            oldParent->IncreaseRunningOperationCount(-pool->RunningOperationCount());
            oldParent->RemoveChild(pool);
        }

        pool->SetParent(parent.Get());
        if (parent) {
            parent->AddChild(pool);
            parent->IncreaseHierarchicalResourceUsage(pool->GetLocalResourceUsage());
            parent->IncreaseOperationCount(pool->OperationCount());
            parent->IncreaseRunningOperationCount(pool->RunningOperationCount());

            LOG_INFO("Parent pool set (Pool: %v, Parent: %v)",
                pool->GetId(),
                parent->GetId());
        }
    }

    void SetPoolDefaultParent(const TPoolPtr& pool)
    {
        auto defaultParentPool = FindPool(Config->DefaultParentPool);
        if (!defaultParentPool || defaultParentPool == pool) {
            // NB: root element is not a pool, so we should suppress warning in this special case.
            if (Config->DefaultParentPool != RootPoolName) {
                auto error = TError("Default parent pool %Qv is not registered", Config->DefaultParentPool);
                Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, error);
            }
            SetPoolParent(pool, RootElement);
        } else {
            SetPoolParent(pool, defaultParentPool);
        }
    }

    TPoolPtr FindPool(const TString& id)
    {
        auto it = Pools.find(id);
        return it == Pools.end() ? nullptr : it->second;
    }

    TPoolPtr GetPool(const TString& id)
    {
        auto pool = FindPool(id);
        YCHECK(pool);
        return pool;
    }

    NProfiling::TTagId GetPoolProfilingTag(const TString& id)
    {
        auto it = PoolIdToProfilingTagId.find(id);
        if (it == PoolIdToProfilingTagId.end()) {
            it = PoolIdToProfilingTagId.emplace(
                id,
                NProfiling::TProfileManager::Get()->RegisterTag("pool", id)
            ).first;
        }
        return it->second;
    }

    TOperationElementPtr FindOperationElement(const TOperationId& operationId)
    {
        auto it = OperationIdToElement.find(operationId);
        return it == OperationIdToElement.end() ? nullptr : it->second;
    }

    TOperationElementPtr GetOperationElement(const TOperationId& operationId)
    {
        auto element = FindOperationElement(operationId);
        YCHECK(element);
        return element;
    }

    TRootElementSnapshotPtr CreateRootElementSnapshot()
    {
        auto snapshot = New<TRootElementSnapshot>();
        snapshot->RootElement = RootElement->Clone();
        snapshot->RootElement->BuildOperationToElementMapping(&snapshot->OperationIdToElement);
        snapshot->RegisteredSchedulingTagFilters = RegisteredSchedulingTagFilters;
        snapshot->Config = Config;
        return snapshot;
    }

    void BuildEssentialPoolsInformation(TFluentMap fluent)
    {
        fluent
            .Item("pools").DoMapFor(Pools, [&] (TFluentMap fluent, const TPoolMap::value_type& pair) {
                const auto& id = pair.first;
                const auto& pool = pair.second;
                fluent
                    .Item(id).BeginMap()
                        .Do(BIND(&TFairShareTree::BuildEssentialPoolElementYson, Unretained(this), pool))
                    .EndMap();
            });
    }

    void BuildElementYson(const TSchedulerElementPtr& element, TFluentMap fluent)
    {
        const auto& attributes = element->Attributes();
        auto dynamicAttributes = GetGlobalDynamicAttributes(element);

        auto guaranteedResources = Host->GetResourceLimits(Config->NodesFilter) * attributes.GuaranteedResourcesRatio;

        fluent
            .Item("scheduling_status").Value(element->GetStatus())
            .Item("starving").Value(element->GetStarving())
            .Item("fair_share_starvation_tolerance").Value(element->GetFairShareStarvationTolerance())
            .Item("min_share_preemption_timeout").Value(element->GetMinSharePreemptionTimeout())
            .Item("fair_share_preemption_timeout").Value(element->GetFairSharePreemptionTimeout())
            .Item("adjusted_fair_share_starvation_tolerance").Value(attributes.AdjustedFairShareStarvationTolerance)
            .Item("adjusted_min_share_preemption_timeout").Value(attributes.AdjustedMinSharePreemptionTimeout)
            .Item("adjusted_fair_share_preemption_timeout").Value(attributes.AdjustedFairSharePreemptionTimeout)
            .Item("resource_demand").Value(element->ResourceDemand())
            .Item("resource_usage").Value(element->GetLocalResourceUsage())
            .Item("resource_limits").Value(element->ResourceLimits())
            .Item("dominant_resource").Value(attributes.DominantResource)
            .Item("weight").Value(element->GetWeight())
            .Item("min_share_ratio").Value(element->GetMinShareRatio())
            .Item("max_share_ratio").Value(element->GetMaxShareRatio())
            .Item("min_share_resources").Value(element->GetMinShareResources())
            .Item("adjusted_min_share_ratio").Value(attributes.AdjustedMinShareRatio)
            .Item("recursive_min_share_ratio").Value(attributes.RecursiveMinShareRatio)
            .Item("guaranteed_resources_ratio").Value(attributes.GuaranteedResourcesRatio)
            .Item("guaranteed_resources").Value(guaranteedResources)
            .Item("max_possible_usage_ratio").Value(attributes.MaxPossibleUsageRatio)
            .Item("usage_ratio").Value(element->GetLocalResourceUsageRatio())
            .Item("demand_ratio").Value(attributes.DemandRatio)
            .Item("fair_share_ratio").Value(attributes.FairShareRatio)
            .Item("satisfaction_ratio").Value(dynamicAttributes.SatisfactionRatio)
            .Item("best_allocation_ratio").Value(attributes.BestAllocationRatio);
    }

    void BuildEssentialElementYson(const TSchedulerElementPtr& element, TFluentMap fluent, bool shouldPrintResourceUsage)
    {
        const auto& attributes = element->Attributes();
        auto dynamicAttributes = GetGlobalDynamicAttributes(element);

        fluent
            .Item("usage_ratio").Value(element->GetLocalResourceUsageRatio())
            .Item("demand_ratio").Value(attributes.DemandRatio)
            .Item("fair_share_ratio").Value(attributes.FairShareRatio)
            .Item("satisfaction_ratio").Value(dynamicAttributes.SatisfactionRatio)
            .Item("dominant_resource").Value(attributes.DominantResource)
            .DoIf(shouldPrintResourceUsage, [&] (TFluentMap fluent) {
                fluent
                    .Item("resource_usage").Value(element->GetLocalResourceUsage());
            });
    }

    void BuildEssentialPoolElementYson(const TSchedulerElementPtr& element, TFluentMap fluent)
    {
        BuildEssentialElementYson(element, fluent, false);
    }

    void BuildEssentialOperationElementYson(const TSchedulerElementPtr& element, TFluentMap fluent)
    {
        BuildEssentialElementYson(element, fluent, true);
    }

    TYPath GetPoolPath(const TCompositeSchedulerElementPtr& element)
    {
        std::vector<TString> tokens;
        auto current = element;
        while (!current->IsRoot()) {
            if (current->IsExplicit()) {
                tokens.push_back(current->GetId());
            }
            current = current->GetParent();
        }

        std::reverse(tokens.begin(), tokens.end());

        TYPath path = "/" + NYPath::ToYPathLiteral(TreeId);
        for (const auto& token : tokens) {
            path.append('/');
            path.append(NYPath::ToYPathLiteral(token));
        }
        return path;
    }

    TCompositeSchedulerElementPtr GetDefaultParent()
    {
        auto defaultPool = FindPool(Config->DefaultParentPool);
        if (defaultPool) {
            return defaultPool;
        } else {
            return RootElement;
        }
    }

    TCompositeSchedulerElementPtr GetPoolOrParent(const TPoolName& poolName)
    {
        TCompositeSchedulerElementPtr pool = FindPool(poolName.GetPool());
        if (pool) {
            return pool;
        }
        if (!poolName.GetParentPool()) {
            return GetDefaultParent();
        }
        pool = FindPool(poolName.GetParentPool().Get());
        if (!pool) {
            THROW_ERROR_EXCEPTION("Parent pool %Qv does not exist", poolName.GetParentPool());
        }
        return pool;
    }

    void ValidateOperationCountLimit(const IOperationStrategyHost* operation, const TPoolName& poolName)
    {
        auto poolWithViolatedLimit = FindPoolWithViolatedOperationCountLimit(GetPoolOrParent(poolName));
        if (poolWithViolatedLimit) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::TooManyOperations,
                "Limit for the number of concurrent operations %v for pool %Qv in tree %Qv has been reached",
                poolWithViolatedLimit->GetMaxOperationCount(),
                poolWithViolatedLimit->GetId(),
                TreeId);
        }
    }

    void ValidateEphemeralPoolLimit(const IOperationStrategyHost* operation, const TPoolName& poolName)
    {
        auto pool = FindPool(poolName.GetPool());
        if (pool) {
            return;
        }

        const auto& userName = operation->GetAuthenticatedUser();

        auto it = UserToEphemeralPools.find(userName);
        if (it == UserToEphemeralPools.end()) {
            return;
        }

        if (it->second.size() + 1 > Config->MaxEphemeralPoolsPerUser) {
            THROW_ERROR_EXCEPTION("Limit for number of ephemeral pools %v for user %v in tree %Qv has been reached",
                Config->MaxEphemeralPoolsPerUser,
                userName,
                TreeId);
        }
    }

    void DoValidateOperationPoolsCanBeUsed(const IOperationStrategyHost* operation, const TPoolName& poolPair)
    {
        TCompositeSchedulerElementPtr pool = FindPool(poolPair.GetPool());
        // NB: Check is not performed if operation is started in default or unknown pool.
        if (pool && pool->AreImmediateOperationsForbidden()) {
            THROW_ERROR_EXCEPTION("Starting operations immediately in pool %Qv is forbidden", poolPair.GetPool());
        }

        if (!pool) {
            pool = GetPoolOrParent(poolPair);
        }

        Host->ValidatePoolPermission(GetPoolPath(pool), operation->GetAuthenticatedUser(), EPermission::Use);
    }

    void ProfileOperationElement(TOperationElementPtr element) const
    {
        auto poolTag = element->GetParent()->GetProfilingTag();
        auto slotIndexTag = GetSlotIndexProfilingTag(element->GetSlotIndex());

        ProfileSchedulerElement(element, "/operations", {poolTag, slotIndexTag, TreeIdProfilingTag});
    }

    void ProfileCompositeSchedulerElement(TCompositeSchedulerElementPtr element) const
    {
        auto tag = element->GetProfilingTag();
        ProfileSchedulerElement(element, "/pools", {tag, TreeIdProfilingTag});

        Profiler.Enqueue(
            "/running_operation_count",
            element->RunningOperationCount(),
            EMetricType::Gauge,
            {tag, TreeIdProfilingTag});
        Profiler.Enqueue(
            "/total_operation_count",
            element->OperationCount(),
            EMetricType::Gauge,
            {tag, TreeIdProfilingTag});
    }

    void ProfileSchedulerElement(TSchedulerElementPtr element, const TString& profilingPrefix, const TTagIdList& tags) const
    {
        Profiler.Enqueue(
            profilingPrefix + "/fair_share_ratio_x100000",
            static_cast<i64>(element->Attributes().FairShareRatio * 1e5),
            EMetricType::Gauge,
            tags);
        Profiler.Enqueue(
            profilingPrefix + "/usage_ratio_x100000",
            static_cast<i64>(element->GetLocalResourceUsageRatio() * 1e5),
            EMetricType::Gauge,
            tags);
        Profiler.Enqueue(
            profilingPrefix + "/demand_ratio_x100000",
            static_cast<i64>(element->Attributes().DemandRatio * 1e5),
            EMetricType::Gauge,
            tags);
        Profiler.Enqueue(
            profilingPrefix + "/guaranteed_resource_ratio_x100000",
            static_cast<i64>(element->Attributes().GuaranteedResourcesRatio * 1e5),
            EMetricType::Gauge,
            tags);

        ProfileResources(
            Profiler,
            element->GetLocalResourceUsage(),
            profilingPrefix + "/resource_usage",
            tags);
        ProfileResources(
            Profiler,
            element->ResourceLimits(),
            profilingPrefix + "/resource_limits",
            tags);
        ProfileResources(
            Profiler,
            element->ResourceDemand(),
            profilingPrefix + "/resource_demand",
            tags);

        element->GetJobMetrics().SendToProfiler(
            Profiler,
            profilingPrefix + "/metrics",
            tags);
    }
};

DEFINE_REFCOUNTED_TYPE(TFairShareTree)

////////////////////////////////////////////////////////////////////////////////

class TFairShareStrategy
    : public ISchedulerStrategy
{
public:
    TFairShareStrategy(
        TFairShareStrategyConfigPtr config,
        ISchedulerStrategyHost* host,
        const std::vector<IInvokerPtr>& feasibleInvokers)
        : Config(config)
        , Host(host)
        , FeasibleInvokers(feasibleInvokers)
        , Logger(SchedulerLogger)
        , LastProfilingTime_(TInstant::Zero())
    {
        FairShareUpdateExecutor_ = New<TPeriodicExecutor>(
            GetCurrentInvoker(),
            BIND(&TFairShareStrategy::OnFairShareUpdate, MakeWeak(this)),
            Config->FairShareUpdatePeriod);

        FairShareLoggingExecutor_ = New<TPeriodicExecutor>(
            GetCurrentInvoker(),
            BIND(&TFairShareStrategy::OnFairShareLogging, MakeWeak(this)),
            Config->FairShareLogPeriod);

        MinNeededJobResourcesUpdateExecutor_ = New<TPeriodicExecutor>(
            GetCurrentInvoker(),
            BIND(&TFairShareStrategy::OnMinNeededJobResourcesUpdate, MakeWeak(this)),
            Config->MinNeededResourcesUpdatePeriod);
    }

    virtual void OnMasterConnected() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        FairShareLoggingExecutor_->Start();
        FairShareUpdateExecutor_->Start();
        MinNeededJobResourcesUpdateExecutor_->Start();
    }

    virtual void OnMasterDisconnected() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        FairShareLoggingExecutor_->Stop();
        FairShareUpdateExecutor_->Stop();
        MinNeededJobResourcesUpdateExecutor_->Stop();

        {
            TWriterGuard guard(RegisteredOperationsLock_);
            RegisteredOperations_.clear();
        }

        OperationIdToOperationState_.clear();
        IdToTree_.clear();

        DefaultTreeId_.Reset();

        {
            TWriterGuard guard(TreeIdToSnapshotLock_);
            TreeIdToSnapshot_.clear();
        }
    }

    void OnFairShareUpdate()
    {
        OnFairShareUpdateAt(TInstant::Now());
    }

    void OnMinNeededJobResourcesUpdate() override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        LOG_INFO("Starting min needed job resources update");

        for (const auto& pair : OperationIdToOperationState_) {
            const auto& state = pair.second;
            if (state->GetHost()->IsSchedulable()) {
                state->GetController()->UpdateMinNeededJobResources();
            }
        }

        LOG_INFO("Min needed job resources successfully updated");
    }

    void OnFairShareLogging()
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        OnFairShareLoggingAt(TInstant::Now());
    }

    virtual TFuture<void> ScheduleJobs(const ISchedulingContextPtr& schedulingContext) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto snapshot = FindTreeSnapshotByNodeDescriptor(schedulingContext->GetNodeDescriptor());

        // Can happen if all trees are removed.
        if (!snapshot) {
            LOG_INFO("Node does not belong to any fair-share tree, scheduling skipped (Address: %v)",
                schedulingContext->GetNodeDescriptor().Address);
            return VoidFuture;
        }

        return snapshot->ScheduleJobs(schedulingContext);
    }

    virtual void RegisterOperation(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto spec = ParseSpec(operation);
        auto state = New<TFairShareStrategyOperationState>(operation);
        state->TreeIdToPoolIdMap() = GetOperationPools(operation->GetRuntimeParameters());

        YCHECK(OperationIdToOperationState_.insert(
            std::make_pair(operation->GetId(), state)).second);

        {
            TWriterGuard guard(RegisteredOperationsLock_);
            YCHECK(RegisteredOperations_.insert(operation->GetId()).second);
        }

        auto runtimeParams = operation->GetRuntimeParameters();

        for (const auto& pair : state->TreeIdToPoolIdMap()) {
            const auto& treeId = pair.first;
            const auto& tree = GetTree(pair.first);

            auto paramsIt = runtimeParams->SchedulingOptionsPerPoolTree.find(treeId);
            YCHECK(paramsIt != runtimeParams->SchedulingOptionsPerPoolTree.end());

            if (tree->RegisterOperation(state, spec, paramsIt->second)) {
                ActivateOperations({operation->GetId()});
            }
        }
    }

    virtual void UnregisterOperation(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operation->GetId());
        for (const auto& pair : state->TreeIdToPoolIdMap()) {
            const auto& treeId = pair.first;
            DoUnregisterOperationFromTree(state, treeId);
        }

        {
            TWriterGuard guard(RegisteredOperationsLock_);
            YCHECK(RegisteredOperations_.erase(operation->GetId()) == 1);
        }

        YCHECK(OperationIdToOperationState_.erase(operation->GetId()) == 1);
    }

    virtual void UnregisterOperationFromTree(const TOperationId& operationId, const TString& treeId) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operationId);
        if (!state->TreeIdToPoolIdMap().has(treeId)) {
            LOG_INFO("Operation to be removed from a tentative tree was not found in that tree (OperationId: %v, TreeId: %v)",
                operationId,
                treeId);
            return;
        }

        DoUnregisterOperationFromTree(state, treeId);

        state->EraseTree(treeId);

        LOG_INFO("Operation removed from a tentative tree (OperationId: %v, TreeId: %v)", operationId, treeId);
    }

    void DoUnregisterOperationFromTree(const TFairShareStrategyOperationStatePtr& operationState, const TString& treeId)
    {
        auto unregistrationResult = GetTree(treeId)->UnregisterOperation(operationState);
        ActivateOperations(unregistrationResult.OperationsToActivate);
    }

    virtual void DisableOperation(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operation->GetId());
        for (const auto& pair : state->TreeIdToPoolIdMap()) {
            const auto& treeId = pair.first;
            GetTree(treeId)->DisableOperation(state);
        }
    }

    virtual void UpdatePoolTrees(const INodePtr& poolTreesNode) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        LOG_INFO("Updating pool trees");

        if (poolTreesNode->GetType() != NYTree::ENodeType::Map) {
            auto error = TError("Pool trees node has invalid type")
                << TErrorAttribute("expected_type", NYTree::ENodeType::Map)
                << TErrorAttribute("actual_type", poolTreesNode->GetType());
            LOG_WARNING(error);
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, error);
            return;
        }

        auto poolsMap = poolTreesNode->AsMap();

        std::vector<TError> errors;

        // Collect trees to add and remove.
        THashSet<TString> treeIdsToAdd;
        THashSet<TString> treeIdsToRemove;
        CollectTreesToAddAndRemove(poolsMap, &treeIdsToAdd, &treeIdsToRemove);

        // Populate trees map. New trees are not added to global map yet.
        auto idToTree = ConstructUpdatedTreeMap(
            poolsMap,
            treeIdsToAdd,
            treeIdsToRemove,
            &errors);

        // Check default tree pointer. It should point to some valid tree,
        // otherwise pool trees are not updated.
        auto defaultTreeId = poolsMap->Attributes().Find<TString>(DefaultTreeAttributeName);

        if (defaultTreeId && idToTree.find(*defaultTreeId) == idToTree.end()) {
            errors.emplace_back("Default tree is missing");
            auto error = TError("Error updating pool trees")
                << std::move(errors);
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, error);
            return;
        }

        // Check that after adding or removing trees each node will belong exactly to one tree.
        // Check is skipped if trees configuration did not change.
        bool skipTreesConfigurationCheck = treeIdsToAdd.empty() && treeIdsToRemove.empty();

        if (!skipTreesConfigurationCheck)
        {
            if (!CheckTreesConfiguration(idToTree, &errors)) {
                auto error = TError("Error updating pool trees")
                    << std::move(errors);
                Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, error);
                return;
            }
        }

        // Update configs and pools structure of all trees.
        int updatedTreeCount;
        UpdateTreesConfigs(poolsMap, idToTree, &errors, &updatedTreeCount);

        // Abort orphaned operations.
        AbortOrphanedOperations(treeIdsToRemove);

        // Updating default fair-share tree and global tree map.
        DefaultTreeId_ = defaultTreeId;
        std::swap(IdToTree_, idToTree);

        THashMap<TString, IFairShareTreeSnapshotPtr> snapshots;
        for (const auto& pair : IdToTree_) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;
            YCHECK(snapshots.insert(std::make_pair(treeId, tree->CreateSnapshot())).second);
        }

        {
            TWriterGuard guard(TreeIdToSnapshotLock_);
            std::swap(TreeIdToSnapshot_, snapshots);
        }

        // Setting alerts.
        if (!errors.empty()) {
            auto error = TError("Error updating pool trees")
                << std::move(errors);
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, error);
        } else {
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdatePools, TError());
            if (updatedTreeCount > 0 || treeIdsToRemove.size() > 0 || treeIdsToAdd.size() > 0) {
                Host->LogEventFluently(ELogEventType::PoolsInfo)
                    .Item("pools").DoMapFor(IdToTree_, [&] (TFluentMap fluent, const auto& value) {
                        const auto& treeId = value.first;
                        const auto& tree = value.second;
                        fluent
                            .Item(treeId).Do(BIND(&TFairShareTree::BuildStaticPoolsInformation, tree));
                    });
            }
            LOG_INFO("Pool trees updated");
        }
    }

    virtual void BuildOperationAttributes(const TOperationId& operationId, TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operationId);
        const auto& pools = state->TreeIdToPoolIdMap();

        if (DefaultTreeId_ && pools.find(*DefaultTreeId_) != pools.end()) {
            GetTree(*DefaultTreeId_)->BuildOperationAttributes(operationId, fluent);
        }
    }

    virtual void BuildOperationProgress(const TOperationId& operationId, TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        if (!FindOperationState(operationId)) {
            return;
        }

        DoBuildOperationProgress(&TFairShareTree::BuildOperationProgress, operationId, fluent);
    }

    virtual void BuildBriefOperationProgress(const TOperationId& operationId, TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        if (!FindOperationState(operationId)) {
            return;
        }

        DoBuildOperationProgress(&TFairShareTree::BuildBriefOperationProgress, operationId, fluent);
    }

    virtual TPoolTreeToSchedulingTagFilter GetOperationPoolTreeToSchedulingTagFilter(const TOperationId& operationId) override
    {
        TPoolTreeToSchedulingTagFilter result;
        for (const auto& pair : GetOperationState(operationId)->TreeIdToPoolIdMap()) {
            const auto& treeName = pair.first;
            result.insert(std::make_pair(treeName, GetTree(treeName)->GetNodesFilter()));
        }
        return result;
    }

    virtual std::vector<std::pair<TOperationId, TError>> GetUnschedulableOperations() override
    {
        std::vector<std::pair<TOperationId, TError>> result;
        for (const auto& operationStatePair : OperationIdToOperationState_) {
            const auto& operationId = operationStatePair.first;
            const auto& operationState = operationStatePair.second;

            bool hasSchedulableTree = false;
            TError operationError("Operation is unschedulable in all trees");

            YCHECK(operationState->TreeIdToPoolIdMap().size() > 0);

            for (const auto& treePoolPair : operationState->TreeIdToPoolIdMap()) {
                const auto& treeName = treePoolPair.first;
                auto error = GetTree(treeName)->CheckOperationUnschedulable(
                    operationId,
                    Config->OperationUnschedulableSafeTimeout,
                    Config->OperationUnschedulableMinScheduleJobAttempts);
                if (error.IsOK()) {
                    hasSchedulableTree = true;
                    break;
                } else {
                    operationError.InnerErrors().push_back(error);
                }
            }

            if (!hasSchedulableTree) {
                result.emplace_back(operationId, operationError);
            }
        }
        return result;
    }

    virtual void UpdateConfig(const TFairShareStrategyConfigPtr& config) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        Config = config;

        for (const auto& pair : IdToTree_) {
            const auto& tree = pair.second;
            tree->UpdateControllerConfig(config);
        }

        FairShareUpdateExecutor_->SetPeriod(Config->FairShareUpdatePeriod);
        FairShareLoggingExecutor_->SetPeriod(Config->FairShareLogPeriod);
        MinNeededJobResourcesUpdateExecutor_->SetPeriod(Config->MinNeededResourcesUpdatePeriod);
    }

    virtual void BuildOperationInfoForEventLog(const IOperationStrategyHost* operation, TFluentMap fluent)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& operationState = GetOperationState(operation->GetId());
        const auto& pools = operationState->TreeIdToPoolIdMap();

        fluent
            .DoIf(DefaultTreeId_.HasValue(), [&] (TFluentMap fluent) {
                auto it = pools.find(*DefaultTreeId_);
                if (it != pools.end()) {
                    fluent
                        .Item("pool").Value(it->second.GetPool());
                }
            });
    }

    virtual void ApplyOperationRuntimeParameters(IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto state = GetOperationState(operation->GetId());
        const auto runtimeParams = operation->GetRuntimeParameters();

        auto newPools = GetOperationPools(operation->GetRuntimeParameters());

        YCHECK(newPools.size() == state->TreeIdToPoolIdMap().size());

        //tentative trees can be removed from state, we must apply these changes to new state
        for (const auto& erasedTree : state->ErasedTrees()) {
            newPools.erase(erasedTree);
        }

        for (const auto& pair : state->TreeIdToPoolIdMap()) {
            const auto& treeId = pair.first;
            const auto& oldPool = pair.second;

            auto newPoolIt = newPools.find(treeId);
            YCHECK(newPoolIt != newPools.end());

            if (oldPool.GetPool() != newPoolIt->second.GetPool()) {
                bool wasActive = GetTree(treeId)->ChangeOperationPool(operation->GetId(), state, newPoolIt->second);
                if (!wasActive) {
                    ActivateOperations({operation->GetId()});
                }
            }

            auto it = runtimeParams->SchedulingOptionsPerPoolTree.find(treeId);
            YCHECK(it != runtimeParams->SchedulingOptionsPerPoolTree.end());
            GetTree(treeId)->UpdateOperationRuntimeParameters(operation->GetId(), it->second);
        }
        state->TreeIdToPoolIdMap() = newPools;
    }

    virtual void InitOperationRuntimeParameters(
        const TOperationRuntimeParametersPtr& runtimeParameters,
        const TOperationSpecBasePtr& spec,
        const TString& user,
        EOperationType operationType) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto poolTrees = ParsePoolTrees(spec, operationType);
        runtimeParameters->Owners = spec->Owners;
        for (const auto& tree : poolTrees) {
            auto treeParams = New<TOperationFairShareTreeRuntimeParameters>();
            auto specIt = spec->SchedulingOptionsPerPoolTree.find(tree);
            if (specIt != spec->SchedulingOptionsPerPoolTree.end()) {
                treeParams->Weight = spec->Weight ? spec->Weight : specIt->second->Weight;
                treeParams->Pool = GetTree(tree)->MakeAppropriatePoolName(spec->Pool ? spec->Pool : specIt->second->Pool, user);
                treeParams->ResourceLimits = spec->ResourceLimits ? spec->ResourceLimits : specIt->second->ResourceLimits;
            } else {
                treeParams->Weight = spec->Weight;
                treeParams->Pool = GetTree(tree)->MakeAppropriatePoolName(spec->Pool, user);
                treeParams->ResourceLimits = spec->ResourceLimits;
            }
            YCHECK(runtimeParameters->SchedulingOptionsPerPoolTree.emplace(tree, std::move(treeParams)).second);
        }
    }

    virtual void ValidateOperationRuntimeParameters(
        IOperationStrategyHost* operation,
        const TOperationRuntimeParametersPtr& runtimeParams) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operation->GetId());

        for (const auto& pair : runtimeParams->SchedulingOptionsPerPoolTree) {
            auto poolTrees = state->TreeIdToPoolIdMap();
            if (poolTrees.find(pair.first) == poolTrees.end()) {
                THROW_ERROR_EXCEPTION("Pool tree %Qv was not configured for this operation", pair.first);
            }
        }

        ValidateOperationPoolsCanBeUsed(operation, runtimeParams);
        ValidatePoolLimits(operation, runtimeParams);
        ValidateMaxRunningOperationsCountOnPoolChange(operation, runtimeParams);
    }

    //TODO(renadeen): Remove when YT-8931 is done
    virtual void UpdateOperationRuntimeParametersOld(IOperationStrategyHost* operation, const IMapNodePtr& parametersNode) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        const auto& state = GetOperationState(operation->GetId());
        const auto& pools = state->TreeIdToPoolIdMap();

        if (DefaultTreeId_ && pools.find(*DefaultTreeId_) != pools.end()) {
            auto params = operation->GetRuntimeParameters();
            auto defaultTreeOptionsIt = params->SchedulingOptionsPerPoolTree.find(*DefaultTreeId_);
            YCHECK(defaultTreeOptionsIt != params->SchedulingOptionsPerPoolTree.end());

            auto& treeParams = defaultTreeOptionsIt->second;
            auto weightNode = parametersNode->FindChild("weight");
            if (weightNode) {
                Deserialize(treeParams->Weight, weightNode);
            }
            auto resourceLimits = parametersNode->FindChild("resource_limits");
            if (resourceLimits && resourceLimits->AsMap()->GetKeys().size() > 0) {
                treeParams->ResourceLimits = ConvertTo<TResourceLimitsConfigPtr>(resourceLimits);
            }

            GetTree(*DefaultTreeId_)->UpdateOperationRuntimeParameters(
                operation->GetId(),
                treeParams);
        }
    }

    virtual void BuildOrchid(TFluentMap fluent) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        // TODO(ignat): stop using pools from here and remove this section (since it is also presented in fair_share_info subsection).
        if (DefaultTreeId_) {
            GetTree(*DefaultTreeId_)->BuildPoolsInformation(fluent);
        }


        THashMap<TString, std::vector<TExecNodeDescriptor>> descriptorsPerPoolTree;
        for (const auto& pair : IdToTree_) {
            const auto& treeId = pair.first;
            descriptorsPerPoolTree.emplace(treeId, std::vector<TExecNodeDescriptor>{});
        }

        auto descriptors = Host->CalculateExecNodeDescriptors(TSchedulingTagFilter());
        for (const auto& idDescriptorPair : *descriptors) {
            const auto& descriptor = idDescriptorPair.second;
            for (const auto& idTreePair : IdToTree_) {
                const auto& treeId = idTreePair.first;
                const auto& tree = idTreePair.second;
                if (tree->GetNodesFilter().CanSchedule(descriptor.Tags)) {
                    descriptorsPerPoolTree[treeId].push_back(descriptor);
                    break;
                }
            }
        }

        fluent
            .DoIf(DefaultTreeId_.HasValue(), [&] (TFluentMap fluent) {
                fluent
                    // COMPAT(asaitgalin): Remove it when UI will use scheduling_info_per_pool_tree
                    .Item("fair_share_info").BeginMap()
                        .Do(BIND(&TFairShareTree::BuildFairShareInfo, GetTree(*DefaultTreeId_)))
                    .EndMap()
                    .Item("default_fair_share_tree").Value(*DefaultTreeId_);
            })
            .Item("scheduling_info_per_pool_tree").DoMapFor(IdToTree_, [&] (TFluentMap fluent, const auto& pair) {
                    const auto& treeId = pair.first;
                    const auto& tree = pair.second;

                    auto it = descriptorsPerPoolTree.find(treeId);
                    YCHECK(it != descriptorsPerPoolTree.end());

                    fluent
                        .Item(treeId).BeginMap()
                            .Do(BIND(&TFairShareStrategy::BuildTreeOrchid, tree, it->second))
                        .EndMap();
            });
    }

    virtual void ApplyJobMetricsDelta(const TOperationIdToOperationJobMetrics& operationIdToOperationJobMetrics) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TForbidContextSwitchGuard contextSwitchGuard;

        THashMap<TString, IFairShareTreeSnapshotPtr> snapshots;
        {
            TReaderGuard guard(TreeIdToSnapshotLock_);
            snapshots = TreeIdToSnapshot_;
        }

        for (const auto& pair : operationIdToOperationJobMetrics) {
            const auto& operationId = pair.first;
            for (const auto& metrics : pair.second) {
                auto snapshotIt = snapshots.find(metrics.TreeId);
                if (snapshotIt == snapshots.end()) {
                    continue;
                }

                const auto& snapshot = snapshotIt->second;
                snapshot->ApplyJobMetricsDelta(operationId, metrics.Metrics);
            }
        }
    }

    virtual TFuture<void> ValidateOperationStart(const IOperationStrategyHost* operation) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        return BIND(&TFairShareStrategy::ValidateOperationPoolsCanBeUsed, Unretained(this))
            .AsyncVia(GetCurrentInvoker())
            .Run(operation, operation->GetRuntimeParameters());
    }

    virtual void ValidatePoolLimits(
        const IOperationStrategyHost* operation,
        const TOperationRuntimeParametersPtr& runtimeParameters) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto pools = GetOperationPools(runtimeParameters);

        for (const auto& pair : pools) {
            auto tree = GetTree(pair.first);
            tree->ValidatePoolLimits(operation, pair.second);
        }
    }

    virtual void ValidateMaxRunningOperationsCountOnPoolChange(
        const IOperationStrategyHost* operation,
        const TOperationRuntimeParametersPtr& runtimeParameters)
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        auto pools = GetOperationPools(runtimeParameters);

        for (const auto& pair : pools) {
            auto tree = GetTree(pair.first);
            tree->ValidatePoolLimitsOnPoolChange(operation, pair.second);
        }
    }

    // NB: This function is public for testing purposes.
    virtual void OnFairShareUpdateAt(TInstant now) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        LOG_INFO("Starting fair share update");

        std::vector<TError> errors;

        for (const auto& pair : IdToTree_) {
            const auto& tree = pair.second;
            auto error = tree->OnFairShareUpdateAt(now);
            if (!error.IsOK()) {
                errors.push_back(error);
            }
        }

        THashMap<TString, IFairShareTreeSnapshotPtr> snapshots;

        for (const auto& pair : IdToTree_) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;
            YCHECK(snapshots.insert(std::make_pair(treeId, tree->CreateSnapshot())).second);
        }

        {
            TWriterGuard guard(TreeIdToSnapshotLock_);
            std::swap(TreeIdToSnapshot_, snapshots);
        }

        if (LastProfilingTime_ + Config->FairShareProfilingPeriod < now) {
            LastProfilingTime_ = now;
            for (const auto& pair : IdToTree_) {
                const auto& tree = pair.second;
                tree->ProfileFairShare();
            }
        }

        if (!errors.empty()) {
            auto error = TError("Found pool configuration issues during fair share update")
                << std::move(errors);
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdateFairShare, error);
        } else {
            Host->SetSchedulerAlert(ESchedulerAlertType::UpdateFairShare, TError());
        }

        LOG_INFO("Fair share successfully updated");
    }

    virtual void OnFairShareEssentialLoggingAt(TInstant now) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        for (const auto& pair : IdToTree_) {
            const auto& tree = pair.second;
            tree->OnFairShareEssentialLoggingAt(now);
        }
    }

    virtual void OnFairShareLoggingAt(TInstant now) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        for (const auto& pair : IdToTree_) {
            const auto& tree = pair.second;
            tree->OnFairShareLoggingAt(now);
        }
    }

    virtual void ProcessJobUpdates(
        const std::vector<TJobUpdate>& jobUpdates,
        std::vector<std::pair<TOperationId, TJobId>>* successfullyUpdatedJobs,
        std::vector<TJobId>* jobsToAbort) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        LOG_DEBUG("Processing job updates to strategy");

        YCHECK(successfullyUpdatedJobs->empty());
        YCHECK(jobsToAbort->empty());

        THashMap<TString, IFairShareTreeSnapshotPtr> snapshots;
        {
            TReaderGuard guard(TreeIdToSnapshotLock_);
            snapshots = TreeIdToSnapshot_;
        }

        THashSet<TJobId> jobsToSave;

        for (const auto& job : jobUpdates) {
            if (job.Status == EJobUpdateStatus::Running) {
                auto snapshotIt = snapshots.find(job.TreeId);
                if (snapshotIt == snapshots.end()) {
                    // Job is orphaned (does not belong to any tree), aborting it.
                    jobsToAbort->push_back(job.JobId);
                } else {
                    // XXX(ignat): check snapshot->HasOperation(job.OperationId) ?
                    const auto& snapshot = snapshotIt->second;
                    snapshot->ProcessUpdatedJob(job.OperationId, job.JobId, job.Delta);
                }
            } else { // EJobUpdateStatus::Finished
                auto snapshotIt = snapshots.find(job.TreeId);
                if (snapshotIt == snapshots.end()) {
                    // Job is finished but tree does not exist, nothing to do.
                    continue;
                }
                const auto& snapshot = snapshotIt->second;
                if (snapshot->HasOperation(job.OperationId)) {
                    snapshot->ProcessFinishedJob(job.OperationId, job.JobId);
                } else {
                    // If operation is not yet in snapshot let's push it back to finished jobs.
                    TReaderGuard guard(RegisteredOperationsLock_);
                    if (RegisteredOperations_.find(job.OperationId) != RegisteredOperations_.end()) {
                        jobsToSave.insert(job.JobId);
                    }
                }
            }
        }

        for (const auto& job : jobUpdates) {
            if (!jobsToSave.has(job.JobId)) {
                successfullyUpdatedJobs->push_back({job.OperationId, job.JobId});
            }
        }
    }

    virtual void RegisterJobs(const TOperationId& operationId, const std::vector<TJobPtr>& jobs) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        THashMap<TString, std::vector<TJobPtr>> jobsByTreeId;

        for (const auto& job : jobs) {
            jobsByTreeId[job->GetTreeId()].push_back(job);
        }

        for (const auto& pair : jobsByTreeId) {
            auto tree = FindTree(pair.first);
            if (tree) {
                tree->RegisterJobs(operationId, pair.second);
            }
        }
    }

    virtual void EnableOperation(IOperationStrategyHost* host) override
    {
        const auto& operationId = host->GetId();
        const auto& state = GetOperationState(operationId);
        for (const auto& pair : state->TreeIdToPoolIdMap()) {
            const auto& treeId = pair.first;
            GetTree(treeId)->EnableOperation(state);
        }
        if (host->IsSchedulable()) {
            state->GetController()->UpdateMinNeededJobResources();
        }
    }

    virtual void ValidateNodeTags(const THashSet<TString>& tags) override
    {
        VERIFY_INVOKERS_AFFINITY(FeasibleInvokers);

        // Trees this node falls into.
        std::vector<TString> trees;

        for (const auto& pair : IdToTree_) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;
            if (tree->GetNodesFilter().CanSchedule(tags)) {
                trees.push_back(treeId);
            }
        }

        if (trees.size() > 1) {
            THROW_ERROR_EXCEPTION("Node belongs to more than one fair-share tree")
                << TErrorAttribute("matched_trees", trees);
        }
    }

private:
    TFairShareStrategyConfigPtr Config;
    ISchedulerStrategyHost* const Host;

    const std::vector<IInvokerPtr> FeasibleInvokers;

    mutable NLogging::TLogger Logger;

    TPeriodicExecutorPtr FairShareUpdateExecutor_;
    TPeriodicExecutorPtr FairShareLoggingExecutor_;
    TPeriodicExecutorPtr MinNeededJobResourcesUpdateExecutor_;

    THashMap<TOperationId, TFairShareStrategyOperationStatePtr> OperationIdToOperationState_;

    TReaderWriterSpinLock RegisteredOperationsLock_;
    THashSet<TOperationId> RegisteredOperations_;

    TInstant LastProfilingTime_;

    using TFairShareTreeMap = THashMap<TString, TFairShareTreePtr>;
    TFairShareTreeMap IdToTree_;

    TNullable<TString> DefaultTreeId_;

    TReaderWriterSpinLock TreeIdToSnapshotLock_;
    THashMap<TString, IFairShareTreeSnapshotPtr> TreeIdToSnapshot_;
    std::array<EOperationType, 3> OperationTypesWithShuffle = {
        EOperationType::Sort,
        EOperationType::MapReduce,
        EOperationType::RemoteCopy
    };

    TStrategyOperationSpecPtr ParseSpec(const IOperationStrategyHost* operation) const
    {
        try {
            return ConvertTo<TStrategyOperationSpecPtr>(operation->GetSpec());
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing strategy spec of operation")
                << ex;
        }
    }

    std::vector<TString> ParsePoolTrees(const TOperationSpecBasePtr& spec, EOperationType operationType) const
    {
        for (const auto& treeId : spec->PoolTrees) {
            if (!FindTree(treeId)) {
                THROW_ERROR_EXCEPTION("Pool tree %Qv not found", treeId);
            }
        }

        if (!spec->TentativePoolTrees.empty() && spec->PoolTrees.empty()) {
            THROW_ERROR_EXCEPTION("Regular pool trees must be specified for tentative pool trees to work properly");
        }

        for (const auto& tentativePoolTree : spec->TentativePoolTrees) {
            if (spec->PoolTrees.has(tentativePoolTree)) {
                THROW_ERROR_EXCEPTION("Regular and tentative pool trees must not intersect");
            }
        }

        std::vector<TString> result(spec->PoolTrees.begin(), spec->PoolTrees.end());
        if (result.empty()) {
            if (!DefaultTreeId_) {
                THROW_ERROR_EXCEPTION("Failed to determine fair-share tree for operation since "
                    "valid pool trees are not specified and default fair-share tree is not configured");
            }
            result.push_back(*DefaultTreeId_);
        }

        // Data shuffling shouldn't be launched in tentative trees.
        if (FindIndex(OperationTypesWithShuffle, operationType) == NPOS) {
            std::vector<TString> presentedTentativePoolTrees;
            for (const auto& treeId : spec->TentativePoolTrees) {
                if (FindTree(treeId)) {
                    presentedTentativePoolTrees.push_back(treeId);
                } else {
                    if (!spec->TentativeTreeEligibility->IgnoreMissingPoolTrees) {
                        THROW_ERROR_EXCEPTION("Pool tree %Qv not found", treeId);
                    }
                }
            }
            result.insert(result.end(), presentedTentativePoolTrees.begin(), presentedTentativePoolTrees.end());
        }

        return result;
    }

    THashMap<TString, TPoolName> GetOperationPools(const TOperationRuntimeParametersPtr& runtimeParams) const
    {
        THashMap<TString, TPoolName> pools;
        for (const auto& pair : runtimeParams->SchedulingOptionsPerPoolTree) {
            pools.emplace(pair.first, pair.second->Pool.Get());
        }
        return pools;
    }

    void ValidateOperationPoolsCanBeUsed(const IOperationStrategyHost* operation, const TOperationRuntimeParametersPtr& runtimeParameters)
    {
        if (IdToTree_.empty()) {
            THROW_ERROR_EXCEPTION("Scheduler strategy does not have configured fair-share trees");
        }

        auto spec = ParseSpec(operation);
        auto pools = GetOperationPools(runtimeParameters);

        if (pools.size() > 1 && !spec->SchedulingTagFilter.IsEmpty()) {
            THROW_ERROR_EXCEPTION(
                "Scheduling tag filter cannot be specified for operations "
                "to be scheduled in multiple fair-share trees");
        }

        std::vector<TFuture<void>> futures;

        for (const auto& pair : pools) {
            auto tree = GetTree(pair.first);
            futures.push_back(tree->ValidateOperationPoolsCanBeUsed(operation, pair.second));
        }

        WaitFor(Combine(futures))
            .ThrowOnError();
    }

    TFairShareStrategyOperationStatePtr FindOperationState(const TOperationId& operationId) const
    {
        auto it = OperationIdToOperationState_.find(operationId);
        if (it == OperationIdToOperationState_.end()) {
            return nullptr;
        }
        return it->second;
    }

    TFairShareStrategyOperationStatePtr GetOperationState(const TOperationId& operationId) const
    {
        auto it = OperationIdToOperationState_.find(operationId);
        YCHECK(it != OperationIdToOperationState_.end());
        return it->second;
    }

    TFairShareTreePtr FindTree(const TString& id) const
    {
        auto treeIt = IdToTree_.find(id);
        return treeIt != IdToTree_.end() ? treeIt->second : nullptr;
    }

    TFairShareTreePtr GetTree(const TString& id) const
    {
        auto tree = FindTree(id);
        YCHECK(tree);
        return tree;
    }

    IFairShareTreeSnapshotPtr FindTreeSnapshotByNodeDescriptor(const TExecNodeDescriptor& descriptor) const
    {
        IFairShareTreeSnapshotPtr result;

        TReaderGuard guard(TreeIdToSnapshotLock_);

        for (const auto& pair : TreeIdToSnapshot_) {
            const auto& snapshot = pair.second;
            if (snapshot->GetNodesFilter().CanSchedule(descriptor.Tags)) {
                YCHECK(!result);  // Only one snapshot should be found
                result = snapshot;
            }
        }

        return result;
    }

    void DoBuildOperationProgress(
        void (TFairShareTree::*method)(const TOperationId& operationId, TFluentMap fluent),
        const TOperationId& operationId,
        TFluentMap fluent)
    {
        const auto& state = GetOperationState(operationId);
        const auto& pools = state->TreeIdToPoolIdMap();

        fluent
            .Item("scheduling_info_per_pool_tree")
                .DoMapFor(pools, [&] (TFluentMap fluent, const std::pair<TString, TPoolName>& value) {
                    const auto& treeId = value.first;
                    fluent
                        .Item(treeId).BeginMap()
                            .Do(BIND(method, GetTree(treeId), operationId))
                        .EndMap();
                });
    }

    void ActivateOperations(const std::vector<TOperationId>& operationIds) const
    {
        for (const auto& operationId : operationIds) {
            const auto& state = GetOperationState(operationId);
            if (!state->GetActive()) {
                Host->ActivateOperation(operationId);
                state->SetActive(true);
            }
        }
    }

    void CollectTreesToAddAndRemove(
        const IMapNodePtr& poolsMap,
        THashSet<TString>* treesToAdd,
        THashSet<TString>* treesToRemove) const
    {
        for (const auto& key : poolsMap->GetKeys()) {
            if (IdToTree_.find(key) == IdToTree_.end()) {
                treesToAdd->insert(key);
            }
        }

        for (const auto& pair : IdToTree_) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;

            auto child = poolsMap->FindChild(treeId);
            if (!child) {
                treesToRemove->insert(treeId);
                continue;
            }

            // Nodes filter update is equivalent to remove-add operation.
            try {
                auto configMap = child->Attributes().ToMap();
                auto config = ConvertTo<TFairShareStrategyTreeConfigPtr>(configMap);

                if (config->NodesFilter != tree->GetNodesFilter()) {
                    treesToRemove->insert(treeId);
                    treesToAdd->insert(treeId);
                }
            } catch (const std::exception&) {
                // Do nothing, alert will be set later.
                continue;
            }
        }
    }

    TFairShareTreeMap ConstructUpdatedTreeMap(
        const IMapNodePtr& poolsMap,
        const THashSet<TString>& treesToAdd,
        const THashSet<TString>& treesToRemove,
        std::vector<TError>* errors) const
    {
        TFairShareTreeMap trees;

        for (const auto& treeId : treesToAdd) {
            TFairShareStrategyTreeConfigPtr treeConfig;
            try {
                auto configMap = poolsMap->GetChild(treeId)->Attributes().ToMap();
                treeConfig = ConvertTo<TFairShareStrategyTreeConfigPtr>(configMap);
            } catch (const std::exception& ex) {
                auto error = TError("Error parsing configuration of tree %Qv", treeId)
                    << ex;
                errors->push_back(error);
                LOG_WARNING(error);
                continue;
            }

            auto tree = New<TFairShareTree>(treeConfig, Config, Host, FeasibleInvokers, treeId);
            trees.emplace(treeId, tree);
        }

        for (const auto& pair : IdToTree_) {
            if (treesToRemove.find(pair.first) == treesToRemove.end()) {
                trees.insert(pair);
            }
        }

        return trees;
    }

    bool CheckTreesConfiguration(const TFairShareTreeMap& trees, std::vector<TError>* errors) const
    {
        THashMap<NNodeTrackerClient::TNodeId, THashSet<TString>> nodeIdToTreeSet;

        for (const auto& pair : trees) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;
            auto nodes = Host->GetExecNodeIds(tree->GetNodesFilter());

            for (const auto& node : nodes) {
                nodeIdToTreeSet[node].insert(treeId);
            }
        }

        for (const auto& pair : nodeIdToTreeSet) {
            const auto& nodeId = pair.first;
            const auto& trees  = pair.second;
            if (trees.size() > 1) {
                errors->emplace_back("Cannot update fair-share trees since there is node that "
                    "belongs to multiple trees (NodeId: %v, MatchedTrees: %v)",
                    nodeId,
                    trees);
                return false;
            }
        }

        return true;
    }

    void UpdateTreesConfigs(
        const IMapNodePtr& poolsMap,
        const TFairShareTreeMap& trees,
        std::vector<TError>* errors,
        int* updatedTreeCount) const
    {
        *updatedTreeCount = 0;

        for (const auto& pair : trees) {
            const auto& treeId = pair.first;
            const auto& tree = pair.second;

            auto child = poolsMap->GetChild(treeId);

            try {
                auto configMap = child->Attributes().ToMap();
                auto config = ConvertTo<TFairShareStrategyTreeConfigPtr>(configMap);
                tree->UpdateConfig(config);
            } catch (const std::exception& ex) {
                auto error = TError("Failed to configure tree %Qv, defaults will be used", treeId)
                    << ex;
                errors->push_back(error);
                continue;
            }

            auto updateResult = tree->UpdatePools(child);
            if (!updateResult.Error.IsOK()) {
                errors->push_back(updateResult.Error);
            }
            if (updateResult.Updated) {
                *updatedTreeCount = *updatedTreeCount + 1;
            }
        }
    }

    void AbortOrphanedOperations(const THashSet<TString>& treesToRemove)
    {
        if (treesToRemove.empty()) {
            return;
        }

        THashMap<TOperationId, THashSet<TString>> operationIdToTreeSet;
        THashMap<TString, THashSet<TOperationId>> treeIdToOperationSet;

        for (const auto& pair : OperationIdToOperationState_) {
            const auto& operationId = pair.first;
            const auto& poolsMap = pair.second->TreeIdToPoolIdMap();

            for (const auto& treeAndPool : poolsMap) {
                const auto& treeId = treeAndPool.first;

                YCHECK(operationIdToTreeSet[operationId].insert(treeId).second);
                YCHECK(treeIdToOperationSet[treeId].insert(operationId).second);
            }
        }

        for (const auto& treeId : treesToRemove) {
            auto it = treeIdToOperationSet.find(treeId);

            // No operations are running in this tree.
            if (it == treeIdToOperationSet.end()) {
                continue;
            }

            // Unregister operations in removed tree and update their tree set.
            for (const auto& operationId : it->second) {
                const auto& state = GetOperationState(operationId);
                GetTree(treeId)->UnregisterOperation(state);
                YCHECK(state->TreeIdToPoolIdMap().erase(treeId) == 1);

                auto treeSetIt = operationIdToTreeSet.find(operationId);
                YCHECK(treeSetIt != operationIdToTreeSet.end());
                YCHECK(treeSetIt->second.erase(treeId) == 1);
            }
        }

        // Aborting orphaned operations.
        for (const auto& pair : operationIdToTreeSet) {
            const auto& operationId = pair.first;
            const auto& treeSet = pair.second;
            if (treeSet.empty()) {
                Host->AbortOperation(
                    operationId,
                    TError("No suitable fair-share trees to schedule operation"));
            }
        }
    }

    static void BuildTreeOrchid(
        const TFairShareTreePtr& tree,
        const std::vector<TExecNodeDescriptor>& descriptors,
        TFluentMap fluent)
    {
        auto resourceLimits = ZeroJobResources();
        for (const auto& descriptor : descriptors) {
            resourceLimits += descriptor.ResourceLimits;
        }

        fluent
            .Item("user_to_ephemeral_pools").Do(BIND(&TFairShareTree::BuildUserToEphemeralPools, tree))
            .Item("fair_share_info").BeginMap()
                .Do(BIND(&TFairShareTree::BuildFairShareInfo, tree))
            .EndMap()
            .Do(BIND(&TFairShareTree::BuildOrchid, tree))
            .Item("resource_limits").Value(resourceLimits)
            .Item("node_count").Value(descriptors.size())
            .Item("node_addresses").BeginList()
                .DoFor(descriptors, [&] (TFluentList fluent, const auto& descriptor) {
                    fluent
                        .Item().Value(descriptor.Address);
                })
            .EndList();
    }
};

ISchedulerStrategyPtr CreateFairShareStrategy(
    TFairShareStrategyConfigPtr config,
    ISchedulerStrategyHost* host,
    const std::vector<IInvokerPtr>& feasibleInvokers)
{
    return New<TFairShareStrategy>(config, host, feasibleInvokers);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

