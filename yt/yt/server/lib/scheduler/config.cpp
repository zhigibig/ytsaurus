#include "config.h"

#include "experiments.h"
#include "yt/yt/server/lib/scheduler/public.h"

#include <yt/yt/ytlib/scheduler/config.h>

#include <yt/yt/server/lib/node_tracker_server/name_helpers.h>
#include <yt/yt/server/scheduler/private.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

TJobResourcesConfigPtr GetDefaultMinSpareJobResourcesOnNode()
{
    auto config = New<TJobResourcesConfig>();
    config->UserSlots = 1;
    config->Cpu = 1;
    config->Memory = 256_MB;
    return config;
}

////////////////////////////////////////////////////////////////////////////////

void TStrategyTestingOptions::Register(TRegistrar registrar)
{
    registrar.Parameter("delay_inside_fair_share_update", &TThis::DelayInsideFairShareUpdate)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

void TFairShareStrategyControllerThrottling::Register(TRegistrar registrar)
{
    registrar.Parameter("schedule_job_start_backoff_time", &TThis::ScheduleJobStartBackoffTime)
        .Default(TDuration::MilliSeconds(100));
    registrar.Parameter("schedule_job_max_backoff_time", &TThis::ScheduleJobMaxBackoffTime)
        .Default(TDuration::Seconds(10));
    registrar.Parameter("schedule_job_backoff_multiplier", &TThis::ScheduleJobBackoffMultiplier)
        .Default(1.1);
}

////////////////////////////////////////////////////////////////////////////////

void TFairShareStrategyOperationControllerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("max_concurrent_controller_schedule_job_calls", &TThis::MaxConcurrentControllerScheduleJobCalls)
        .Default(100)
        .GreaterThan(0);

    registrar.Parameter("concurrent_controller_schedule_job_calls_regularization", &TThis::ConcurrentControllerScheduleJobCallsRegularization)
        .Default(2.0)
        .GreaterThanOrEqual(1.0);

    registrar.Parameter("schedule_job_time_limit", &TThis::ScheduleJobTimeLimit)
        .Default(TDuration::Seconds(30));

    registrar.Parameter("schedule_job_fail_backoff_time", &TThis::ScheduleJobFailBackoffTime)
        .Default(TDuration::MilliSeconds(100));

    registrar.Parameter("controller_throttling", &TThis::ControllerThrottling)
        .DefaultNew();

    registrar.Parameter("schedule_job_timeout_alert_reset_time", &TThis::ScheduleJobTimeoutAlertResetTime)
        .Default(TDuration::Minutes(15));

    registrar.Parameter("schedule_jobs_timeout", &TThis::ScheduleJobsTimeout)
        .Default(TDuration::Seconds(40));

    registrar.Parameter("long_schedule_job_logging_threshold", &TThis::LongScheduleJobLoggingThreshold)
        .Default(TDuration::Seconds(10));
}

////////////////////////////////////////////////////////////////////////////////

void TFairShareStrategySchedulingSegmentsConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("mode", &TThis::Mode)
        .Default(ESegmentedSchedulingMode::Disabled);

    registrar.Parameter("satisfaction_margins", &TThis::SatisfactionMargins)
        .Default();

    registrar.Parameter("unsatisfied_segments_rebalancing_timeout", &TThis::UnsatisfiedSegmentsRebalancingTimeout)
        .Default(TDuration::Minutes(5));

    registrar.Parameter("data_center_reconsideration_timeout", &TThis::DataCenterReconsiderationTimeout)
        .Default(TDuration::Minutes(20));

    registrar.Parameter("data_centers", &TThis::DataCenters)
        .Default();

    registrar.Parameter("data_center_assignment_heuristic", &TThis::DataCenterAssignmentHeuristic)
        .Default(ESchedulingSegmentDataCenterAssignmentHeuristic::MaxRemainingCapacity);

    registrar.Postprocessor([&] (TFairShareStrategySchedulingSegmentsConfig* config) {
        for (const auto& dataCenter : config->DataCenters) {
            ValidateDataCenterName(dataCenter);
        }
    });

    registrar.Postprocessor([&] (TFairShareStrategySchedulingSegmentsConfig* config) {
        for (auto segment : TEnumTraits<ESchedulingSegment>::GetDomainValues()) {
            if (!IsDataCenterAwareSchedulingSegment(segment)) {
                continue;
            }

            for (const auto& dataCenter : config->SatisfactionMargins.At(segment).GetDataCenters()) {
                if (!dataCenter) {
                    // This could never happen but I'm afraid to put YT_VERIFY here.
                    THROW_ERROR_EXCEPTION("Satisfaction margin can be specified only for non-null data centers");
                }

                if (config->DataCenters.find(*dataCenter) == config->DataCenters.end()) {
                    THROW_ERROR_EXCEPTION("Satisfaction margin can be specified only for configured data centers")
                        << TErrorAttribute("configured_data_centers", config->DataCenters)
                        << TErrorAttribute("specified_data_center", dataCenter);
                }
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TFairShareStrategyTreeConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("nodes_filter", &TThis::NodesFilter)
        .Default();

    registrar.Parameter("fair_share_starvation_timeout", &TThis::FairShareStarvationTimeout)
        .Alias("fair_share_preemption_timeout")
        .Default(TDuration::Seconds(30));
    registrar.Parameter("fair_share_aggressive_starvation_timeout", &TThis::FairShareAggressiveStarvationTimeout)
        .Default(TDuration::Seconds(120));
    registrar.Parameter("fair_share_starvation_tolerance", &TThis::FairShareStarvationTolerance)
        .InRange(0.0, 1.0)
        .Default(0.8);

    registrar.Parameter("enable_aggressive_starvation", &TThis::EnableAggressiveStarvation)
        .Default(false);

    registrar.Parameter("max_unpreemptable_running_job_count", &TThis::MaxUnpreemptableRunningJobCount)
        .Default(10);

    registrar.Parameter("max_running_operation_count", &TThis::MaxRunningOperationCount)
        .Default(200)
        .GreaterThan(0);

    registrar.Parameter("max_running_operation_count_per_pool", &TThis::MaxRunningOperationCountPerPool)
        .Default(50)
        .GreaterThan(0);

    registrar.Parameter("max_operation_count_per_pool", &TThis::MaxOperationCountPerPool)
        .Default(50)
        .GreaterThan(0);

    registrar.Parameter("max_operation_count", &TThis::MaxOperationCount)
        .Default(50000)
        .GreaterThan(0);

    registrar.Parameter("enable_pool_starvation", &TThis::EnablePoolStarvation)
        .Default(true);

    registrar.Parameter("default_parent_pool", &TThis::DefaultParentPool)
        .Default(RootPoolName);

    registrar.Parameter("forbid_immediate_operations_in_root", &TThis::ForbidImmediateOperationsInRoot)
        .Default(true);

    registrar.Parameter("job_count_preemption_timeout_coefficient", &TThis::JobCountPreemptionTimeoutCoefficient)
        .Default(1.0)
        .GreaterThanOrEqual(1.0);

    registrar.Parameter("preemption_satisfaction_threshold", &TThis::PreemptionSatisfactionThreshold)
        .Default(1.0)
        .GreaterThan(0);

    registrar.Parameter("aggressive_preemption_satisfaction_threshold", &TThis::AggressivePreemptionSatisfactionThreshold)
        .Default(0.2)
        .GreaterThanOrEqual(0);

    registrar.Parameter("enable_scheduling_tags", &TThis::EnableSchedulingTags)
        .Default(true);

    registrar.Parameter("heartbeat_tree_scheduling_info_log_period", &TThis::HeartbeatTreeSchedulingInfoLogBackoff)
        .Default(TDuration::MilliSeconds(100));

    registrar.Parameter("max_ephemeral_pools_per_user", &TThis::MaxEphemeralPoolsPerUser)
        .GreaterThanOrEqual(1)
        .Default(1);

    registrar.Parameter("update_preemptable_list_duration_logging_threshold", &TThis::UpdatePreemptableListDurationLoggingThreshold)
        .Default(TDuration::MilliSeconds(100));

    registrar.Parameter("enable_operations_profiling", &TThis::EnableOperationsProfiling)
        .Default(true);

    registrar.Parameter("custom_profiling_tag_filter", &TThis::CustomProfilingTagFilter)
        .Default();

    registrar.Parameter("total_resource_limits_consider_delay", &TThis::TotalResourceLimitsConsiderDelay)
        .Default(TDuration::Seconds(60));

    registrar.Parameter("preemptive_scheduling_backoff", &TThis::PreemptiveSchedulingBackoff)
        .Default(TDuration::Seconds(5));

    registrar.Parameter("tentative_tree_saturation_deactivation_period", &TThis::TentativeTreeSaturationDeactivationPeriod)
        .Default(TDuration::Seconds(10));

    registrar.Parameter("infer_weight_from_guarantees_share_multiplier", &TThis::InferWeightFromGuaranteesShareMultiplier)
        .Alias("infer_weight_from_strong_guarantee_share_multiplier")
        .Alias("infer_weight_from_min_share_ratio_multiplier")
        .Default()
        .GreaterThanOrEqual(1.0);

    registrar.Parameter("packing", &TThis::Packing)
        .DefaultNew();

    registrar.Parameter("non_tentative_operation_types", &TThis::NonTentativeOperationTypes)
        .Default(std::nullopt);

    registrar.Parameter("best_allocation_ratio_update_period", &TThis::BestAllocationRatioUpdatePeriod)
        .Default(TDuration::Minutes(1));

    registrar.Parameter("enable_by_user_profiling", &TThis::EnableByUserProfiling)
        .Default(true);

    registrar.Parameter("integral_guarantees", &TThis::IntegralGuarantees)
        .DefaultNew();

    registrar.Parameter("enable_resource_tree_structure_lock_profiling", &TThis::EnableResourceTreeStructureLockProfiling)
        .Default(true);

    registrar.Parameter("enable_resource_tree_usage_lock_profiling", &TThis::EnableResourceTreeUsageLockProfiling)
        .Default(true);

    registrar.Parameter("preemption_check_starvation", &TThis::PreemptionCheckStarvation)
        .Default(true);

    registrar.Parameter("preemption_check_satisfaction", &TThis::PreemptionCheckSatisfaction)
        .Default(true);

    registrar.Parameter("job_interrupt_timeout", &TThis::JobInterruptTimeout)
        .Default(TDuration::Seconds(10));

    registrar.Parameter("job_graceful_interrupt_timeout", &TThis::JobGracefulInterruptTimeout)
        .Default(TDuration::Seconds(60));

    registrar.Parameter("scheduling_segments", &TThis::SchedulingSegments)
        .DefaultNew();

    registrar.Parameter("enable_pools_vector_profiling", &TThis::EnablePoolsVectorProfiling)
        .Default(true);

    registrar.Parameter("enable_operations_vector_profiling", &TThis::EnableOperationsVectorProfiling)
        .Default(false);

    registrar.Parameter("sparsify_fair_share_profiling", &TThis::SparsifyFairShareProfiling)
        .Default(false);

    registrar.Parameter("enable_limiting_ancestor_check", &TThis::EnableLimitingAncestorCheck)
        .Default(true);

    registrar.Parameter("profiled_pool_resources", &TThis::ProfiledPoolResources)
        .Default({
            EJobResourceType::Cpu,
            EJobResourceType::Memory,
            EJobResourceType::UserSlots,
            EJobResourceType::Gpu,
            EJobResourceType::Network
        });

    registrar.Parameter("profiled_operation_resources", &TThis::ProfiledOperationResources)
        .Default({
            EJobResourceType::Cpu,
            EJobResourceType::Memory,
            EJobResourceType::UserSlots,
            EJobResourceType::Gpu,
            EJobResourceType::Network
        });

    registrar.Parameter("waiting_job_timeout", &TThis::WaitingJobTimeout)
        .Default();

    registrar.Parameter("min_child_heap_size", &TThis::MinChildHeapSize)
        .Default(16);

    registrar.Parameter("main_resource", &TThis::MainResource)
        .Default(EJobResourceType::Cpu);

    registrar.Parameter("metering_tags", &TThis::MeteringTags)
        .Default();

    registrar.Parameter("pool_config_presets", &TThis::PoolConfigPresets)
        .Default();

    registrar.Parameter("enable_fair_share_truncation_in_fifo_pool", &TThis::EnableFairShareTruncationInFifoPool)
        .Alias("truncate_fifo_pool_unsatisfied_child_fair_share")
        .Default(false);

    registrar.Parameter("enable_conditional_preemption", &TThis::EnableConditionalPreemption)
        .Default(false);

    registrar.Parameter("use_resource_usage_with_precommit", &TThis::UseResourceUsageWithPrecommit)
        .Default(false);

    registrar.Parameter("allowed_resource_usage_staleness", &TThis::AllowedResourceUsageStaleness)
        .Default(TDuration::Seconds(5));

    registrar.Parameter("cached_job_preemption_statuses_update_period", &TThis::CachedJobPreemptionStatusesUpdatePeriod)
        .Default(TDuration::Seconds(15));

    registrar.Parameter("should_distribute_free_volume_among_children", &TThis::ShouldDistributeFreeVolumeAmongChildren)
        // TODO(renadeen): temporarily disabled.
        .Default(false);

    registrar.Parameter("use_user_default_parent_pool_map", &TThis::UseUserDefaultParentPoolMap)
        .Default(false);

    registrar.Parameter("enable_resource_usage_snapshot", &TThis::EnableResourceUsageSnapshot)
        .Default(false);

    registrar.Parameter("max_event_log_operation_batch_size", &TThis::MaxEventLogOperationBatchSize)
        .Default(1000);

    registrar.Postprocessor([&] (TFairShareStrategyTreeConfig* config) {
        if (config->AggressivePreemptionSatisfactionThreshold > config->PreemptionSatisfactionThreshold) {
            THROW_ERROR_EXCEPTION("Aggressive starvation satisfaction threshold must be less than starvation satisfaction threshold")
                << TErrorAttribute("aggressive_threshold", config->AggressivePreemptionSatisfactionThreshold)
                << TErrorAttribute("threshold", config->PreemptionSatisfactionThreshold);
        }
        if (config->FairShareAggressiveStarvationTimeout < config->FairShareStarvationTimeout) {
            THROW_ERROR_EXCEPTION("Aggressive starvation timeout must be greater than starvation timeout")
                << TErrorAttribute("aggressive_timeout", config->FairShareAggressiveStarvationTimeout)
                << TErrorAttribute("timeout", config->FairShareStarvationTimeout);
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TPoolTreesTemplateConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("priority", &TThis::Priority);

    registrar.Parameter("filter", &TThis::Filter);

    registrar.Parameter("config", &TThis::Config);
}

////////////////////////////////////////////////////////////////////////////////

void TFairShareStrategyConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("fair_share_update_period", &TThis::FairShareUpdatePeriod)
        .InRange(TDuration::MilliSeconds(10), TDuration::Seconds(60))
        .Default(TDuration::MilliSeconds(1000));

    registrar.Parameter("fair_share_profiling_period", &TThis::FairShareProfilingPeriod)
        .InRange(TDuration::MilliSeconds(10), TDuration::Seconds(60))
        .Default(TDuration::MilliSeconds(5000));

    registrar.Parameter("fair_share_log_period", &TThis::FairShareLogPeriod)
        .InRange(TDuration::MilliSeconds(10), TDuration::Seconds(60))
        .Default(TDuration::MilliSeconds(1000));

    registrar.Parameter("min_needed_resources_update_period", &TThis::MinNeededResourcesUpdatePeriod)
        .Default(TDuration::Seconds(3));

    registrar.Parameter("resource_metering_period", &TThis::ResourceMeteringPeriod)
        .Default(TDuration::Minutes(1));

    registrar.Parameter("resource_usage_snapshot_update_period", &TThis::ResourceUsageSnapshotUpdatePeriod)
        .Default(TDuration::MilliSeconds(20));

    registrar.Parameter("operation_hangup_check_period", &TThis::OperationHangupCheckPeriod)
        .Alias("operation_unschedulable_check_period")
        .Default(TDuration::Minutes(1));

    registrar.Parameter("operation_hangup_safe_timeout", &TThis::OperationHangupSafeTimeout)
        .Alias("operation_unschedulable_safe_timeout")
        .Default(TDuration::Minutes(60));

    registrar.Parameter("operation_hangup_min_schedule_job_attempts", &TThis::OperationHangupMinScheduleJobAttempts)
        .Alias("operation_unschedulable_min_schedule_job_attempts")
        .Default(1000);

    registrar.Parameter("operation_hangup_deactivation_reasons", &TThis::OperationHangupDeactivationReasons)
        .Alias("operation_unschedulable_deactivation_reasons")
        .Default({EDeactivationReason::ScheduleJobFailed, EDeactivationReason::MinNeededResourcesUnsatisfied});

    registrar.Parameter("operation_hangup_due_to_limiting_ancestor_safe_timeout", &TThis::OperationHangupDueToLimitingAncestorSafeTimeout)
        .Alias("operation_unschedulable_due_to_limiting_ancestor_safe_timeout")
        .Default(TDuration::Minutes(5));

    registrar.Parameter("max_operation_count", &TThis::MaxOperationCount)
        .Default(5000)
        .GreaterThan(0)
        // This value corresponds to the maximum possible number of memory tags.
        // It should be changed simultaneously with values of all `MaxTagValue`
        // across the code base.
        .LessThan(NYTAlloc::MaxMemoryTag);

    registrar.Parameter("operations_without_tentative_pool_trees", &TThis::OperationsWithoutTentativePoolTrees)
        .Default({EOperationType::Sort, EOperationType::MapReduce, EOperationType::RemoteCopy});

    registrar.Parameter("default_tentative_pool_trees", &TThis::DefaultTentativePoolTrees)
        .Default();

    registrar.Parameter("enable_schedule_in_single_tree", &TThis::EnableScheduleInSingleTree)
        .Default(true);

    registrar.Parameter("strategy_testing_options", &TThis::StrategyTestingOptions)
        .DefaultNew();

    registrar.Parameter("template_pool_tree_config_map", &TThis::TemplatePoolTreeConfigMap)
        .Default();

    registrar.Postprocessor([&] (TFairShareStrategyConfig* config) {
        THashMap<int, TStringBuf> priorityToName;
        priorityToName.reserve(std::size(config->TemplatePoolTreeConfigMap));

        for (const auto& [name, value] : config->TemplatePoolTreeConfigMap) {
            if (const auto [it, inserted] = priorityToName.try_emplace(value->Priority, name); !inserted) {
                THROW_ERROR_EXCEPTION("\"template_pool_tree_config_map\" has equal priority for templates")
                    << TErrorAttribute("template_names", std::array{it->second, TStringBuf{name}});
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TTestingOptions::Register(TRegistrar registrar)
{
    registrar.Parameter("enable_random_master_disconnection", &TThis::EnableRandomMasterDisconnection)
        .Default(false);
    registrar.Parameter("random_master_disconnection_max_backoff", &TThis::RandomMasterDisconnectionMaxBackoff)
        .Default(TDuration::Seconds(5));
    registrar.Parameter("master_disconnect_delay", &TThis::MasterDisconnectDelay)
        .Default();
    registrar.Parameter("handle_orphaned_operations_delay", &TThis::HandleOrphanedOperationsDelay)
        .Default();
    registrar.Parameter("finish_operation_transition_delay", &TThis::FinishOperationTransitionDelay)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

void TOperationsCleanerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("enable", &TThis::Enable)
        .Default(true);
    registrar.Parameter("enable_operation_archivation", &TThis::EnableOperationArchivation)
        .Alias("enable_archivation")
        .Default(true);
    registrar.Parameter("clean_delay", &TThis::CleanDelay)
        .Default(TDuration::Minutes(5));
    registrar.Parameter("analysis_period", &TThis::AnalysisPeriod)
        .Default(TDuration::Seconds(30));
    registrar.Parameter("remove_batch_size", &TThis::RemoveBatchSize)
        .Default(256);
    registrar.Parameter("remove_subbatch_size", &TThis::RemoveSubbatchSize)
        .Default(64);
    registrar.Parameter("remove_batch_timeout", &TThis::RemoveBatchTimeout)
        .Default(TDuration::Seconds(5));
    registrar.Parameter("archive_batch_size", &TThis::ArchiveBatchSize)
        .Default(100);
    registrar.Parameter("archive_batch_timeout", &TThis::ArchiveBatchTimeout)
        .Default(TDuration::Seconds(5));
    registrar.Parameter("max_operation_age", &TThis::MaxOperationAge)
        .Default(TDuration::Hours(6));
    registrar.Parameter("max_operation_count_per_user", &TThis::MaxOperationCountPerUser)
        .Default(200);
    registrar.Parameter("soft_retained_operation_count", &TThis::SoftRetainedOperationCount)
        .Default(200);
    registrar.Parameter("hard_retained_operation_count", &TThis::HardRetainedOperationCount)
        .Default(4000);
    registrar.Parameter("min_archivation_retry_sleep_delay", &TThis::MinArchivationRetrySleepDelay)
        .Default(TDuration::Seconds(3));
    registrar.Parameter("max_archivation_retry_sleep_delay", &TThis::MaxArchivationRetrySleepDelay)
        .Default(TDuration::Minutes(1));
    registrar.Parameter("max_operation_count_enqueued_for_archival", &TThis::MaxOperationCountEnqueuedForArchival)
        .Default(20000);
    registrar.Parameter("archivation_enable_delay", &TThis::ArchivationEnableDelay)
        .Default(TDuration::Minutes(30));
    registrar.Parameter("max_removal_sleep_delay", &TThis::MaxRemovalSleepDelay)
        .Default(TDuration::Seconds(5));
    registrar.Parameter("min_operation_count_enqueued_for_alert", &TThis::MinOperationCountEnqueuedForAlert)
        .Default(500);
    registrar.Parameter("finished_operations_archive_lookup_timeout", &TThis::FinishedOperationsArchiveLookupTimeout)
        .Default(TDuration::Seconds(30));
    registrar.Parameter("parse_operation_attributes_batch_size", &TThis::ParseOperationAttributesBatchSize)
        .Default(100);
    registrar.Parameter("enable_operation_alert_event_archivation", &TThis::EnableOperationAlertEventArchivation)
        .Default(true);
    registrar.Parameter("max_enqueued_operation_alert_event_count", &TThis::MaxEnqueuedOperationAlertEventCount)
        .Default(1000)
        .GreaterThanOrEqual(0);
    registrar.Parameter("max_alert_event_count_per_operation", &TThis::MaxAlertEventCountPerOperation)
        .Default(1000)
        .GreaterThanOrEqual(0);
    registrar.Parameter("operation_alert_event_send_period", &TThis::OperationAlertEventSendPeriod)
        .Default(TDuration::Seconds(5));
    registrar.Parameter("operation_alert_sender_alert_threshold", &TThis::OperationAlertSenderAlertThreshold)
        .Default(TDuration::Minutes(5));

    registrar.Postprocessor([&] (TOperationsCleanerConfig* config) {
        if (config->MaxArchivationRetrySleepDelay <= config->MinArchivationRetrySleepDelay) {
            THROW_ERROR_EXCEPTION("\"max_archivation_retry_sleep_delay\" must be greater than "
                "\"min_archivation_retry_sleep_delay\"")
                << TErrorAttribute("min_archivation_retry_sleep_delay", config->MinArchivationRetrySleepDelay)
                << TErrorAttribute("max_archivation_retry_sleep_delay", config->MaxArchivationRetrySleepDelay);
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TSchedulerIntegralGuaranteesConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("smooth_period", &TThis::SmoothPeriod)
        .Default(TDuration::Minutes(1));

    registrar.Parameter("pool_capacity_saturation_period", &TThis::PoolCapacitySaturationPeriod)
        .Default(TDuration::Days(1));

    registrar.Parameter("relaxed_share_multiplier_limit", &TThis::RelaxedShareMultiplierLimit)
        .Default(3);
}

////////////////////////////////////////////////////////////////////////////////

void Deserialize(TAliveControllerAgentThresholds& thresholds, const NYTree::INodePtr& node)
{
    const auto& mapNode = node->AsMap();

    thresholds.Absolute = mapNode->GetChildOrThrow("absolute")->AsInt64()->GetValue();
    thresholds.Relative = mapNode->GetChildOrThrow("relative")->AsDouble()->GetValue();
}

void Serialize(const TAliveControllerAgentThresholds& thresholds, NYson::IYsonConsumer* consumer)
{
    NYTree::BuildYsonFluently(consumer)
        .BeginMap()
            .Item("absolute").Value(thresholds.Absolute)
            .Item("relative").Value(thresholds.Relative)
        .EndMap();
}

void TControllerAgentTrackerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("light_rpc_timeout", &TThis::LightRpcTimeout)
        .Default(TDuration::Seconds(30));

    registrar.Parameter("heavy_rpc_timeout", &TThis::HeavyRpcTimeout)
        .Default(TDuration::Minutes(30));

    registrar.Parameter("heartbeat_timeout", &TThis::HeartbeatTimeout)
        .Default(TDuration::Seconds(15));

    registrar.Parameter("incarnation_transaction_timeout", &TThis::IncarnationTransactionTimeout)
        .Default(TDuration::Seconds(30));

    registrar.Parameter("incarnation_transaction_ping_period", &TThis::IncarnationTransactionPingPeriod)
        .Default();

    registrar.Parameter("agent_pick_strategy", &TThis::AgentPickStrategy)
        .Default(EControllerAgentPickStrategy::Random);

    registrar.Parameter("min_agent_available_memory", &TThis::MinAgentAvailableMemory)
        .Default(1_GB);

    registrar.Parameter("min_agent_available_memory_fraction", &TThis::MinAgentAvailableMemoryFraction)
        .InRange(0.0, 1.0)
        .Default(0.05);

    registrar.Parameter("memory_balanced_pick_strategy_score_power", &TThis::MemoryBalancedPickStrategyScorePower)
        .Default(1.0);

    registrar.Parameter("min_agent_count", &TThis::MinAgentCount)
        .Default(1);

    registrar.Parameter("tag_to_alive_controller_agent_thresholds", &TThis::TagToAliveControllerAgentThresholds)
        .Default();

    registrar.Parameter("max_message_job_event_count", &TThis::MaxMessageJobEventCount)
        .Default(10000)
        .GreaterThan(0);

    registrar.Postprocessor([&] (TControllerAgentTrackerConfig* config) {
        if (!config->TagToAliveControllerAgentThresholds.contains(DefaultOperationTag)) {
            config->TagToAliveControllerAgentThresholds[DefaultOperationTag] = {static_cast<i64>(config->MinAgentCount), 0.0};
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TResourceMeteringConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("enable_new_abc_format", &TThis::EnableNewAbcFormat)
        .Default(true);

    registrar.Parameter("default_abc_id", &TThis::DefaultAbcId)
        .Default(-1);

    registrar.Parameter("default_cloud_id", &TThis::DefaultCloudId)
        .Default();

    registrar.Parameter("default_folder_id", &TThis::DefaultFolderId)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

void TSchedulerConfig::Register(TRegistrar registrar)
{
    registrar.UnrecognizedStrategy(NYTree::EUnrecognizedStrategy::KeepRecursive);

    registrar.Parameter("node_shard_count", &TThis::NodeShardCount)
        .Default(4)
        .InRange(1, MaxNodeShardCount);

    registrar.Parameter("connect_retry_backoff_time", &TThis::ConnectRetryBackoffTime)
        .Default(TDuration::Seconds(15));

    registrar.Parameter("node_heartbeat_timeout", &TThis::NodeHeartbeatTimeout)
        .Default(TDuration::Seconds(60));

    registrar.Parameter("node_registration_timeout", &TThis::NodeRegistrationTimeout)
        .Default(TDuration::Seconds(600));

    registrar.Parameter("watchers_update_period", &TThis::WatchersUpdatePeriod)
        .Default(TDuration::Seconds(3));
    registrar.Parameter("nodes_attributes_update_period", &TThis::NodesAttributesUpdatePeriod)
        .Default(TDuration::Seconds(15));
    registrar.Parameter("profiling_update_period", &TThis::ProfilingUpdatePeriod)
        .Default(TDuration::Seconds(1));
    registrar.Parameter("alerts_update_period", &TThis::AlertsUpdatePeriod)
        .Default(TDuration::Seconds(1));
    registrar.Parameter("node_shard_submit_jobs_to_strategy_period", &TThis::NodeShardSubmitJobsToStrategyPeriod)
        .Default(TDuration::MilliSeconds(100));

    // NB: This setting is NOT synchronized with the Cypress while scheduler is connected to master.
    registrar.Parameter("lock_transaction_timeout", &TThis::LockTransactionTimeout)
        .Default(TDuration::Seconds(30));
    registrar.Parameter("pool_trees_lock_transaction_timeout", &TThis::PoolTreesLockTransactionTimeout)
        .Default(TDuration::Seconds(30));
    registrar.Parameter("pool_trees_lock_check_backoff", &TThis::PoolTreesLockCheckBackoff)
        .Default(TDuration::MilliSeconds(500));

    registrar.Parameter("job_prober_rpc_timeout", &TThis::JobProberRpcTimeout)
        .Default(TDuration::Seconds(300));

    registrar.Parameter("cluster_info_logging_period", &TThis::ClusterInfoLoggingPeriod)
        .Default(TDuration::Seconds(1));
    registrar.Parameter("nodes_info_logging_period", &TThis::NodesInfoLoggingPeriod)
        .Default(TDuration::Seconds(30));
    registrar.Parameter("exec_node_descriptors_update_period", &TThis::ExecNodeDescriptorsUpdatePeriod)
        .Default(TDuration::Seconds(10));
    registrar.Parameter("jobs_logging_period", &TThis::JobsLoggingPeriod)
        .Default(TDuration::Seconds(30));
    registrar.Parameter("running_jobs_update_period", &TThis::RunningJobsUpdatePeriod)
        .Default(TDuration::Seconds(10));
    registrar.Parameter("running_job_statistics_update_period", &TThis::RunningJobStatisticsUpdatePeriod)
        .Default(TDuration::Seconds(1));
    registrar.Parameter("missing_jobs_check_period", &TThis::MissingJobsCheckPeriod)
        .Default(TDuration::Seconds(10));
    registrar.Parameter("transient_operation_queue_scan_period", &TThis::TransientOperationQueueScanPeriod)
        .Default(TDuration::MilliSeconds(100));
    registrar.Parameter("pending_by_pool_operation_scan_period", &TThis::PendingByPoolOperationScanPeriod)
        .Default(TDuration::Minutes(1));

    registrar.Parameter("operation_to_agent_assignment_backoff", &TThis::OperationToAgentAssignmentBackoff)
        .Default(TDuration::Seconds(1));

    registrar.Parameter("max_started_jobs_per_heartbeat", &TThis::MaxStartedJobsPerHeartbeat)
        .Default()
        .GreaterThan(0);

    registrar.Parameter("node_shard_exec_nodes_cache_update_period", &TThis::NodeShardExecNodesCacheUpdatePeriod)
        .Default(TDuration::Seconds(10));

    registrar.Parameter("heartbeat_process_backoff", &TThis::HeartbeatProcessBackoff)
        .Default(TDuration::MilliSeconds(5000));
    registrar.Parameter("soft_concurrent_heartbeat_limit", &TThis::SoftConcurrentHeartbeatLimit)
        .Default(50)
        .GreaterThanOrEqual(1);
    registrar.Parameter("hard_concurrent_heartbeat_limit", &TThis::HardConcurrentHeartbeatLimit)
        .Default(100)
        .GreaterThanOrEqual(1);

    registrar.Parameter("static_orchid_cache_update_period", &TThis::StaticOrchidCacheUpdatePeriod)
        .Default(TDuration::Seconds(1));

    registrar.Parameter("orchid_keys_update_period", &TThis::OrchidKeysUpdatePeriod)
        .Default(TDuration::Seconds(1));

    registrar.Parameter("enable_job_reporter", &TThis::EnableJobReporter)
        .Default(true);
    registrar.Parameter("enable_job_spec_reporter", &TThis::EnableJobSpecReporter)
        .Default(true);
    registrar.Parameter("enable_job_stderr_reporter", &TThis::EnableJobStderrReporter)
        .Default(true);
    registrar.Parameter("enable_job_profile_reporter", &TThis::EnableJobProfileReporter)
        .Default(true);
    registrar.Parameter("enable_job_fail_context_reporter", &TThis::EnableJobFailContextReporter)
        .Default(true);

    registrar.Parameter("enable_unrecognized_alert", &TThis::EnableUnrecognizedAlert)
        .Default(true);

    registrar.Parameter("job_revival_abort_timeout", &TThis::JobRevivalAbortTimeout)
        .Default(TDuration::Minutes(5));

    registrar.Parameter("scheduling_tag_filter_expire_timeout", &TThis::SchedulingTagFilterExpireTimeout)
        .Default(TDuration::Seconds(10));

    registrar.Parameter("operations_cleaner", &TThis::OperationsCleaner)
        .DefaultNew();

    registrar.Parameter("operations_update_period", &TThis::OperationsUpdatePeriod)
        .Default(TDuration::Seconds(3));

    registrar.Parameter("finished_job_storing_timeout", &TThis::FinishedJobStoringTimeout)
        .Default(TDuration::Minutes(30));

    registrar.Parameter("finished_operation_job_storing_timeout", &TThis::FinishedOperationJobStoringTimeout)
        .Default(TDuration::Seconds(10));

    registrar.Parameter("operations_destroy_period", &TThis::OperationsDestroyPeriod)
        .Default(TDuration::Seconds(1));

    registrar.Parameter("testing_options", &TThis::TestingOptions)
        .DefaultNew();

    registrar.Parameter("event_log", &TThis::EventLog)
        .DefaultNew();

    registrar.Parameter("spec_template", &TThis::SpecTemplate)
        .Default();

    registrar.Parameter("controller_agent_tracker", &TThis::ControllerAgentTracker)
        .DefaultNew();

    registrar.Parameter("job_reporter_issues_check_period", &TThis::JobReporterIssuesCheckPeriod)
        .Default(TDuration::Minutes(1));

    registrar.Parameter("job_reporter_write_failures_alert_threshold", &TThis::JobReporterWriteFailuresAlertThreshold)
        .Default(1000);
    registrar.Parameter("job_reporter_queue_is_too_large_alert_threshold", &TThis::JobReporterQueueIsTooLargeAlertThreshold)
        .Default(10);

    registrar.Parameter("node_changes_count_threshold_to_update_cache", &TThis::NodeChangesCountThresholdToUpdateCache)
        .Default(5);

    registrar.Parameter("operation_transaction_ping_period", &TThis::OperationTransactionPingPeriod)
        .Default(TDuration::Seconds(30));

    registrar.Parameter("pool_change_is_allowed", &TThis::PoolChangeIsAllowed)
        .Default(true);

    registrar.Parameter("skip_operations_with_malformed_spec_during_revival", &TThis::SkipOperationsWithMalformedSpecDuringRevival)
        .Default(false);

    registrar.Parameter("max_offline_node_age", &TThis::MaxOfflineNodeAge)
        .Default(TDuration::Hours(12));

    registrar.Parameter("max_node_unseen_period_to_abort_jobs", &TThis::MaxNodeUnseenPeriodToAbortJobs)
        .Default(TDuration::Minutes(5));

    registrar.Parameter("orchid_worker_thread_count", &TThis::OrchidWorkerThreadCount)
        .Default(4)
        .GreaterThan(0);

    registrar.Parameter("fair_share_update_thread_count", &TThis::FairShareUpdateThreadCount)
        .Default(4)
        .GreaterThan(0);

    registrar.Parameter("handle_node_id_changes_strictly", &TThis::HandleNodeIdChangesStrictly)
        .Default(true);

    registrar.Parameter("allowed_node_resources_overcommit_duration", &TThis::AllowedNodeResourcesOvercommitDuration)
        .Default(TDuration::Seconds(15));

    registrar.Parameter("pool_trees_root", &TThis::PoolTreesRoot)
        .Default(PoolTreesRootCypressPath);

    registrar.Parameter("validate_node_tags_period", &TThis::ValidateNodeTagsPeriod)
        .Default(TDuration::Seconds(30));

    registrar.Parameter("enable_job_abort_on_zero_user_slots", &TThis::EnableJobAbortOnZeroUserSlots)
        .Default(true);

    registrar.Parameter("fetch_operation_attributes_subbatch_size", &TThis::FetchOperationAttributesSubbatchSize)
        .Default(1000);

    registrar.Parameter("resource_metering", &TThis::ResourceMetering)
        .DefaultNew();

    registrar.Parameter("scheduling_segments_manage_period", &TThis::SchedulingSegmentsManagePeriod)
        .Default(TDuration::Seconds(10));

    registrar.Parameter("scheduling_segments_initialization_timeout", &TThis::SchedulingSegmentsInitializationTimeout)
        .Default(TDuration::Minutes(5));

    registrar.Parameter("parse_operation_attributes_batch_size", &TThis::ParseOperationAttributesBatchSize)
        .Default(100);

    registrar.Parameter("experiments", &TThis::Experiments)
        .Default();

    registrar.Parameter("min_spare_job_resources_on_node", &TThis::MinSpareJobResourcesOnNode)
        .DefaultCtor(&GetDefaultMinSpareJobResourcesOnNode);

    registrar.Parameter("schedule_job_duration_logging_threshold", &TThis::ScheduleJobDurationLoggingThreshold)
        .Default(TDuration::MilliSeconds(500));

    registrar.Parameter("send_preemption_reason_in_node_heartbeat", &TThis::SendPreemptionReasonInNodeHeartbeat)
        .Default(true);

    registrar.Parameter("update_last_metering_log_time", &TThis::UpdateLastMeteringLogTime)
        .Default(true);

    registrar.Parameter("enable_heavy_runtime_parameters", &TThis::EnableHeavyRuntimeParameters)
        .Default(false);

    registrar.Preprocessor([&] (TSchedulerConfig* config) {
        config->EventLog->MaxRowWeight = 128_MB;
        if (!config->EventLog->Path) {
            config->EventLog->Path = "//sys/scheduler/event_log";
        }
    });

    registrar.Postprocessor([&] (TSchedulerConfig* config) {
        if (config->SoftConcurrentHeartbeatLimit > config->HardConcurrentHeartbeatLimit) {
            THROW_ERROR_EXCEPTION("\"soft_limit\" must be less than or equal to \"hard_limit\"")
                << TErrorAttribute("soft_limit", config->SoftConcurrentHeartbeatLimit)
                << TErrorAttribute("hard_limit", config->HardConcurrentHeartbeatLimit);
        }

        ValidateExperiments(config->Experiments);
    });
}

////////////////////////////////////////////////////////////////////////////////

void TSchedulerBootstrapConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cluster_connection", &TThis::ClusterConnection);
    registrar.Parameter("scheduler", &TThis::Scheduler)
        .DefaultNew();
    registrar.Parameter("response_keeper", &TThis::ResponseKeeper)
        .DefaultNew();
    registrar.Parameter("addresses", &TThis::Addresses)
        .Default();
    registrar.Parameter("cypress_annotations", &TThis::CypressAnnotations)
        .Default(NYTree::BuildYsonNodeFluently()
            .BeginMap()
            .EndMap()
            ->AsMap());

    registrar.Parameter("abort_on_unrecognized_options", &TThis::AbortOnUnrecognizedOptions)
        .Default(false);

    registrar.Preprocessor([&] (TThis* config) {
        config->ResponseKeeper->EnableWarmup = false;
    });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
