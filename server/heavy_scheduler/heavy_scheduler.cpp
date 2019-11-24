#include "heavy_scheduler.h"

#include "bootstrap.h"
#include "cluster_reader.h"
#include "config.h"
#include "label_filter_evaluator.h"
#include "private.h"
#include "resource_vector.h"
#include "yt_connector.h"

#include <yp/server/lib/cluster/allocator.h>
#include <yp/server/lib/cluster/cluster.h>
#include <yp/server/lib/cluster/config.h>
#include <yp/server/lib/cluster/node.h>
#include <yp/server/lib/cluster/node_segment.h>
#include <yp/server/lib/cluster/pod.h>
#include <yp/server/lib/cluster/pod_disruption_budget.h>
#include <yp/server/lib/cluster/pod_set.h>

#include <yp/server/lib/objects/object_filter.h>

#include <yp/client/api/native/helpers.h>

#include <yp/client/api/proto/data_model.pb.h>
#include <yp/client/api/proto/enums.pb.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <util/random/shuffle.h>

namespace NYP::NServer::NHeavyScheduler {

using namespace NCluster;

using namespace NClient::NApi::NNative;
using namespace NClient::NApi;

using namespace NConcurrency;
using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

TInstant ParseErrorDatetime(const TError& error)
{
    return TInstant::ParseIso8601(error.Attributes().Get<TString>("datetime"));
}

////////////////////////////////////////////////////////////////////////////////

struct TObjectCompositeId
{
    TObjectId Id;
    TObjectId Uuid;
};

void FormatValue(
    TStringBuilderBase* builder,
    const TObjectCompositeId& compositeId,
    TStringBuf /*format*/)
{
    builder->AppendFormat("{Id: %v, Uuid: %v}",
        compositeId.Id,
        compositeId.Uuid);
}

TObjectCompositeId GetCompositeId(const TPod* pod)
{
    return TObjectCompositeId{
        pod->GetId(),
        pod->Uuid()};
}

TPod* FindPod(const TClusterPtr& cluster, const TObjectCompositeId& compositeId)
{
    auto* pod = cluster->FindPod(compositeId.Id);
    if (!pod) {
        return nullptr;
    }
    if (pod->Uuid() != compositeId.Uuid) {
        return nullptr;
    }
    return pod;
}

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETaskState,
    (Active)
    (Finished)
);

class TSwapTask
    : public TRefCounted
{
public:
    TSwapTask(
        TGuid id,
        TInstant startTime,
        TObjectCompositeId starvingPodCompositeId,
        TObjectCompositeId victimPodCompositeId)
        : Logger(TLogger(NHeavyScheduler::Logger)
            .AddTag("TaskId: %v", id))
        , Id_(std::move(id))
        , StartTime_(startTime)
        , StarvingPodCompositeId_(std::move(starvingPodCompositeId))
        , VictimPodCompositeId_(std::move(victimPodCompositeId))
        , State_(ETaskState::Active)
    { }

    TGuid GetId() const
    {
        return Id_;
    }

    TInstant GetStartTime() const
    {
        return StartTime_;
    }

    ETaskState GetState() const
    {
        return State_;
    }

    void ReconcileState(const TClusterPtr& cluster)
    {
        YT_VERIFY(State_ == ETaskState::Active);

        auto* starvingPod = FindPod(cluster, StarvingPodCompositeId_);
        auto* victimPod = FindPod(cluster, VictimPodCompositeId_);

        if (!starvingPod) {
            YT_LOG_DEBUG("Swap task is considered finished; starving pod does not exist");
            State_ = ETaskState::Finished;
            return;
        }

        if (starvingPod->GetNode()) {
            YT_LOG_DEBUG("Swap task is considered finished; starving pod is scheduled");
            State_ = ETaskState::Finished;
            return;
        }

        if (victimPod && victimPod->Eviction().state() != NProto::EEvictionState::ES_NONE) {
            YT_LOG_DEBUG("Swap task is considered not finished; victim pod is not evicted yet");
            return;
        }

        SchedulingStatusSketchAfterVictimEviction_.Update(starvingPod);

        // Ensure at least one scheduling iteration after victim eviction.
        if (SchedulingStatusSketchAfterVictimEviction_.ErrorIterationCount > 1) {
            YT_LOG_DEBUG(
                "Swap task is considered finished; "
                "passed at least one scheduling iteration after victim eviction");
            State_ = ETaskState::Finished;
        } else {
            YT_LOG_DEBUG(
                "Swap task is cosidered not finished; "
                "no evidence of passed scheduling iteration after victim eviction");
        }
    }

private:
    struct TSchedulingStatusSketch
    {
        int ErrorIterationCount = 0;
        TInstant LastErrorDatetime = TInstant::Zero();

        void Update(const TPod* pod)
        {
            auto error = pod->ParseSchedulingError();
            if (error.IsOK()) {
                return;
            }

            auto errorDatetime = ParseErrorDatetime(error);
            if (errorDatetime > LastErrorDatetime) {
                ++ErrorIterationCount;
            }
            LastErrorDatetime = errorDatetime;
        }
    };

    const TLogger Logger;

    const TGuid Id_;
    const TInstant StartTime_;

    const TObjectCompositeId StarvingPodCompositeId_;
    const TObjectCompositeId VictimPodCompositeId_;

    ETaskState State_;

    TSchedulingStatusSketch SchedulingStatusSketchAfterVictimEviction_;
};

