#include "config.h"

#include <yt/yt/server/master/chunk_server/config.h>

#include <yt/yt/server/master/cypress_server/config.h>

#include <yt/yt/server/master/cell_server/config.h>

#include <yt/yt/server/master/chaos_server/config.h>

#include <yt/yt/server/master/incumbent_server/config.h>

#include <yt/yt/server/master/node_tracker_server/config.h>

#include <yt/yt/server/master/object_server/config.h>

#include <yt/yt/server/master/security_server/config.h>

#include <yt/yt/server/master/tablet_server/config.h>

#include <yt/yt/server/master/transaction_server/config.h>

#include <yt/yt/server/master/journal_server/config.h>

#include <yt/yt/server/master/sequoia_server/config.h>

#include <yt/yt/server/master/scheduler_pool_server/config.h>

#include <yt/yt/server/lib/hive/config.h>

#include <yt/yt/server/lib/election/config.h>

#include <yt/yt/server/lib/timestamp_server/config.h>

#include <yt/yt/server/lib/transaction_supervisor/config.h>

#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/ytlib/election/config.h>

#include <yt/yt/ytlib/hive/config.h>

#include <yt/yt/ytlib/transaction_client/config.h>

#include <yt/yt/library/auth_server/config.h>

#include <yt/yt/library/program/config.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/transaction_client/config.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NCellMaster {

using namespace NObjectClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

void TMasterHydraManagerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("response_keeper", &TThis::ResponseKeeper)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

void TMasterConnectionConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("rpc_timeout", &TThis::RpcTimeout)
        .Default(TDuration::Seconds(30));

    registrar.Preprocessor([] (TThis* config) {
        config->RetryAttempts = 100;
        config->RetryTimeout = TDuration::Minutes(3);
    });
}

////////////////////////////////////////////////////////////////////////////////

void TDiscoveryServersConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("rpc_timeout", &TThis::RpcTimeout)
        .Default(TDuration::Seconds(30));
}

////////////////////////////////////////////////////////////////////////////////

void TMulticellManagerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("master_connection", &TThis::MasterConnection)
        .DefaultNew();
    registrar.Parameter("upstream_sync_delay", &TThis::UpstreamSyncDelay)
        .Default(TDuration::MilliSeconds(10));
}

////////////////////////////////////////////////////////////////////////////////

void TWorldInitializerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("init_retry_period", &TThis::InitRetryPeriod)
        .Default(TDuration::Seconds(3));
    registrar.Parameter("init_transaction_timeout", &TThis::InitTransactionTimeout)
        .Default(TDuration::Seconds(60));
    registrar.Parameter("update_period", &TThis::UpdatePeriod)
        .Default(TDuration::Minutes(5));
}

////////////////////////////////////////////////////////////////////////////////

void TMasterCellDescriptor::Register(TRegistrar registrar)
{
    registrar.Parameter("name", &TThis::Name)
        .Optional();
    registrar.Parameter("roles", &TThis::Roles)
        .Optional();
}

////////////////////////////////////////////////////////////////////////////////

void TDynamicMulticellManagerConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("cell_statistics_gossip_period", &TThis::CellStatisticsGossipPeriod)
        .Default(TDuration::Seconds(1));

    registrar.Parameter("cell_descriptors", &TThis::CellDescriptors)
        .Default();

    registrar.Postprocessor([] (TThis* config) {
        THashMap<TString, NObjectServer::TCellTag> nameToCellTag;
        for (auto& [cellTag, descriptor] : config->CellDescriptors) {
            if (descriptor->Roles && None(*descriptor->Roles)) {
                THROW_ERROR_EXCEPTION("Cell %v has no roles",
                    cellTag);
            }

            if (!descriptor->Name) {
                continue;
            }

            const auto& cellName = *descriptor->Name;

            NObjectClient::TCellTag cellTagCellName;
            if (TryFromString(cellName, cellTagCellName)) {
                THROW_ERROR_EXCEPTION("Invalid cell name %Qv",
                    cellName);
            }

            auto [it, inserted] = nameToCellTag.emplace(cellName, cellTag);
            if (!inserted) {
                THROW_ERROR_EXCEPTION("Duplicate cell name %Qv for cell tags %v and %v",
                    cellName,
                    cellTag,
                    it->second);
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TDynamicResponseKeeperConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("expiration_timeout", &TThis::ExpirationTimeout)
        .Default(TDuration::Minutes(5));

    registrar.Parameter("max_response_count_per_eviction_pass", &TThis::MaxResponseCountPerEvictionPass)
        .Default(50'000);

    registrar.Parameter("eviction_period", &TThis::EvictionPeriod)
        .Default(TDuration::Seconds(10));
}

////////////////////////////////////////////////////////////////////////////////

void TCellMasterConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("networks", &TThis::Networks)
        .Default(NNodeTrackerClient::DefaultNetworkPreferences);
    registrar.Parameter("primary_master", &TThis::PrimaryMaster)
        .Default();
    registrar.Parameter("secondary_masters", &TThis::SecondaryMasters)
        .Default();
    registrar.Parameter("election_manager", &TThis::ElectionManager)
        .DefaultNew();
    registrar.Parameter("changelogs", &TThis::Changelogs);
    registrar.Parameter("snapshots", &TThis::Snapshots);
    registrar.Parameter("hydra_manager", &TThis::HydraManager)
        .DefaultNew();
    registrar.Parameter("snapshot_validation", &TThis::SnapshotValidation)
        .DefaultNew();
    registrar.Parameter("cell_directory", &TThis::CellDirectory)
        .DefaultNew();
    registrar.Parameter("cell_directory_synchronizer", &TThis::CellDirectorySynchronizer)
        .DefaultNew();
    registrar.Parameter("hive_manager", &TThis::HiveManager)
        .DefaultNew();
    registrar.Parameter("node_tracker", &TThis::NodeTracker)
        .DefaultNew();
    registrar.Parameter("chunk_manager", &TThis::ChunkManager)
        .DefaultNew();
    registrar.Parameter("object_service", &TThis::ObjectService)
        .DefaultNew();
    registrar.Parameter("cypress_manager", &TThis::CypressManager)
        .DefaultNew();
    registrar.Parameter("replicated_table_tracker", &TThis::ReplicatedTableTracker)
        .DefaultNew();
    registrar.Parameter("enable_timestamp_manager", &TThis::EnableTimestampManager)
        .Default(true);
    registrar.Parameter("timestamp_manager", &TThis::TimestampManager)
        .DefaultNew();
    registrar.Parameter("timestamp_provider", &TThis::TimestampProvider);
    registrar.Parameter("discovery_server", &TThis::DiscoveryServer)
        .Default();
    registrar.Parameter("transaction_supervisor", &TThis::TransactionSupervisor)
        .DefaultNew();
    registrar.Parameter("multicell_manager", &TThis::MulticellManager)
        .DefaultNew();
    registrar.Parameter("world_initializer", &TThis::WorldInitializer)
        .DefaultNew();
    registrar.Parameter("security_manager", &TThis::SecurityManager)
        .DefaultNew();
    registrar.Parameter("enable_provision_lock", &TThis::EnableProvisionLock)
        .Default(true);
    registrar.Parameter("bus_client", &TThis::BusClient)
        .DefaultNew();
    registrar.Parameter("cypress_annotations", &TThis::CypressAnnotations)
        .Default(BuildYsonNodeFluently()
            .BeginMap()
            .EndMap()
        ->AsMap());
    registrar.Parameter("abort_on_unrecognized_options", &TThis::AbortOnUnrecognizedOptions)
        .Default(false);
    registrar.Parameter("cluster_connection", &TThis::ClusterConnection);
    registrar.Parameter("use_new_hydra", &TThis::UseNewHydra)
        .Default(false);
    registrar.Parameter("tvm_service", &TThis::TvmService)
        .DefaultNew();

    registrar.Postprocessor([] (TThis* config) {
        if (config->SecondaryMasters.size() > MaxSecondaryMasterCells) {
            THROW_ERROR_EXCEPTION("Too many secondary master cells");
        }

        auto cellId = config->PrimaryMaster->CellId;
        auto primaryCellTag = CellTagFromId(config->PrimaryMaster->CellId);
        THashSet<TCellTag> cellTags = {primaryCellTag};
        for (const auto& cellConfig : config->SecondaryMasters) {
            if (ReplaceCellTagInId(cellConfig->CellId, primaryCellTag) != cellId) {
                THROW_ERROR_EXCEPTION("Invalid cell id %v specified for secondary master in server configuration",
                    cellConfig->CellId);
            }
            auto cellTag = CellTagFromId(cellConfig->CellId);
            if (!cellTags.insert(cellTag).second) {
                THROW_ERROR_EXCEPTION("Duplicate cell tag %v in server configuration",
                    cellTag);
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TDynamicCellMasterConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("mutation_time_commit_period", &TThis::MutationTimeCommitPeriod)
        .Default(TDuration::Minutes(10));

    registrar.Parameter("alert_update_period", &TThis::AlertUpdatePeriod)
        .Default(TDuration::Seconds(30));

    registrar.Parameter("automaton_thread_bucket_weights", &TThis::AutomatonThreadBucketWeights)
        .Default();

    registrar.Parameter("expected_mutation_commit_duration", &TThis::ExpectedMutationCommitDuration)
        .Default(TDuration::Zero());

    registrar.Parameter("response_keeper", &TThis::ResponseKeeper)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

void TDynamicClusterConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("enable_safe_mode", &TThis::EnableSafeMode)
        .Default(false);
    registrar.Parameter("enable_descending_sort_order", &TThis::EnableDescendingSortOrder)
        .Default(false);
    registrar.Parameter("enable_descending_sort_order_dynamic", &TThis::EnableDescendingSortOrderDynamic)
        .Default(false);
    registrar.Parameter("enable_table_column_renaming", &TThis::EnableTableColumnRenaming)
        .Default(false);

    registrar.Parameter("chunk_manager", &TThis::ChunkManager)
        .DefaultNew();
    registrar.Parameter("cell_manager", &TThis::CellManager)
        .DefaultNew();
    registrar.Parameter("tablet_manager", &TThis::TabletManager)
        .DefaultNew();
    registrar.Parameter("chaos_manager", &TThis::ChaosManager)
        .DefaultNew();
    registrar.Parameter("node_tracker", &TThis::NodeTracker)
        .DefaultNew();
    registrar.Parameter("object_manager", &TThis::ObjectManager)
        .DefaultNew();
    registrar.Parameter("security_manager", &TThis::SecurityManager)
        .DefaultNew();
    registrar.Parameter("cypress_manager", &TThis::CypressManager)
        .DefaultNew();
    registrar.Parameter("multicell_manager", &TThis::MulticellManager)
        .DefaultNew();
    registrar.Parameter("transaction_manager", &TThis::TransactionManager)
        .DefaultNew();
    registrar.Parameter("scheduler_pool_manager", &TThis::SchedulerPoolManager)
        .DefaultNew();
    registrar.Parameter("sequoia_manager", &TThis::SequoiaManager)
        .DefaultNew();
    registrar.Parameter("cell_master", &TThis::CellMaster)
        .DefaultNew();
    registrar.Parameter("object_service", &TThis::ObjectService)
        .DefaultNew();
    registrar.Parameter("chunk_service", &TThis::ChunkService)
        .DefaultNew();
    registrar.Parameter("incumbent_manager", &TThis::IncumbentManager)
        .DefaultNew();

    registrar.Postprocessor([] (TThis* config) {
        if (config->EnableDescendingSortOrderDynamic && !config->EnableDescendingSortOrder) {
            THROW_ERROR_EXCEPTION(
                "Setting enable_descending_sort_order_dynamic requires "
                "enable_descending_sort_order to be set");
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

TMasterSnapshotValidationConfig::TMasterSnapshotValidationConfig()
{
    RegisterParameter("enable_host_name_validation", EnableHostNameValidation)
        .Default(true);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
