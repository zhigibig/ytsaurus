#include "controller_agent_tracker.h"
#include "scheduler.h"
#include "scheduler_strategy.h"
#include "controller_agent.h"
#include "operation.h"
#include "node_shard.h"
#include "operation_controller_impl.h"
#include "scheduling_context.h"
#include "master_connector.h"
#include "bootstrap.h"

#include <yt/yt/server/lib/scheduler/config.h>
#include <yt/yt/server/lib/scheduler/helpers.h>
#include <yt/yt/server/lib/scheduler/job_metrics.h>

#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/client/api/transaction.h>

#include <yt/yt/ytlib/node_tracker_client/channel.h>

#include <yt/yt/core/concurrency/thread_affinity.h>
#include <yt/yt/core/concurrency/lease_manager.h>

#include <yt/yt/core/yson/public.h>

#include <yt/yt/build/build.h>

#include <util/string/join.h>

namespace NYT::NScheduler {

using namespace NConcurrency;
using namespace NRpc;
using namespace NYson;
using namespace NYTree;
using namespace NControllerAgent;

using NJobTrackerClient::TReleaseJobFlags;

using NYT::FromProto;
using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerLogger;

////////////////////////////////////////////////////////////////////////////////

struct TOperationInfo
{
    TOperationId OperationId;
    TOperationJobMetrics JobMetrics;
    THashMap<EOperationAlertType, TError> AlertMap;
    TControllerRuntimeDataPtr ControllerRuntimeData;
    TYsonString SuspiciousJobsYson;
};

void FromProto(TOperationInfo* operationInfo, const NProto::TOperationInfo& operationInfoProto)
{
    operationInfo->OperationId = FromProto<TOperationId>(operationInfoProto.operation_id());
    operationInfo->JobMetrics = FromProto<TOperationJobMetrics>(operationInfoProto.job_metrics());
    if (operationInfoProto.has_alerts()) {
        THashMap<EOperationAlertType, TError> alertMap;
        for (const auto& protoAlert : operationInfoProto.alerts().alerts()) {
            alertMap[EOperationAlertType(protoAlert.type())] = FromProto<TError>(protoAlert.error());
        }
        operationInfo->AlertMap = alertMap;
    }

    if (operationInfoProto.has_suspicious_jobs()) {
        operationInfo->SuspiciousJobsYson = TYsonString(operationInfoProto.suspicious_jobs(), EYsonType::MapFragment);
    } else {
        operationInfo->SuspiciousJobsYson = TYsonString();
    }

    auto controllerData = New<TControllerRuntimeData>();
    controllerData->SetPendingJobCount(operationInfoProto.pending_job_count());
    controllerData->SetNeededResources(FromProto<TJobResources>(operationInfoProto.needed_resources()));
    controllerData->MinNeededJobResources() = FromProto<TJobResourcesWithQuotaList>(operationInfoProto.min_needed_job_resources());
    operationInfo->ControllerRuntimeData = std::move(controllerData);
}

////////////////////////////////////////////////////////////////////////////////

class TControllerAgentTracker::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TSchedulerConfigPtr config,
        TBootstrap* bootstrap)
        : SchedulerConfig_(std::move(config))
        , Config_(SchedulerConfig_->ControllerAgentTracker)
        , Bootstrap_(bootstrap)
    { }

    void Initialize()
    {
        auto* masterConnector = Bootstrap_->GetScheduler()->GetMasterConnector();
        masterConnector->SubscribeMasterConnected(BIND(
            &TImpl::OnMasterConnected,
            Unretained(this)));
        masterConnector->SubscribeMasterDisconnected(BIND(
            &TImpl::OnMasterDisconnected,
            Unretained(this)));

        masterConnector->AddCommonWatcher(
            BIND(&TImpl::RequestControllerAgentInstances, Unretained(this)),
            BIND(&TImpl::HandleControllerAgentInstances, Unretained(this)));
    }

    std::vector<TControllerAgentPtr> GetAgents() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        std::vector<TControllerAgentPtr> result;
        result.reserve(IdToAgent_.size());
        for (const auto& [agentId, agent] : IdToAgent_) {
            result.push_back(agent);
        }
        return result;
    }

    IOperationControllerPtr CreateController(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return New<TOperationControllerImpl>(Bootstrap_, SchedulerConfig_, operation);
    }

    TControllerAgentPtr PickAgentForOperation(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto controllerAgentTag = operation->Spec()->ControllerAgentTag;

        if (!AgentTagsFetched_ || TagsWithTooFewAgents_.contains(controllerAgentTag)) {
            YT_LOG_DEBUG(
                "Failed to pick agent for operation (OperationId: %v, ControllerAgentTag: %v",
                operation->GetId(),
                controllerAgentTag);

            return nullptr;
        }

        int excludedByTagCount = 0;

        std::vector<TControllerAgentPtr> aliveAgents;
        for (const auto& [agentId, agent] : IdToAgent_) {
            if (agent->GetState() != EControllerAgentState::Registered) {
                continue;
            }
            if (!agent->GetTags().contains(controllerAgentTag)) {
                ++excludedByTagCount;
                continue;
            }
            aliveAgents.push_back(agent);
        }

        switch (Config_->AgentPickStrategy) {
            case EControllerAgentPickStrategy::Random: {
                std::vector<TControllerAgentPtr> agents;
                for (const auto& agent : aliveAgents) {
                    auto memoryStatistics = agent->GetMemoryStatistics();
                    if (memoryStatistics) {
                        auto minAgentAvailableMemory = std::max(
                            Config_->MinAgentAvailableMemory,
                            static_cast<i64>(Config_->MinAgentAvailableMemoryFraction * memoryStatistics->Limit));
                        if (memoryStatistics->Usage + minAgentAvailableMemory >= memoryStatistics->Limit) {
                            continue;
                        }
                    }
                    agents.push_back(agent);
                }

                return agents.empty() ? nullptr : agents[RandomNumber(agents.size())];
            }
            case EControllerAgentPickStrategy::MemoryUsageBalanced: {
                TControllerAgentPtr pickedAgent;
                double scoreSum = 0.0;
                for (const auto& agent : aliveAgents) {
                    auto memoryStatistics = agent->GetMemoryStatistics();
                    if (!memoryStatistics) {
                        YT_LOG_WARNING("Controller agent skipped since it did not report memory information "
                            "and memory usage balanced pick strategy used (AgentId: %v)",
                            agent->GetId());
                        continue;
                    }

                    auto minAgentAvailableMemory = std::max(
                        Config_->MinAgentAvailableMemory,
                        static_cast<i64>(Config_->MinAgentAvailableMemoryFraction * memoryStatistics->Limit));
                    if (memoryStatistics->Usage + minAgentAvailableMemory >= memoryStatistics->Limit) {
                        continue;
                    }

                    i64 freeMemory = std::max(static_cast<i64>(0), memoryStatistics->Limit - memoryStatistics->Usage);
                    double rawScore = static_cast<double>(freeMemory) / memoryStatistics->Limit;
                    double score = std::pow(rawScore, Config_->MemoryBalancedPickStrategyScorePower);

                    scoreSum += score;
                    if (RandomNumber<float>() <= static_cast<float>(score) / scoreSum) {
                        pickedAgent = agent;
                    }
                }
                return pickedAgent;
            }
            default:
                YT_ABORT();
        }
    }

    void AssignOperationToAgent(
        const TOperationPtr& operation,
        const TControllerAgentPtr& agent)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YT_VERIFY(agent->Operations().insert(operation).second);
        operation->SetAgent(agent.Get());

        YT_LOG_INFO("Operation assigned to agent (AgentId: %v, Tags: %v, OperationId: %v)",
            agent->GetId(),
            agent->GetTags(),
            operation->GetId());
    }


    void HandleAgentFailure(
        const TControllerAgentPtr& agent,
        const TError& error)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        YT_LOG_WARNING(error, "Agent failed; unregistering (AgentId: %v, IncarnationId: %v)",
            agent->GetId(),
            agent->GetIncarnationId());

        Bootstrap_->GetControlInvoker(EControlQueue::AgentTracker)->Invoke(
            BIND(&TImpl::UnregisterAgent, MakeStrong(this), agent));
    }


    void UnregisterOperationFromAgent(const TOperationPtr& operation)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto agent = operation->FindAgent();
        if (!agent) {
            return;
        }

        YT_VERIFY(agent->Operations().erase(operation) == 1);

        YT_LOG_DEBUG("Operation unregistered from agent (AgentId: %v, OperationId: %v)",
            agent->GetId(),
            operation->GetId());
    }

    void UpdateConfig(TSchedulerConfigPtr config)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        SchedulerConfig_ = std::move(config);
        Config_ = SchedulerConfig_->ControllerAgentTracker;
    }

    TControllerAgentPtr FindAgent(const TAgentId& id)
    {
        auto it = IdToAgent_.find(id);
        return it == IdToAgent_.end() ? nullptr : it->second;
    }

    TControllerAgentPtr GetAgentOrThrow(const TAgentId& id)
    {
        auto agent = FindAgent(id);
        if (!agent) {
            THROW_ERROR_EXCEPTION("Agent %v is not registered",
                id);
        }
        return agent;
    }


    void ProcessAgentHandshake(const TCtxAgentHandshakePtr& context)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        const auto& scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        auto* request = &context->Request();
        auto* response = &context->Response();

        const auto& agentId = request->agent_id();
        auto existingAgent = FindAgent(agentId);
        if (existingAgent) {
            auto state = existingAgent->GetState();
            if (state == EControllerAgentState::Registered || state == EControllerAgentState::WaitingForInitialHeartbeat) {
                YT_LOG_INFO("Kicking out agent due to id conflict (AgentId: %v, ExistingIncarnationId: %v)",
                    agentId,
                    existingAgent->GetIncarnationId());
                UnregisterAgent(existingAgent);
            }

            context->Reply(TError("Agent %Qv is in %Qlv state; please retry",
                agentId,
                state));
            return;
        }

        auto agent = [&] {
            auto addresses = FromProto<NNodeTrackerClient::TAddressMap>(request->agent_addresses());
            auto tags = FromProto<THashSet<TString>>(request->tags());
            // COMPAT(gritukan): Remove it when controller agents will be fresh enough.
            if (tags.empty()) {
                tags.insert(DefaultOperationTag);
            }

            auto address = NNodeTrackerClient::GetAddressOrThrow(addresses, Bootstrap_->GetLocalNetworks());
            auto channel = Bootstrap_->GetMasterClient()->GetChannelFactory()->CreateChannel(address);

            YT_LOG_INFO("Registering agent (AgentId: %v, Addresses: %v, Tags: %v)",
                agentId,
                addresses,
                tags);

            auto agent = New<TControllerAgent>(
                agentId,
                std::move(addresses),
                std::move(tags),
                std::move(channel),
                Bootstrap_->GetControlInvoker(EControlQueue::AgentTracker));
            agent->SetState(EControllerAgentState::Registering);
            RegisterAgent(agent);

            return agent;
        }();

        YT_LOG_INFO("Starting agent incarnation transaction (AgentId: %v)",
            agentId);

        NApi::TTransactionStartOptions options;
        options.Timeout = Config_->IncarnationTransactionTimeout;
        if (Config_->IncarnationTransactionPingPeriod) {
            options.PingPeriod = Config_->IncarnationTransactionPingPeriod;
        }
        auto attributes = CreateEphemeralAttributes();
        attributes->Set("title", Format("Controller agent incarnation for %v", agentId));
        options.Attributes = std::move(attributes);
        const auto& lockTransaction = Bootstrap_->GetScheduler()->GetMasterConnector()->GetLockTransaction();
        lockTransaction->StartTransaction(NTransactionClient::ETransactionType::Master, options)
            .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<NApi::ITransactionPtr>& transactionOrError) {
                VERIFY_THREAD_AFFINITY(ControlThread);

                if (!transactionOrError.IsOK()) {
                    Bootstrap_->GetScheduler()->Disconnect(transactionOrError);
                    return;
                }

                if (agent->GetState() != EControllerAgentState::Registering) {
                    return;
                }

                const auto& transaction = transactionOrError.Value();
                agent->SetIncarnationTransaction(transaction);
                agent->SetState(EControllerAgentState::WaitingForInitialHeartbeat);

                agent->SetLease(TLeaseManager::CreateLease(
                    Config_->HeartbeatTimeout,
                    BIND(&TImpl::OnAgentHeartbeatTimeout, MakeWeak(this), MakeWeak(agent))
                        .Via(GetCancelableControlInvoker())));

                transaction->SubscribeAborted(
                    BIND(&TImpl::OnAgentIncarnationTransactionAborted, MakeWeak(this), MakeWeak(agent))
                        .Via(GetCancelableControlInvoker()));

                YT_LOG_INFO("Agent incarnation transaction started (AgentId: %v, IncarnationId: %v)",
                    agentId,
                    agent->GetIncarnationId());

                context->SetResponseInfo("IncarnationId: %v",
                    agent->GetIncarnationId());
                ToProto(response->mutable_incarnation_id(), agent->GetIncarnationId());
                response->set_config(ConvertToYsonString(SchedulerConfig_).ToString());
                response->set_scheduler_version(GetVersion());
                context->Reply();
            })
            .Via(GetCancelableControlInvoker()));
    }

    void ProcessAgentHeartbeat(const TCtxAgentHeartbeatPtr& context)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        const auto& scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        auto* request = &context->Request();
        auto* response = &context->Response();

        const auto& agentId = request->agent_id();
        auto incarnationId = FromProto<NControllerAgent::TIncarnationId>(request->incarnation_id());

        context->SetRequestInfo("AgentId: %v, IncarnationId: %v, OperationCount: %v",
            agentId,
            incarnationId,
            request->operations_size());

        auto agent = GetAgentOrThrow(agentId);
        if (agent->GetState() != EControllerAgentState::Registered && agent->GetState() != EControllerAgentState::WaitingForInitialHeartbeat) {
            context->Reply(TError("Agent %Qv is in %Qlv state",
                agentId,
                agent->GetState()));
            return;
        }
        if (incarnationId != agent->GetIncarnationId()) {
            context->Reply(TError("Wrong agent incarnation id: expected %v, got %v",
                agent->GetIncarnationId(),
                incarnationId));
            return;
        }
        if (agent->GetState() == EControllerAgentState::WaitingForInitialHeartbeat) {
            YT_LOG_INFO("Agent registration confirmed by heartbeat");
            agent->SetState(EControllerAgentState::Registered);
        }

        TLeaseManager::RenewLease(agent->GetLease(), Config_->HeartbeatTimeout);

        SwitchTo(agent->GetCancelableInvoker());

        std::vector<TOperationInfo> operationInfos;
        auto parseOperationsFuture = BIND([&operationsProto = request->operations(), &operationInfos = operationInfos] () {
                operationInfos.reserve(operationsProto.size());
                for (const auto& operationInfoProto : operationsProto) {
                    operationInfos.emplace_back(FromProto<TOperationInfo>(operationInfoProto));
                }
            })
            .AsyncVia(NRpc::TDispatcher::Get()->GetHeavyInvoker())
            .Run();
        WaitFor(parseOperationsFuture)
            .ThrowOnError();

        TOperationIdToOperationJobMetrics operationIdToOperationJobMetrics;
        for (const auto& operationInfo : operationInfos) {
            auto operationId = operationInfo.OperationId;
            auto operation = scheduler->FindOperation(operationId);
            if (!operation) {
                // TODO(eshcherbin): This is used for flap diagnostics. Remove when TestPoolMetricsPorto is fixed (YT-12207).
                THashMap<TString, i64> treeIdToOperationTotalTimeDelta;
                for (const auto& [treeId, metrics] : operationInfo.JobMetrics) {
                    treeIdToOperationTotalTimeDelta.emplace(treeId, metrics.Values()[EJobMetricName::TotalTime]);
                }

                YT_LOG_DEBUG("Unknown operation is running at agent; unregister requested (AgentId: %v, OperationId: %v, TreeIdToOperationTotalTimeDelta: %v)",
                    agent->GetId(),
                    operationId,
                    treeIdToOperationTotalTimeDelta);
                ToProto(response->add_operation_ids_to_unregister(), operationId);
                continue;
            }
            YT_VERIFY(operationIdToOperationJobMetrics.emplace(operationId, std::move(operationInfo.JobMetrics)).second);

            // TODO(ignat): remove/refactor this log message after fixing the bug.
            if (!operationInfo.AlertMap.empty()) {
                YT_LOG_DEBUG("Received alert information (OperationId: %v)", operation->GetId());
            }
            for (const auto& [alertType, alert] : operationInfo.AlertMap) {
                if (alert.IsOK()) {
                    operation->ResetAlert(alertType);
                    YT_LOG_DEBUG("Reset alert (OperationId: %v, AlertType: %v)", operation->GetId(), alertType);
                } else {
                    operation->SetAlert(alertType, alert);
                    YT_LOG_DEBUG("Set alert (OperationId: %v, AlertType: %v)", operation->GetId(), alertType);
                }
            }

            if (operationInfo.SuspiciousJobsYson) {
                operation->SetSuspiciousJobs(operationInfo.SuspiciousJobsYson);
            }

            operation->GetController()->SetControllerRuntimeData(operationInfo.ControllerRuntimeData);
        }

        scheduler->GetStrategy()->ApplyJobMetricsDelta(std::move(operationIdToOperationJobMetrics));

        const auto& nodeShards = scheduler->GetNodeShards();
        int nodeShardCount = static_cast<int>(nodeShards.size());

        std::vector<std::vector<const NProto::TAgentToSchedulerJobEvent*>> groupedJobEvents(nodeShards.size());
        std::vector<std::vector<const NProto::TScheduleJobResponse*>> groupedScheduleJobResponses(nodeShards.size());

        RunInMessageOffloadThread([&] {
            agent->GetJobEventsInbox()->HandleIncoming(
                request->mutable_agent_to_scheduler_job_events(),
                [&] (auto* protoEvent) {
                    auto jobId = FromProto<TJobId>(protoEvent->job_id());
                    auto shardId = scheduler->GetNodeShardId(NodeIdFromJobId(jobId));
                    groupedJobEvents[shardId].push_back(protoEvent);
                });
            agent->GetJobEventsInbox()->ReportStatus(
                response->mutable_agent_to_scheduler_job_events());

            agent->GetScheduleJobResponsesInbox()->HandleIncoming(
                request->mutable_agent_to_scheduler_schedule_job_responses(),
                [&] (auto* protoEvent) {
                    auto jobId = FromProto<TJobId>(protoEvent->job_id());
                    auto shardId = scheduler->GetNodeShardId(NodeIdFromJobId(jobId));
                    groupedScheduleJobResponses[shardId].push_back(protoEvent);
                });
            agent->GetScheduleJobResponsesInbox()->ReportStatus(
                response->mutable_agent_to_scheduler_schedule_job_responses());

            agent->GetJobEventsOutbox()->HandleStatus(
                request->scheduler_to_agent_job_events());
            agent->GetJobEventsOutbox()->BuildOutcoming(
                response->mutable_scheduler_to_agent_job_events(),
                [] (auto* protoEvent, const auto& event) {
                    ToProto(protoEvent->mutable_operation_id(), event.OperationId);
                    protoEvent->set_event_type(static_cast<int>(event.EventType));
                    protoEvent->set_log_and_profile(event.LogAndProfile);
                    protoEvent->mutable_status()->CopyFrom(*event.Status);
                    protoEvent->set_start_time(ToProto<ui64>(event.StartTime));
                    if (event.FinishTime) {
                        protoEvent->set_finish_time(ToProto<ui64>(*event.FinishTime));
                    }
                    if (event.Abandoned) {
                        protoEvent->set_abandoned(*event.Abandoned);
                    }
                    if (event.AbortReason) {
                        protoEvent->set_abort_reason(static_cast<int>(*event.AbortReason));
                    }
                    if (event.InterruptReason) {
                        protoEvent->set_interrupt_reason(static_cast<int>(*event.InterruptReason));
                    }
                    if (event.AbortedByScheduler) {
                        protoEvent->set_aborted_by_scheduler(*event.AbortedByScheduler);
                    }
                    if (event.PreemptedFor) {
                        ToProto(protoEvent->mutable_preempted_for(), *event.PreemptedFor);
                    }
                });

            agent->GetOperationEventsOutbox()->HandleStatus(
                request->scheduler_to_agent_operation_events());
            agent->GetOperationEventsOutbox()->BuildOutcoming(
                response->mutable_scheduler_to_agent_operation_events(),
                [] (auto* protoEvent, const auto& event) {
                    protoEvent->set_event_type(static_cast<int>(event.EventType));
                    ToProto(protoEvent->mutable_operation_id(), event.OperationId);
                });

            agent->GetScheduleJobRequestsOutbox()->HandleStatus(
                request->scheduler_to_agent_schedule_job_requests());
            agent->GetScheduleJobRequestsOutbox()->BuildOutcoming(
                response->mutable_scheduler_to_agent_schedule_job_requests(),
                [] (auto* protoRequest, const auto& request) {
                    ToProto(protoRequest, *request);
                });
        });

        agent->GetOperationEventsInbox()->HandleIncoming(
            request->mutable_agent_to_scheduler_operation_events(),
            [&] (auto* protoEvent) {
                auto eventType = static_cast<EAgentToSchedulerOperationEventType>(protoEvent->event_type());
                auto operationId = FromProto<TOperationId>(protoEvent->operation_id());
                auto controllerEpoch = protoEvent->controller_epoch();
                auto error = FromProto<TError>(protoEvent->error());
                auto operation = scheduler->FindOperation(operationId);
                if (!operation) {
                    return;
                }

                if (operation->ControllerEpoch() != controllerEpoch) {
                    YT_LOG_DEBUG("Received operation event with unexpected controller epoch; ignored "
                        "(OperationId: %v, ControllerEpoch: %v, EventType: %v)",
                        operationId,
                        controllerEpoch,
                        eventType);
                    return;
                }

                switch (eventType) {
                    case EAgentToSchedulerOperationEventType::Completed:
                        scheduler->OnOperationCompleted(operation);
                        break;
                    case EAgentToSchedulerOperationEventType::Suspended:
                        scheduler->OnOperationSuspended(operation, error);
                        break;
                    case EAgentToSchedulerOperationEventType::Aborted:
                        scheduler->OnOperationAborted(operation, error);
                        break;
                    case EAgentToSchedulerOperationEventType::Failed:
                        scheduler->OnOperationFailed(operation, error);
                        break;
                    case EAgentToSchedulerOperationEventType::BannedInTentativeTree: {
                        auto treeId = protoEvent->tentative_tree_id();
                        auto jobIds = FromProto<std::vector<TJobId>>(protoEvent->tentative_tree_job_ids());
                        scheduler->OnOperationBannedInTentativeTree(operation, treeId, jobIds);
                        break;
                    }
                    case EAgentToSchedulerOperationEventType::InitializationFinished: {
                        TErrorOr<TOperationControllerInitializeResult> resultOrError;
                        if (error.IsOK()) {
                            YT_ASSERT(protoEvent->has_initialize_result());

                            TOperationControllerInitializeResult result;
                            FromProto(
                                &result,
                                protoEvent->initialize_result(),
                                operationId,
                                Bootstrap_,
                                SchedulerConfig_->OperationTransactionPingPeriod);

                            resultOrError = std::move(result);
                        } else {
                            resultOrError = std::move(error);
                        }

                        operation->GetController()->OnInitializationFinished(resultOrError);
                        break;
                    }
                    case EAgentToSchedulerOperationEventType::PreparationFinished: {
                        TErrorOr<TOperationControllerPrepareResult> resultOrError;
                        if (error.IsOK()) {
                            YT_ASSERT(protoEvent->has_prepare_result());
                            resultOrError = FromProto<TOperationControllerPrepareResult>(protoEvent->prepare_result());
                        } else {
                            resultOrError = std::move(error);
                        }

                        operation->GetController()->OnPreparationFinished(resultOrError);
                        break;
                    }
                    case EAgentToSchedulerOperationEventType::MaterializationFinished: {
                        TErrorOr<TOperationControllerMaterializeResult> resultOrError;
                        if (error.IsOK()) {
                            YT_ASSERT(protoEvent->has_materialize_result());
                            resultOrError = FromProto<TOperationControllerMaterializeResult>(protoEvent->materialize_result());
                        } else {
                            resultOrError = std::move(error);
                        }

                        operation->GetController()->OnMaterializationFinished(resultOrError);
                        break;
                    }
                    case EAgentToSchedulerOperationEventType::RevivalFinished: {
                        TErrorOr<TOperationControllerReviveResult> resultOrError;
                        if (error.IsOK()) {
                            YT_ASSERT(protoEvent->has_revive_result());

                            TOperationControllerReviveResult result;
                            FromProto(
                                &result,
                                protoEvent->revive_result(),
                                operationId,
                                incarnationId,
                                operation->GetController()->GetPreemptionMode());

                            resultOrError = std::move(result);
                        } else {
                            resultOrError = std::move(error);
                        }

                        operation->GetController()->OnRevivalFinished(resultOrError);
                        break;
                    }
                    case EAgentToSchedulerOperationEventType::CommitFinished: {
                        TErrorOr<TOperationControllerCommitResult> resultOrError;
                        if (error.IsOK()) {
                            YT_ASSERT(protoEvent->has_commit_result());
                            resultOrError = FromProto<TOperationControllerCommitResult>(protoEvent->commit_result());
                        } else {
                            resultOrError = std::move(error);
                        }

                        operation->GetController()->OnCommitFinished(resultOrError);
                        break;
                    }
                    default:
                        YT_ABORT();
                }
            });
        agent->GetOperationEventsInbox()->ReportStatus(
            response->mutable_agent_to_scheduler_operation_events());

        if (request->has_controller_memory_limit()) {
            agent->SetMemoryStatistics(TControllerAgentMemoryStatistics{request->controller_memory_limit(), request->controller_memory_usage()});
        }

        if (request->exec_nodes_requested()) {
            RunInMessageOffloadThread([&] {
                auto descriptors = scheduler->GetCachedExecNodeDescriptors();
                for (const auto& [_, descriptor] : *descriptors) {
                    ToProto(response->mutable_exec_nodes()->add_exec_nodes(), descriptor);
                }
            });
        }

        for (int shardId = 0; shardId < nodeShardCount; ++shardId) {
            scheduler->GetCancelableNodeShardInvoker(shardId)->Invoke(
                BIND([
                    context,
                    nodeShard = nodeShards[shardId],
                    protoEvents = std::move(groupedJobEvents[shardId]),
                    protoResponses = std::move(groupedScheduleJobResponses[shardId])
                ] {
                    for (const auto* protoEvent : protoEvents) {
                        auto eventType = static_cast<EAgentToSchedulerJobEventType>(protoEvent->event_type());
                        auto jobId = FromProto<TJobId>(protoEvent->job_id());
                        auto controllerEpoch = protoEvent->controller_epoch();
                        auto error = FromProto<TError>(protoEvent->error());
                        auto interruptReason = static_cast<EInterruptReason>(protoEvent->interrupt_reason());

                        auto expectedControllerEpoch = nodeShard->GetJobControllerEpoch(jobId);

                        // NB(gritukan, ignat): If job is released, either it is stored into operation snapshot
                        // or operation is completed. In both cases controller epoch actually is not important.
                        bool shouldValidateEpoch = eventType != EAgentToSchedulerJobEventType::Released;

                        if (shouldValidateEpoch && (controllerEpoch != expectedControllerEpoch)) {
                            YT_LOG_DEBUG("Received job event with unexpected controller epoch; ignored "
                                "(JobId: %v, EventType: %v, ControllerEpoch: %v, ExpectedControllerEpoch: %v)",
                                jobId,
                                eventType,
                                controllerEpoch,
                                expectedControllerEpoch);
                            continue;
                        }

                        switch (eventType) {
                            case EAgentToSchedulerJobEventType::Interrupted:
                                nodeShard->InterruptJob(jobId, interruptReason);
                                break;
                            case EAgentToSchedulerJobEventType::Aborted:
                                nodeShard->AbortJob(jobId, error);
                                break;
                            case EAgentToSchedulerJobEventType::Failed:
                                nodeShard->FailJob(jobId);
                                break;
                            case EAgentToSchedulerJobEventType::Released:
                                nodeShard->ReleaseJob(jobId, FromProto<TReleaseJobFlags>(protoEvent->release_job_flags()));
                                break;
                            default:
                                YT_ABORT();
                        }
                    }

                    for (const auto* protoResponse : protoResponses) {
                        auto operationId = FromProto<TOperationId>(protoResponse->operation_id());
                        auto controllerEpoch = protoResponse->controller_epoch();
                        auto expectedControllerEpoch = nodeShard->GetOperationControllerEpoch(operationId);
                        if (controllerEpoch != expectedControllerEpoch) {
                            YT_LOG_DEBUG("Received job schedule result with unexpected controller epoch; ignored "
                                "(OperationId: %v, JobId: %v, ControllerEpoch: %v, ExpectedControllerEpoch: %v)",
                                operationId,
                                FromProto<TJobId>(protoResponse->job_id()),
                                controllerEpoch,
                                expectedControllerEpoch);
                            continue;
                        }
                        nodeShard->EndScheduleJob(*protoResponse);
                    }
                }));
        }

        response->set_operation_archive_version(Bootstrap_->GetScheduler()->GetOperationArchiveVersion());
        response->set_enable_job_reporter(Bootstrap_->GetScheduler()->IsJobReporterEnabled());

        context->Reply();
    }