using TSwapTaskPtr = TIntrusivePtr<TSwapTask>;

////////////////////////////////////////////////////////////////////////////////

TSwapTaskPtr CreateSwapTask(const IClientPtr& client, TPod* starvingPod, TPod* victimPod)
{
    auto id = TGuid::Create();
    auto starvingPodCompositeId = GetCompositeId(starvingPod);
    auto victimPodCompositeId = GetCompositeId(victimPod);

    YT_LOG_DEBUG("Creating swap task (TaskId: %v, StarvingPod: %v, VictimPod: %v)",
        id,
        starvingPodCompositeId,
        victimPodCompositeId);

    WaitFor(RequestPodEviction(
        client,
        victimPod->GetId(),
        Format("Heavy Scheduler cluster defragmentation (TaskId: %v)", id),
        /* validateDisruptionBudget */ true))
        .ValueOrThrow();

    return New<TSwapTask>(
        std::move(id),
        TInstant::Now(),
        std::move(starvingPodCompositeId),
        std::move(victimPodCompositeId));
}

////////////////////////////////////////////////////////////////////////////////

class TTaskManager
{
public:
    explicit TTaskManager(TDuration taskTimeLimit)
        : TaskTimeLimit_(taskTimeLimit)
        , Profiler_(NProfiling::TProfiler(NHeavyScheduler::Profiler)
            .AppendPath("/task_manager"))
    { }

    void ReconcileState(const TClusterPtr& cluster)
    {
        for (const auto& task : Tasks_) {
            task->ReconcileState(cluster);
        }
    }

    void RemoveFinishedTasks()
    {
        auto now = TInstant::Now();

        int timedOutCount = 0;
        int finishedCount = 0;

        Tasks_.erase(
            std::remove_if(
                Tasks_.begin(),
                Tasks_.end(),
                [&] (const TSwapTaskPtr& task) {
                    if (task->GetState() == ETaskState::Finished) {
                        ++finishedCount;
                        return true;
                    }
                    if (task->GetStartTime() + TaskTimeLimit_ < now) {
                        ++timedOutCount;
                        YT_LOG_DEBUG("Task time limit exceeded (TaskId: %v, StartTime: %v, TimeLimit: %v)",
                            task->GetId(),
                            task->GetStartTime(),
                            TaskTimeLimit_);
                        return true;
                    }
                    return false;
                }),
            Tasks_.end());

        Profiler_.Update(Profiling_.TimedOutCounter, timedOutCount);
        Profiler_.Update(Profiling_.FinishedCounter, finishedCount);
        Profiler_.Update(Profiling_.ActiveCounter, Tasks_.size());
    }

    bool ShouldWait() const
    {
        return !Tasks_.empty();
    }

    void Add(TSwapTaskPtr task)
    {
        Tasks_.push_back(std::move(task));
    }

private:
    const TDuration TaskTimeLimit_;
    const NProfiling::TProfiler Profiler_;

    std::vector<TSwapTaskPtr> Tasks_;

    struct TProfiling
    {
        NProfiling::TSimpleGauge TimedOutCounter{"/timed_out"};
        NProfiling::TSimpleGauge FinishedCounter{"/finished"};
        NProfiling::TSimpleGauge ActiveCounter{"/active"};
    };

    TProfiling Profiling_;
};

////////////////////////////////////////////////////////////////////////////////