private:
    TSchedulerConfigPtr SchedulerConfig_;
    TControllerAgentTrackerConfigPtr Config_;
    TBootstrap* const Bootstrap_;

    const TActionQueuePtr MessageOffloadQueue_ = New<TActionQueue>("MessageOffload");

    THashMap<TAgentId, TControllerAgentPtr> IdToAgent_;

    THashSet<TString> TagsWithTooFewAgents_;
    bool AgentTagsFetched_{};

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);


    template <class F>
    void RunInMessageOffloadThread(F func)
    {
        Y_UNUSED(WaitFor(BIND(func)
            .AsyncVia(MessageOffloadQueue_->GetInvoker())
            .Run()));
    }


    void RegisterAgent(const TControllerAgentPtr& agent)
    {
        YT_VERIFY(IdToAgent_.emplace(agent->GetId(), agent).second);
    }

    void UnregisterAgent(const TControllerAgentPtr& agent)
    {
        if (agent->GetState() == EControllerAgentState::Unregistering ||
            agent->GetState() == EControllerAgentState::Unregistered)
        {
            return;
        }
        
        YT_LOG_INFO("Notify operations that agent is going to unregister (AgentId: %v, IncarnationId: %v)",
            agent->GetId(),
            agent->GetIncarnationId());

        YT_VERIFY(agent->GetState() == EControllerAgentState::Registered || agent->GetState() == EControllerAgentState::WaitingForInitialHeartbeat);

        const auto& scheduler = Bootstrap_->GetScheduler();
        for (const auto& operation : agent->Operations()) {
            scheduler->OnOperationAgentUnregistered(operation);
        }

        TerminateAgent(agent);

        YT_LOG_INFO("Aborting agent incarnation transaction (AgentId: %v, IncarnationId: %v)",
            agent->GetId(),
            agent->GetIncarnationId());

        agent->SetState(EControllerAgentState::Unregistering);
        agent->GetIncarnationTransaction()->Abort()
            .Subscribe(BIND([=, this_ = MakeStrong(this)] (const TError& error) {
                VERIFY_THREAD_AFFINITY(ControlThread);

                if (!error.IsOK()) {
                    Bootstrap_->GetScheduler()->Disconnect(error);
                    return;
                }

                if (agent->GetState() != EControllerAgentState::Unregistering) {
                    return;
                }

                YT_LOG_INFO("Agent unregistered (AgentId: %v, IncarnationId: %v)",
                    agent->GetId(),
                    agent->GetIncarnationId());

                agent->SetState(EControllerAgentState::Unregistered);
                YT_VERIFY(IdToAgent_.erase(agent->GetId()) == 1);
            })
            .Via(GetCancelableControlInvoker()));
    }

    void TerminateAgent(const TControllerAgentPtr& agent)
    {
        TLeaseManager::CloseLease(agent->GetLease());
        agent->SetLease(TLease());

        TError error("Agent disconnected");
        agent->GetChannel()->Terminate(error);
        agent->Cancel(error);
    }

    void OnAgentHeartbeatTimeout(const TWeakPtr<TControllerAgent>& weakAgent)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto agent = weakAgent.Lock();
        if (!agent) {
            return;
        }

        YT_LOG_WARNING("Agent heartbeat timeout; unregistering (AgentId: %v, IncarnationId: %v)",
            agent->GetId(),
            agent->GetIncarnationId());

        UnregisterAgent(agent);
    }

    void OnAgentIncarnationTransactionAborted(const TWeakPtr<TControllerAgent>& weakAgent)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto agent = weakAgent.Lock();
        if (!agent) {
            return;
        }

        YT_LOG_WARNING("Agent incarnation transaction aborted; unregistering (AgentId: %v, IncarnationId: %v)",
            agent->GetId(),
            agent->GetIncarnationId());

        UnregisterAgent(agent);
    }

    void RequestControllerAgentInstances(const NObjectClient::TObjectServiceProxy::TReqExecuteBatchPtr& batchReq) const
    {
        YT_LOG_INFO("Requesting controller agents list");

        auto req = TYPathProxy::Get("//sys/controller_agents/instances");
        req->mutable_attributes()->add_keys("tags");
        batchReq->AddRequest(req, "get_agent_list");
    }

    void HandleControllerAgentInstances(const NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr& batchRsp) 
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_agent_list");
        if (!rspOrError.IsOK()) {
            THROW_ERROR_EXCEPTION(rspOrError.Wrap(EErrorCode::WatcherHandlerFailed, "Error getting controller agent list"));
        }

        const auto& rsp = rspOrError.Value();

        auto tagToAgentIds = [&] {
            THashMap<TString, std::vector<TString>> tagToAgentIds;

            auto children = ConvertToNode(TYsonString(rsp->value()))->AsMap()->GetChildren();
            for (auto& [agentId, node] : children) {
                const auto tags = [&node{node}, &agentId{agentId}] () -> THashSet<TString> {
                    try {
                        const auto children = node->Attributes().ToMap()->GetChildOrThrow("tags")->AsList()->GetChildren();
                        THashSet<TString> tags;
                        tags.reserve(std::size(children));

                        for (const auto& tagNode : children) {
                            tags.insert(tagNode->AsString()->GetValue());
                        }
                        return tags;
                    } catch (const std::exception& ex) {
                        YT_LOG_WARNING(ex, "Cannot parse tags of agent %v", agentId);
                        return {};
                    }
                }();

                tagToAgentIds.reserve(std::size(tags));
                for (auto& tag : tags) {
                    tagToAgentIds[std::move(tag)].push_back(std::move(agentId));
                }
            }

            return tagToAgentIds;
        }();

        std::vector<TError> errors;
        THashSet<TString> tagsWithTooFewAgents;
        for (const auto& [tag, thresholds] : Config_->TagToAliveControllerAgentThresholds) {
            std::vector<TStringBuf> aliveAgentWithCurrentTag;
            aliveAgentWithCurrentTag.reserve(32);

            for (const auto& [agentId, agent] : IdToAgent_) {
                if (agent->GetTags().contains(tag)) {
                    aliveAgentWithCurrentTag.push_back(agentId);
                }
            }

            const auto agentsWithTag = std::move(tagToAgentIds[tag]);
            const auto agentWithTagCount = std::ssize(agentsWithTag);
            const auto aliveAgentWithTagCount = std::ssize(aliveAgentWithCurrentTag);
            if (aliveAgentWithTagCount < thresholds.Absolute ||
                (agentWithTagCount &&
                    1.0 * aliveAgentWithTagCount / agentWithTagCount < thresholds.Relative)) {

                tagsWithTooFewAgents.insert(tag);
                errors.push_back(
                    TError{"Too few agents matching tag"}
                        << TErrorAttribute{"controller_agent_tag", tag}
                        << TErrorAttribute{"alive_agents", aliveAgentWithCurrentTag}
                        << TErrorAttribute{"agents", agentsWithTag}
                        << TErrorAttribute{"min_alived_agent_count", thresholds.Absolute}
                        << TErrorAttribute{"min_alive_agent_ratio", thresholds.Relative});
            }
        }

        TagsWithTooFewAgents_ = std::move(tagsWithTooFewAgents);
        AgentTagsFetched_ = true;

        TError error;
        if (!errors.empty()) {
            error = TError{EErrorCode::WatcherHandlerFailed, "Too few matching agents"} << std::move(errors);
            YT_LOG_WARNING(error);
        }
        Bootstrap_->GetScheduler()->GetMasterConnector()->SetSchedulerAlert(
            ESchedulerAlertType::TooFewControllerAgentsAlive, error);
    }


    void DoCleanup()
    {
        for (const auto& [agentId, agent] : IdToAgent_) {
            TerminateAgent(agent);
            agent->SetState(EControllerAgentState::Unregistered);
        }
        IdToAgent_.clear();
    }

    void OnMasterConnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DoCleanup();
    }

    void OnMasterDisconnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DoCleanup();
    }

    const IInvokerPtr& GetCancelableControlInvoker()
    {
        return Bootstrap_
            ->GetScheduler()
            ->GetMasterConnector()
            ->GetCancelableControlInvoker(EControlQueue::AgentTracker);
    }
};