class THeavyScheduler::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TBootstrap* bootstrap,
        THeavySchedulerConfigPtr config)
        : Bootstrap_(bootstrap)
        , Config_(std::move(config))
        , IterationExecutor_(New<TPeriodicExecutor>(
            GetCurrentInvoker(),
            BIND(&TImpl::RunIteration, MakeWeak(this)),
            Config_->IterationPeriod))
        , Cluster_(New<TCluster>(
            Logger,
            NProfiling::TProfiler(NHeavyScheduler::Profiler)
                .AppendPath("/cluster"),
            CreateClusterReader(
                Config_->ClusterReader,
                Bootstrap_->GetClient()),
            CreateLabelFilterEvaluator()))
        , TaskManager_(Config_->TaskTimeLimit)
    { }

    void Initialize()
    {
        IterationExecutor_->Start();
    }

private:
    TBootstrap* const Bootstrap_;
    const THeavySchedulerConfigPtr Config_;

    DECLARE_THREAD_AFFINITY_SLOT(IterationThread);
    const TPeriodicExecutorPtr IterationExecutor_;

    TClusterPtr Cluster_;

    TTaskManager TaskManager_;

    struct TProfiling
    {
        NProfiling::TSimpleGauge VictimSearchFailureCounter{"/victim_search_failure"};
        NProfiling::TSimpleGauge UnhealthyClusterCounter{"/unhealthy_cluster"};
    };

    TProfiling Profiling_;

    void RunIteration()
    {
        VERIFY_THREAD_AFFINITY(IterationThread);

        // This check is just a best-effort. It is possible to have more than one running iteration.
        //
        // Generally mechanism of prerequisite transactions can provide guarantee of no more than one
        // running iteration, but YP master storage does not support it yet.
        if (!Bootstrap_->GetYTConnector()->IsLeading()) {
            YT_LOG_DEBUG("Instance is not leading; skipping Heavy Scheduler iteration");
            return;
        }

        try {
            GuardedRunIteration();
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Error running Heavy Scheduler iteration");
        }
    }

    void GuardedRunIteration()
    {
        Cluster_->LoadSnapshot(New<TClusterConfig>());

        TaskManager_.ReconcileState(Cluster_);
        TaskManager_.RemoveFinishedTasks();
        if (TaskManager_.ShouldWait()) {
            YT_LOG_DEBUG("Waiting for the tasks to finish");
            return;
        }

        if (!CheckClusterHealth()) {
            Profiler.Update(Profiling_.UnhealthyClusterCounter, 1);
            return;
        }

        auto starvingPods = FindStarvingPods();
        if (starvingPods.empty()) {
            YT_LOG_DEBUG("There are no starving pods; skipping iteration");
            return;
        }

        auto* starvingPod = starvingPods[RandomNumber<size_t>(starvingPods.size())];
        YT_LOG_DEBUG("Randomly picked starving pod (PodId: %v, SchedulingError: %v)",
            starvingPod->GetId(),
            starvingPod->ParseSchedulingError());

        const auto& starvingPodFilteredNodesOrError = GetFilteredNodes(starvingPod);
        if (!starvingPodFilteredNodesOrError.IsOK()) {
            YT_LOG_DEBUG(starvingPodFilteredNodesOrError,
                "Error filltering starving pod suitable nodes (StarvingPodId: %v)",
                starvingPod->GetId());
            return;
        }
        const auto& starvingPodFilteredNodes = starvingPodFilteredNodesOrError.Value();

        auto starvingPodSuitableNodes = FindSuitableNodes(
            starvingPod,
            starvingPodFilteredNodes,
            /* limit */ 1);
        if (starvingPodSuitableNodes.size() > 0) {
            YT_LOG_DEBUG("Found suitable node for starving pod (PodId: %v, NodeId: %v)",
                starvingPod->GetId(),
                starvingPodSuitableNodes[0]->GetId());
            return;
        }

        auto* victimPod = FindVictimPod(
            starvingPod,
            starvingPodFilteredNodes);
        if (!victimPod) {
            YT_LOG_DEBUG("Could not find victim pod");
            Profiler.Update(Profiling_.VictimSearchFailureCounter, 1);
            return;
        }

        YT_LOG_DEBUG("Found victim pod (PodId: %v)",
            victimPod->GetId());

        TaskManager_.Add(CreateSwapTask(
            Bootstrap_->GetClient(),
            starvingPod,
            victimPod));
    }

    std::vector<TPod*> GetNodeSegmentSchedulablePods() const
    {
        auto result = Cluster_->GetSchedulablePods();
        result.erase(
            std::remove_if(
                result.begin(),
                result.end(),
                [&] (auto* pod) {
                    return Config_->NodeSegment != pod->GetPodSet()->GetNodeSegment()->GetId();
                }),
            result.end());
        return result;
    }

    int GetPodEvictionCount() const
    {
        int result = 0;
        for (const auto* pod : GetNodeSegmentSchedulablePods()) {
            if (pod->Eviction().state() != NProto::EEvictionState::ES_NONE) {
                result += 1;
            }
        }
        return result;
    }

    bool CheckClusterHealth() const
    {
        auto clusterPodEvictionCount = GetPodEvictionCount();
        if (clusterPodEvictionCount > Config_->SafeClusterPodEvictionCount) {
            YT_LOG_WARNING("Cluster is unhealthy (EvictionCount: %v, SafeEvictionCount: %v)",
                clusterPodEvictionCount,
                Config_->SafeClusterPodEvictionCount);
            return false;
        }
        YT_LOG_DEBUG("Cluster is healthy (EvictionCount: %v)",
            clusterPodEvictionCount);
        return true;
    }

    std::vector<TPod*> FindStarvingPods() const
    {
        std::vector<TPod*> result;
        for (auto* pod : GetNodeSegmentSchedulablePods()) {
            if (pod->GetNode()) {
                continue;
            }
            if (!pod->ParseSchedulingError().IsOK()) {
                result.push_back(pod);
            }
        }
        YT_LOG_DEBUG_UNLESS(result.empty(), "Found starving pods (Count: %v)",
            result.size());
        return result;
    }

    TPod* FindVictimPod(
        TPod* starvingPod,
        const std::vector<TNode*>& starvingPodFilteredNodes) const
    {
        THashSet<TNode*> starvingPodFilteredNodeSet;
        for (auto* node : starvingPodFilteredNodes) {
            starvingPodFilteredNodeSet.insert(node);
        }

        std::vector<TPod*> victimCandidatePods = GetNodeSegmentSchedulablePods();
        victimCandidatePods.erase(
            std::remove_if(
                victimCandidatePods.begin(),
                victimCandidatePods.end(),
                [&] (TPod* pod) {
                    return !pod->GetNode() ||
                        starvingPodFilteredNodeSet.find(pod->GetNode()) == starvingPodFilteredNodeSet.end();
                }),
            victimCandidatePods.end());

        if (static_cast<int>(victimCandidatePods.size()) > Config_->VictimCandidatePodCount) {
            YT_LOG_DEBUG("Randomly selecting victim candidates (TotalCount: %v, RandomSelectionCount: %v)",
                victimCandidatePods.size(),
                Config_->VictimCandidatePodCount);
            Shuffle(victimCandidatePods.begin(), victimCandidatePods.end());
            victimCandidatePods.resize(Config_->VictimCandidatePodCount);
        }

        YT_LOG_DEBUG("Selected victim pod candidates (Count: %v)",
            victimCandidatePods.size());

        for (auto* victimPod : victimCandidatePods) {
            auto* node = victimPod->GetNode();

            if (!node->CanAllocateAntiaffinityVacancies(starvingPod)) {
                YT_LOG_DEBUG_IF(Config_->Verbose,
                    "Not enough antiaffinity vacancies (NodeId: %v, StarvingPodId: %v)",
                    node->GetId(),
                    starvingPod->GetId());
                continue;
            }

            auto starvingPodResourceVector = GetResourceRequestVector(starvingPod);
            auto victimPodResourceVector = GetResourceRequestVector(victimPod);
            auto freeNodeResourceVector = GetFreeResourceVector(node);
            if (freeNodeResourceVector + victimPodResourceVector < starvingPodResourceVector) {
                YT_LOG_DEBUG_IF(Config_->Verbose,
                    "Not enough resources according to resource vectors (NodeId: %v, VictimPodId: %v, StarvingPodId: %v)",
                    node->GetId(),
                    victimPod->GetId(),
                    starvingPod->GetId());
                continue;
            }

            if (!IsSafeToEvict(victimPod)) {
                continue;
            }

            return victimPod;
        }

        return nullptr;
    }

    const TErrorOr<std::vector<TNode*>>& GetFilteredNodes(TPod* pod) const
    {
        auto* nodeSegmentCache = pod->GetPodSet()->GetNodeSegment()->GetSchedulableNodeFilterCache();
        return nodeSegmentCache->Get(NObjects::TObjectFilter{pod->GetEffectiveNodeFilter()});
    }

    std::vector<TNode*> FindSuitableNodes(
        TPod* pod,
        const std::vector<TNode*>& nodes,
        std::optional<int> limit) const
    {
        std::vector<TNode*> result;
        if (limit) {
            YT_VERIFY(*limit >= 0);
            result.reserve(*limit);
        }
        TAllocator allocator;
        for (auto* node : nodes) {
            if (limit && static_cast<int>(result.size()) >= *limit) {
                break;
            }
            if (allocator.CanAllocate(node, pod)) {
                result.push_back(node);
            }
        }
        return result;
    }

    TErrorOr<std::vector<TNode*>> FindSuitableNodes(
        TPod* pod,
        std::optional<int> limit) const
    {
        const auto& nodesOrError = GetFilteredNodes(pod);
        if (!nodesOrError.IsOK()) {
            return TError("Error filtering nodes")
                << nodesOrError;
        }
        return FindSuitableNodes(pod, nodesOrError.Value(), limit);
    }

    bool IsSafeToEvict(TPod* pod) const
    {
        YT_VERIFY(pod->GetNode());
        YT_VERIFY(pod->GetEnableScheduling());

        YT_LOG_DEBUG_IF(Config_->Verbose,
            "Checking eviction safety (PodId: %v)",
            pod->GetId());

        if (auto error = pod->GetSchedulingAttributesValidationError(); !error.IsOK()) {
            YT_LOG_DEBUG_IF(Config_->Verbose,
                "Cannot safely evict pod due to scheduilng attributes validation error (PodId: %v, Error: %v)",
                pod->GetId(),
                error);
            return false;
        }

        if (pod->Eviction().state() != NProto::EEvictionState::ES_NONE) {
            YT_LOG_DEBUG_IF(Config_->Verbose,
                "Cannot safely evict pod because it is not in none eviction state (PodId: %v)",
                pod->GetId());
            return false;
        }

        if (Config_->ValidatePodDisruptionBudget) {
            if (const auto* podDisruptionBudget = pod->GetPodSet()->GetPodDisruptionBudget()) {
                if (podDisruptionBudget->Status().allowed_pod_disruptions() <= 0) {
                    YT_LOG_DEBUG_IF(Config_->Verbose,
                        "Cannot safely evict pod because of zero disruption budget (PodId: %v, PodDisruptionBudgetId: %v)",
                        pod->GetId(),
                        podDisruptionBudget->GetId());
                    return false;
                }
            } else {
                YT_LOG_DEBUG_IF(Config_->Verbose,
                    "Cannot safely evict pod because it is not attached to a disruption budget (PodId: %v)",
                    pod->GetId());
                return false;
            }
        }

        auto suitableNodesOrError = FindSuitableNodes(pod, Config_->SafeSuitableNodeCount);
        if (!suitableNodesOrError.IsOK()) {
            YT_LOG_DEBUG_IF(Config_->Verbose,
                suitableNodesOrError,
                "Error finding suitable nodes (PodId: %v)",
                pod->GetId());
            return false;
        }
        const auto& suitableNodes = suitableNodesOrError.Value();

        YT_LOG_DEBUG_IF(Config_->Verbose,
            "Found suitable nodes (PodId: %v, SuitableNodeCount: %v)",
            pod->GetId(),
            suitableNodes.size());

        if (static_cast<int>(suitableNodes.size()) < Config_->SafeSuitableNodeCount) {
            YT_LOG_DEBUG_IF(Config_->Verbose,
                "Cannot safely evict pod due to lack of suitable nodes "
                "(PodId: %v, SuitableNodeCount: %v, SafeSuitableNodeCount: %v)",
                pod->GetId(),
                suitableNodes.size(),
                Config_->SafeSuitableNodeCount);
            return false;
        }

        return true;
    }
};

////////////////////////////////////////////////////////////////////////////////

THeavyScheduler::THeavyScheduler(
    TBootstrap* bootstrap,
    THeavySchedulerConfigPtr config)
    : Impl_(New<TImpl>(bootstrap, std::move(config)))
{ }

void THeavyScheduler::Initialize()
{
    Impl_->Initialize();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NHeavyScheduler