////////////////////////////////////////////////////////////////////////////////

TControllerAgentTracker::TControllerAgentTracker(
    TSchedulerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(std::move(config), bootstrap))
{ }

TControllerAgentTracker::~TControllerAgentTracker() = default;

void TControllerAgentTracker::Initialize()
{
    Impl_->Initialize();
}

std::vector<TControllerAgentPtr> TControllerAgentTracker::GetAgents() const
{
    return Impl_->GetAgents();
}

IOperationControllerPtr TControllerAgentTracker::CreateController(const TOperationPtr& operation)
{
    return Impl_->CreateController(operation);
}

TControllerAgentPtr TControllerAgentTracker::PickAgentForOperation(const TOperationPtr& operation)
{
    return Impl_->PickAgentForOperation(operation);
}

void TControllerAgentTracker::AssignOperationToAgent(
    const TOperationPtr& operation,
    const TControllerAgentPtr& agent)
{
    Impl_->AssignOperationToAgent(operation, agent);
}

void TControllerAgentTracker::HandleAgentFailure(
    const TControllerAgentPtr& agent,
    const TError& error)
{
    Impl_->HandleAgentFailure(agent, error);
}

void TControllerAgentTracker::UnregisterOperationFromAgent(const TOperationPtr& operation)
{
    Impl_->UnregisterOperationFromAgent(operation);
}

void TControllerAgentTracker::UpdateConfig(TSchedulerConfigPtr config)
{
    Impl_->UpdateConfig(std::move(config));
}

void TControllerAgentTracker::ProcessAgentHeartbeat(const TCtxAgentHeartbeatPtr& context)
{
    Impl_->ProcessAgentHeartbeat(context);
}

void TControllerAgentTracker::ProcessAgentHandshake(const TCtxAgentHandshakePtr& context)
{
    Impl_->ProcessAgentHandshake(context);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
