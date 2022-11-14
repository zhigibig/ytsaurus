#include "tablet_manager.h"

#include "backup_manager.h"
#include "balancing_helpers.h"
#include "config.h"
#include "cypress_integration.h"
#include "mount_config_storage.h"
#include "private.h"
#include "hunk_tablet.h"
#include "hunk_tablet_type_handler.h"
#include "hunk_storage_node.h"
#include "hunk_storage_node_type_handler.h"
#include "table_replica.h"
#include "table_replica_type_handler.h"
#include "tablet.h"
#include "tablet_action.h"
#include "tablet_action_type_handler.h"
#include "tablet_action_manager.h"
#include "tablet_balancer.h"
#include "tablet_cell.h"
#include "tablet_cell_bundle.h"
#include "tablet_cell_bundle_type_handler.h"
#include "tablet_cell_decommissioner.h"
#include "tablet_cell_type_handler.h"
#include "tablet_node_tracker.h"
#include "tablet_resources.h"
#include "tablet_service.h"
#include "tablet_type_handler.h"

#include <yt/yt/server/master/cell_master/config.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>
#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/server/master/cell_server/tamed_cell_manager.h>

#include <yt/yt/server/master/chunk_server/chunk_list.h>
#include <yt/yt/server/master/chunk_server/chunk_view.h>
#include <yt/yt/server/master/chunk_server/chunk_manager.h>
#include <yt/yt/server/master/chunk_server/chunk_tree_traverser.h>
#include <yt/yt/server/master/chunk_server/config.h>
#include <yt/yt/server/master/chunk_server/dynamic_store.h>
#include <yt/yt/server/master/chunk_server/helpers.h>
#include <yt/yt/server/master/chunk_server/medium.h>

#include <yt/yt/server/master/cypress_server/cypress_manager.h>

#include <yt/yt/server/lib/hive/hive_manager.h>
#include <yt/yt/server/lib/hive/helpers.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>
#include <yt/yt/server/lib/misc/profiling_helpers.h>

#include <yt/yt/server/master/node_tracker_server/node.h>
#include <yt/yt/server/master/node_tracker_server/node_tracker.h>

#include <yt/yt/server/master/object_server/object_manager.h>

#include <yt/yt/server/master/security_server/security_manager.h>
#include <yt/yt/server/master/security_server/group.h>
#include <yt/yt/server/master/security_server/subject.h>

#include <yt/yt/server/lib/hydra_common/hydra_janitor_helpers.h>

#include <yt/yt/server/master/table_server/master_table_schema.h>
#include <yt/yt/server/master/table_server/replicated_table_node.h>
#include <yt/yt/server/master/table_server/table_collocation.h>
#include <yt/yt/server/master/table_server/table_manager.h>
#include <yt/yt/server/master/table_server/table_node.h>

#include <yt/yt/server/lib/tablet_node/config.h>
#include <yt/yt/server/lib/tablet_node/proto/tablet_manager.pb.h>

#include <yt/yt/server/lib/tablet_server/replicated_table_tracker.h>
#include <yt/yt/server/lib/tablet_server/proto/tablet_manager.pb.h>

#include <yt/yt/server/lib/transaction_supervisor/helpers.h>

#include <yt/yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/chunk_client/config.h>
#include <yt/yt/ytlib/chunk_client/helpers.h>

#include <yt/yt/ytlib/election/config.h>

#include <yt/yt/ytlib/hive/cell_directory.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/yt/ytlib/table_client/helpers.h>

#include <yt/yt/client/table_client/schema.h>

#include <yt/yt/ytlib/tablet_client/backup.h>
#include <yt/yt/ytlib/tablet_client/config.h>
#include <yt/yt/ytlib/tablet_client/helpers.h>

#include <yt/yt/ytlib/transaction_client/helpers.h>

#include <yt/yt/client/chaos_client/replication_card_serialization.h>

#include <yt/yt/client/table_client/helpers.h>
#include <yt/yt/client/table_client/schema.h>

#include <yt/yt/client/tablet_client/helpers.h>

#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/misc/collection_helpers.h>
#include <yt/yt/core/misc/numeric_helpers.h>
#include <yt/yt/core/misc/string.h>
#include <yt/yt/core/misc/tls_cache.h>

#include <yt/yt/core/profiling/profile_manager.h>

#include <yt/yt/core/ypath/token.h>

#include <yt/yt/core/rpc/authentication_identity.h>

#include <yt/yt/core/ytree/tree_builder.h>

#include <algorithm>

namespace NYT::NTabletServer {

using namespace NCellMaster;
using namespace NCellServer;
using namespace NCellarClient;
using namespace NChunkClient::NProto;
using namespace NChunkClient;
using namespace NChaosClient;
using namespace NChunkServer;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NCypressServer;
using namespace NHiveClient;
using namespace NHiveServer;
using namespace NHydra;
using namespace NNodeTrackerClient::NProto;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerServer::NProto;
using namespace NNodeTrackerServer;
using namespace NObjectClient::NProto;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NProfiling;
using namespace NSecurityServer;
using namespace NTableClient::NProto;
using namespace NTableClient;
using namespace NTableServer;
using namespace NTabletClient;
using namespace NTabletNode::NProto;
using namespace NTabletNodeTrackerClient::NProto;
using namespace NTransactionClient;
using namespace NTransactionServer;
using namespace NTransactionSupervisor;
using namespace NYPath;
using namespace NYTree;
using namespace NYson;

using NNodeTrackerClient::TNodeDescriptor;
using NTabletNode::EStoreType;
using NTabletNode::TBuiltinTableMountConfigPtr;
using NTabletNode::TCustomTableMountConfigPtr;
using NTabletNode::TTableMountConfigPtr;
using NTabletNode::DynamicStoreIdPoolSize;
using NTransactionServer::TTransaction;

using NSecurityServer::ConvertToTabletResources;
using NSecurityServer::ConvertToClusterResources;

using NYT::FromProto;
using NYT::ToProto;

using TTabletResources = NTabletServer::TTabletResources;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletServerLogger;

////////////////////////////////////////////////////////////////////////////////

struct TProfilingCounters
{
    TProfilingCounters() = default;

    explicit TProfilingCounters(const TProfiler& profiler)
        : CopyChunkListIfSharedActionCount(profiler.Counter("/copy_chunk_list_if_shared/action_count"))
        , UpdateTabletStoresStoreCount(profiler.Counter("/update_tablet_stores/store_count"))
        , UpdateTabletStoreTime(profiler.TimeCounter("/update_tablet_stores/cumulative_time"))
        , CopyChunkListTime(profiler.TimeCounter("/copy_chunk_list_if_shared/cumulative_time"))
    { }

    TCounter CopyChunkListIfSharedActionCount;
    TCounter UpdateTabletStoresStoreCount;
    TTimeCounter UpdateTabletStoreTime;
    TTimeCounter CopyChunkListTime;
};

////////////////////////////////////////////////////////////////////////////////

class TTabletManager::TImpl
    : public TMasterAutomatonPart
{
public:
    explicit TImpl(NCellMaster::TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap,  NCellMaster::EAutomatonThreadQueue::TabletManager)
        , TabletService_(New<TTabletService>(Bootstrap_))
        , TabletBalancer_(New<TTabletBalancer>(Bootstrap_))
        , TabletCellDecommissioner_(New<TTabletCellDecommissioner>(Bootstrap_))
        , TabletActionManager_(New<TTabletActionManager>(Bootstrap_))
        , TabletMap_(TEntityMapTypeTraits<TTabletBase>(Bootstrap_))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(EAutomatonThreadQueue::Default), AutomatonThread);

        RegisterLoader(
            "TabletManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "TabletManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "TabletManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "TabletManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));

        auto primaryCellTag = Bootstrap_->GetMulticellManager()->GetPrimaryCellTag();
        DefaultTabletCellBundleId_ = MakeWellKnownId(EObjectType::TabletCellBundle, primaryCellTag, 0xffffffffffffffff);
        SequoiaTabletCellBundleId_ = MakeWellKnownId(EObjectType::TabletCellBundle, primaryCellTag, 0xfffffffffffffffe);

        RegisterMethod(BIND(&TImpl::HydraOnTabletMounted, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletUnmounted, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletFrozen, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletUnfrozen, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdateTableReplicaStatistics, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTableReplicaEnabled, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTableReplicaDisabled, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdateTabletTrimmedRowCount, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletLocked, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraCreateTabletAction, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraDestroyTabletActions, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraKickOrphanedTabletActions, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSetTabletCellStatistics, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdateUpstreamTabletState, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdateTabletState, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraAllocateDynamicStore, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSetTabletCellBundleResourceUsage, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdateTabletCellBundleResourceUsage, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnHunkTabletMounted, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnHunkTabletUnmounted, Unretained(this)));

        const auto& tabletNodeTracker = Bootstrap_->GetTabletNodeTracker();
        tabletNodeTracker->SubscribeHeartbeat(BIND(&TImpl::OnTabletNodeHeartbeat, MakeWeak(this)));
    }

    void Initialize()
    {
        const auto& configManager = Bootstrap_->GetConfigManager();
        configManager->SubscribeConfigChanged(BIND(&TImpl::OnDynamicConfigChanged, MakeWeak(this)));

        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(CreateTabletCellBundleTypeHandler(Bootstrap_));
        objectManager->RegisterHandler(CreateTabletCellTypeHandler(Bootstrap_));
        objectManager->RegisterHandler(CreateTabletTypeHandler(Bootstrap_));
        objectManager->RegisterHandler(CreateHunkTabletTypeHandler(Bootstrap_));
        objectManager->RegisterHandler(CreateTableReplicaTypeHandler(Bootstrap_, &TableReplicaMap_));
        objectManager->RegisterHandler(CreateTabletActionTypeHandler(Bootstrap_, &TabletActionMap_));

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->SubscribeTransactionAborted(BIND_NO_PROPAGATE(&TImpl::OnTransactionAborted, MakeWeak(this)));
        transactionManager->RegisterTransactionActionHandlers(
            MakeTransactionActionHandlerDescriptor(BIND_NO_PROPAGATE(&TImpl::HydraPrepareUpdateTabletStores, Unretained(this))),
            MakeTransactionActionHandlerDescriptor(BIND_NO_PROPAGATE(&TImpl::HydraCommitUpdateTabletStores, Unretained(this))),
            MakeTransactionActionHandlerDescriptor(BIND_NO_PROPAGATE(&TImpl::HydraAbortUpdateTabletStores, Unretained(this))));
        transactionManager->RegisterTransactionActionHandlers(
            MakeTransactionActionHandlerDescriptor(BIND_NO_PROPAGATE(&TImpl::HydraPrepareUpdateHunkTabletStores, Unretained(this))),
            MakeTransactionActionHandlerDescriptor(BIND_NO_PROPAGATE(&TImpl::HydraCommitUpdateHunkTabletStores, Unretained(this))),
            MakeTransactionActionHandlerDescriptor(BIND_NO_PROPAGATE(&TImpl::HydraAbortUpdateHunkTabletStores, Unretained(this))));

        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        cellManager->SubscribeAfterSnapshotLoaded(BIND_NO_PROPAGATE(&TImpl::OnAfterCellManagerSnapshotLoaded, MakeWeak(this)));
        cellManager->SubscribeCellBundleDestroyed(BIND_NO_PROPAGATE(&TImpl::OnTabletCellBundleDestroyed, MakeWeak(this)));
        cellManager->SubscribeCellDecommissionStarted(BIND_NO_PROPAGATE(&TImpl::OnTabletCellDecommissionStarted, MakeWeak(this)));

        TabletService_->Initialize();
    }

    IYPathServicePtr GetOrchidService()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return IYPathService::FromMethod(&TImpl::BuildOrchidYson, MakeWeak(this))
            ->Via(Bootstrap_->GetHydraFacade()->GetGuardedAutomatonInvoker(EAutomatonThreadQueue::TabletManager));
    }

    void OnTabletCellBundleDestroyed(TCellBundle* cellBundle)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (cellBundle->GetType() != EObjectType::TabletCellBundle) {
            return;
        }

        auto* tabletCellBundle = cellBundle->As<TTabletCellBundle>();

        // Unbind tablet actions associated with the bundle.
        for (auto* action : tabletCellBundle->TabletActions()) {
            action->SetTabletCellBundle(nullptr);
        }

        BundleIdToProfilingCounters_.erase(tabletCellBundle->GetId());
    }

    TTabletBase* GetTabletOrThrow(TTabletId id)
    {
        auto* tablet = FindTablet(id);
        if (!IsObjectAlive(tablet)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No tablet %v",
                id);
        }
        return tablet;
    }

    TTabletBase* CreateTablet(TTabletOwnerBase* table, EObjectType type)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());
        YT_VERIFY(IsTabletType(type));

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(type);

        std::unique_ptr<TTabletBase> tabletHolder;
        switch (type) {
            case EObjectType::Tablet:
                tabletHolder = TPoolAllocator::New<TTablet>(id);
                break;
            case EObjectType::HunkTablet:
                tabletHolder = TPoolAllocator::New<THunkTablet>(id);
                break;
            default:
                YT_ABORT();
        }

        tabletHolder->SetOwner(table);

        auto* tablet = TabletMap_.Insert(id, std::move(tabletHolder));
        objectManager->RefObject(tablet);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Tablet created (TableId: %v, TabletId: %v, Type: %lv, Account: %v)",
            table->GetId(),
            tablet->GetId(),
            type,
            table->GetAccount()->GetName());

        return tablet;
    }

    void DestroyTablet(TTabletBase* tablet)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // XXX(savrus): this is a workaround for YTINCIDENTS-42
        if (auto* cell = tablet->GetCell()) {
            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Destroying tablet with non-null tablet cell (TabletId: %v, CellId: %v)",
                tablet->GetId(),
                cell->GetId());

            // Replicated table not supported.
            if (tablet->GetType() == EObjectType::Tablet) {
                YT_VERIFY(tablet->As<TTablet>()->Replicas().empty());
            }

            if (cell->Tablets().erase(tablet) > 0) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Unbinding tablet from tablet cell since tablet is destroyed (TabletId: %v, CellId: %v)",
                    tablet->GetId(),
                    cell->GetId());
            }

            if (tablet->GetState() == ETabletState::Mounted) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Sending force unmount request to node since tablet is destroyed (TabletId: %v, CellId: %v)",
                    tablet->GetId(),
                    cell->GetId());

                TReqUnmountTablet request;
                ToProto(request.mutable_tablet_id(), tablet->GetId());
                request.set_force(true);

                const auto& hiveManager = Bootstrap_->GetHiveManager();
                auto* mailbox = hiveManager->GetMailbox(cell->GetId());
                hiveManager->PostMessage(mailbox, request);
           }
        }

        YT_VERIFY(!tablet->GetOwner());

        if (auto* action = tablet->GetAction()) {
            OnTabletActionTabletsTouched(
                action,
                THashSet<TTabletBase*>{tablet},
                TError("Tablet %v has been removed", tablet->GetId()));
        }

        if (tablet->GetType() == EObjectType::Tablet) {
            auto dynamicStoreCount = tablet->As<TTablet>()->DynamicStores().size();
            if (dynamicStoreCount > 0) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                    "Tablet has dynamic stores upon destruction "
                    "(TabletId: %v, StoreCount: %v)",
                    tablet->GetId(),
                    dynamicStoreCount);
            }
        }

        Y_UNUSED(TabletMap_.Release(tablet->GetId()).release());
    }

    TTableReplica* CreateTableReplica(
        TReplicatedTableNode* table,
        const TString& clusterName,
        const TYPath& replicaPath,
        ETableReplicaMode mode,
        bool preserveTimestamps,
        EAtomicity atomicity,
        bool enabled,
        TTimestamp startReplicationTimestamp,
        const std::optional<std::vector<i64>>& startReplicationRowIndexes)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        for (const auto* replica : GetValuesSortedByKey(table->Replicas())) {
            if (replica->GetClusterName() == clusterName &&
                replica->GetReplicaPath() == replicaPath)
            {
                THROW_ERROR_EXCEPTION(
                    NTabletClient::EErrorCode::TableReplicaAlreadyExists,
                    "Replica table %v at cluster %Qv already exists",
                    replicaPath,
                    clusterName);
            }
        }

        if (!preserveTimestamps && atomicity == EAtomicity::None) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::InvalidTabletState,
                "Cannot create replica table: incompatible atomicity and preserve_timestamps")
                << TErrorAttribute("\"atomicity\"", atomicity)
                << TErrorAttribute("\"preserve_timestamps\"", preserveTimestamps);
        }

        YT_VERIFY(!startReplicationRowIndexes || startReplicationRowIndexes->size() == table->Tablets().size());

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::TableReplica);
        auto replicaHolder = TPoolAllocator::New<TTableReplica>(id);
        replicaHolder->SetTable(table);
        replicaHolder->SetClusterName(clusterName);
        replicaHolder->SetReplicaPath(replicaPath);
        replicaHolder->SetMode(mode);
        replicaHolder->SetPreserveTimestamps(preserveTimestamps);
        replicaHolder->SetAtomicity(atomicity);
        replicaHolder->SetStartReplicationTimestamp(startReplicationTimestamp);
        auto state = enabled
            ? ETableReplicaState::Enabled
            : ETableReplicaState::Disabled;
        replicaHolder->SetState(state);

        auto* replica = TableReplicaMap_.Insert(id, std::move(replicaHolder));
        objectManager->RefObject(replica);

        YT_VERIFY(table->Replicas().insert(replica).second);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica created (TableId: %v, ReplicaId: %v, Mode: %v, StartReplicationTimestamp: %v)",
            table->GetId(),
            replica->GetId(),
            mode,
            startReplicationTimestamp);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        for (int tabletIndex = 0; tabletIndex < std::ssize(table->Tablets()); ++tabletIndex) {
            auto* tablet = table->Tablets()[tabletIndex]->As<TTablet>();
            auto pair = tablet->Replicas().emplace(replica, TTableReplicaInfo());
            YT_VERIFY(pair.second);
            auto& replicaInfo = pair.first->second;

            if (startReplicationRowIndexes) {
                replicaInfo.SetCommittedReplicationRowIndex((*startReplicationRowIndexes)[tabletIndex]);
            }

            if (!tablet->IsActive()) {
                replicaInfo.SetState(ETableReplicaState::None);
                continue;
            }

            replicaInfo.SetState(ETableReplicaState::Disabled);

            auto* cell = tablet->GetCell();
            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            TReqAddTableReplica req;
            ToProto(req.mutable_tablet_id(), tablet->GetId());
            PopulateTableReplicaDescriptor(req.mutable_replica(), replica, replicaInfo);
            hiveManager->PostMessage(mailbox, req);
        }

        ReplicaCreated_.Fire(TReplicaData{
            .TableId = table->GetId(),
            .Id = id,
            .Mode = mode,
            .Enabled = state == ETableReplicaState::Enabled,
            .ClusterName = clusterName,
            .TablePath = replicaPath,
            .TrackingEnabled = replica->GetEnableReplicatedTableTracker(),
            .ContentType = ETableReplicaContentType::Data,
        });

        return replica;
    }

    void DestroyTableReplica(TTableReplica* replica)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* table = replica->GetTable();
        if (table) {
            YT_VERIFY(table->Replicas().erase(replica) == 1);

            const auto& hiveManager = Bootstrap_->GetHiveManager();
            for (auto* tablet : table->Tablets()) {
                YT_VERIFY(tablet->As<TTablet>()->Replicas().erase(replica) == 1);

                if (!tablet->IsActive()) {
                    continue;
                }

                auto* cell = tablet->GetCell();
                auto* mailbox = hiveManager->GetMailbox(cell->GetId());
                TReqRemoveTableReplica req;
                ToProto(req.mutable_tablet_id(), tablet->GetId());
                ToProto(req.mutable_replica_id(), replica->GetId());
                hiveManager->PostMessage(mailbox, req);
            }
        }

        ReplicaDestroyed_.Fire(replica->GetId());
    }

    void AlterTableReplica(
        TTableReplica* replica,
        std::optional<bool> enabled,
        std::optional<ETableReplicaMode> mode,
        std::optional<EAtomicity> atomicity,
        std::optional<bool> preserveTimestamps,
        std::optional<bool> enableReplicatedTableTracker)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (mode && !IsStableReplicaMode(*mode)) {
            THROW_ERROR_EXCEPTION("Invalid replica mode %Qlv", *mode);
        }

        auto* table = replica->GetTable();
        auto state = replica->GetState();

        table->ValidateNotBackup("Cannot alter replica of a backup table");

        if (table->GetAggregatedTabletBackupState() != ETabletBackupState::None) {
            THROW_ERROR_EXCEPTION("Canont alter replica since its table is being backed up")
                << TErrorAttribute("table_id", table->GetId())
                << TErrorAttribute("tablet_backup_state", table->GetAggregatedTabletBackupState());
        }

        if (enabled) {
            if (*enabled) {
                switch (state) {
                    case ETableReplicaState::Enabled:
                    case ETableReplicaState::Enabling:
                        enabled = std::nullopt;
                        break;
                    case ETableReplicaState::Disabled:
                        break;
                    default:
                        replica->ThrowInvalidState();
                        break;
                }
            } else {
                switch (state) {
                    case ETableReplicaState::Disabled:
                    case ETableReplicaState::Disabling:
                        enabled = std::nullopt;
                        break;
                    case ETableReplicaState::Enabled:
                        break;
                    default:
                        replica->ThrowInvalidState();
                        break;
                }
            }

            for (auto* tablet : table->Tablets()) {
                if (tablet->GetState() == ETabletState::Unmounting) {
                    THROW_ERROR_EXCEPTION("Cannot alter \"enabled\" replica flag since tablet %v is in %Qlv state",
                        tablet->GetId(),
                        tablet->GetState());
                }
            }
        }

        if (!preserveTimestamps && atomicity == EAtomicity::None) {
            THROW_ERROR_EXCEPTION(
                NTabletClient::EErrorCode::InvalidTabletState,
                "Cannot set atomicity %v with preserveTimestamps %v",
                atomicity,
                preserveTimestamps);
        }

        if (mode && replica->GetMode() == *mode) {
            mode = std::nullopt;
        }

        if (atomicity && replica->GetAtomicity() == *atomicity) {
            atomicity = std::nullopt;
        }

        if (preserveTimestamps && replica->GetPreserveTimestamps() == *preserveTimestamps) {
            preserveTimestamps = std::nullopt;
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica updated (TableId: %v, ReplicaId: %v, Enabled: %v, Mode: %v, Atomicity: %v, PreserveTimestamps: %v, EnableReplicatedTableTracker: %v)",
            table->GetId(),
            replica->GetId(),
            enabled,
            mode,
            atomicity,
            preserveTimestamps,
            enableReplicatedTableTracker);

        if (mode) {
            replica->SetMode(*mode);
            ReplicaModeUpdated_.Fire(replica->GetId(), *mode);
        }

        if (atomicity) {
            replica->SetAtomicity(*atomicity);
        }

        if (preserveTimestamps) {
            replica->SetPreserveTimestamps(*preserveTimestamps);
        }

        if (enableReplicatedTableTracker) {
            replica->SetEnableReplicatedTableTracker(*enableReplicatedTableTracker);
        }

        if (enabled) {
            if (*enabled) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Enabling table replica (TableId: %v, ReplicaId: %v)",
                    table->GetId(),
                    replica->GetId());
                replica->SetState(ETableReplicaState::Enabling);
            } else {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Disabling table replica (TableId: %v, ReplicaId: %v)",
                    table->GetId(),
                    replica->GetId());
                replica->SetState(ETableReplicaState::Disabling);
            }
        }

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        for (auto* tabletBase : table->Tablets()) {
            if (!tabletBase->IsActive()) {
                continue;
            }

            auto* tablet = tabletBase->As<TTablet>();
            auto* replicaInfo = tablet->GetReplicaInfo(replica);

            auto* cell = tablet->GetCell();
            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            TReqAlterTableReplica req;
            ToProto(req.mutable_tablet_id(), tablet->GetId());
            ToProto(req.mutable_replica_id(), replica->GetId());

            if (enabled) {
                std::optional<ETableReplicaState> newState;
                if (*enabled && replicaInfo->GetState() != ETableReplicaState::Enabled) {
                    newState = ETableReplicaState::Enabling;
                }
                if (!*enabled && replicaInfo->GetState() != ETableReplicaState::Disabled) {
                    newState = ETableReplicaState::Disabling;
                }
                if (newState) {
                    req.set_enabled(*newState == ETableReplicaState::Enabling);
                    StartReplicaTransition(tablet, replica, replicaInfo, *newState);
                }
            }

            if (mode) {
                req.set_mode(ToProto<int>(*mode));
            }
            if (atomicity) {
                req.set_atomicity(ToProto<int>(*atomicity));
            }
            if (preserveTimestamps) {
                req.set_preserve_timestamps(*preserveTimestamps);
            }

            hiveManager->PostMessage(mailbox, req);
        }

        if (enabled) {
            CheckTransitioningReplicaTablets(replica);
        }
    }


    TTabletAction* CreateTabletAction(
        TObjectId hintId,
        ETabletActionKind kind,
        const std::vector<TTabletBase*>& tablets,
        const std::vector<TTabletCell*>& cells,
        const std::vector<NTableClient::TLegacyOwningKey>& pivotKeys,
        const std::optional<int>& tabletCount,
        bool skipFreezing,
        TGuid correlationId,
        TInstant expirationTime,
        std::optional<TDuration> expirationTimeout)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (tablets.empty()) {
            THROW_ERROR_EXCEPTION("Invalid number of tablets: expected more than zero");
        }

        if (tablets[0]->GetType() != EObjectType::Tablet) {
            THROW_ERROR_EXCEPTION("Tablet actions are not supported for tablets of type %Qlv",
                tablets[0]->GetType());
        }

        auto* table = tablets[0]->As<TTablet>()->GetTable();

        // Validate that table is not in process of mount/unmount/etc.
        table->ValidateNoCurrentMountTransaction("Cannot create tablet action");

        for (const auto* tablet : tablets) {
            if (tablet->GetOwner() != table) {
                THROW_ERROR_EXCEPTION("Tablets %v and %v belong to different tables",
                    tablets[0]->GetId(),
                    tablet->GetId());
            }
            if (auto* action = tablet->GetAction()) {
                THROW_ERROR_EXCEPTION("Tablet %v already participating in action %v",
                    tablet->GetId(),
                    action->GetId());
            }
            if (tablet->GetState() != ETabletState::Mounted && tablet->GetState() != ETabletState::Frozen) {
                THROW_ERROR_EXCEPTION("Tablet %v is in state %Qlv",
                    tablet->GetId(),
                    tablet->GetState());
            }
        }

        bool freeze;
        {
            auto state = tablets[0]->GetState();
            for (const auto* tablet : tablets) {
                if (tablet->GetState() != state) {
                    THROW_ERROR_EXCEPTION("Tablets are in mixed state");
                }
            }
            freeze = state == ETabletState::Frozen;
        }

        const auto& bundle = table->TabletCellBundle();

        for (auto* cell : cells) {
            if (!IsCellActive(cell)) {
                THROW_ERROR_EXCEPTION("Tablet cell %v is not active", cell->GetId());
            }

            if (cell->CellBundle() != bundle) {
                THROW_ERROR_EXCEPTION("%v %v and tablet cell %v belong to different bundles",
                    table->GetCapitalizedObjectName(),
                    table->GetId(),
                    cell->GetId());
            }
        }

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(bundle.Get(), EPermission::Use);

        switch (kind) {
            case ETabletActionKind::Move:
                if (!cells.empty() && cells.size() != tablets.size()) {
                    THROW_ERROR_EXCEPTION("Number of destination cells and tablets mismatch: %v tablets, %v cells",
                        cells.size());
                }
                if (!pivotKeys.empty()) {
                    THROW_ERROR_EXCEPTION("Invalid number of pivot keys: expected 0, actual %v",
                        pivotKeys.size());
                }
                if (tabletCount) {
                    THROW_ERROR_EXCEPTION("Invalid number of tablets: expected std::nullopt, actual %v",
                        *tabletCount);
                }
                break;

            case ETabletActionKind::Reshard:
                if (pivotKeys.empty() && (!tabletCount || *tabletCount < 1)) {
                    THROW_ERROR_EXCEPTION("Invalid number of new tablets: expected pivot keys or tablet count greater than 1");
                }

                if (!cells.empty()) {
                    if (pivotKeys.empty()) {
                        if (ssize(cells) != *tabletCount) {
                            THROW_ERROR_EXCEPTION("Number of destination cells and tablet count mismatch: "
                                "tablet count %v, cells %v",
                                *tabletCount,
                                cells.size());
                        }
                    } else {
                        if (ssize(cells) != ssize(pivotKeys)) {
                            THROW_ERROR_EXCEPTION("Number of destination cells and pivot keys mismatch: pivot keys %v, cells %",
                                pivotKeys.size(),
                                cells.size());

                        }
                    }
                }

                for (int index = 1; index < std::ssize(tablets); ++index) {
                    const auto& cur = tablets[index];
                    const auto& prev = tablets[index - 1];
                    if (cur->GetIndex() != prev->GetIndex() + 1) {
                        THROW_ERROR_EXCEPTION("Tablets %v and %v are not consequent",
                            prev->GetId(),
                            cur->GetId());
                    }
                }
                break;

            default:
                YT_ABORT();
        }

        auto* action = DoCreateTabletAction(
            hintId,
            kind,
            ETabletActionState::Preparing,
            tablets,
            cells,
            pivotKeys,
            tabletCount,
            freeze,
            skipFreezing,
            correlationId,
            expirationTime,
            expirationTimeout);

        OnTabletActionStateChanged(action);
        return action;
    }

    void DestroyTabletAction(TTabletAction* action)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        UnbindTabletAction(action);
        if (auto* bundle = action->GetTabletCellBundle()) {
            bundle->TabletActions().erase(action);
            if (!action->IsFinished()) {
                bundle->DecreaseActiveTabletActionCount();
            }
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet action destroyed (ActionId: %v, TabletBalancerCorrelationId: %v)",
            action->GetId(),
            action->GetCorrelationId());
    }

    void PrepareMount(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex,
        TTabletCellId hintCellId,
        const std::vector<TTabletCellId>& targetCellIds,
        bool freeze)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        table->ValidateMount();

        if (table->IsNative()) {
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            securityManager->ValidatePermission(table, EPermission::Mount);
        }

        if (table->IsExternal()) {
            return;
        }

        auto validateCellBundle = [table] (const TTabletCell* cell) {
            if (cell->CellBundle() != table->TabletCellBundle()) {
                THROW_ERROR_EXCEPTION("Cannot mount tablets into cell %v since it belongs to bundle %Qv while the %v "
                    "is configured to use bundle %Qv",
                    cell->GetId(),
                    cell->CellBundle()->GetName(),
                    table->GetLowercaseObjectName(),
                    table->TabletCellBundle()->GetName());
            }
        };

        ParseTabletRangeOrThrow(table, &firstTabletIndex, &lastTabletIndex); // may throw

        if (hintCellId || !targetCellIds.empty()) {
            if (hintCellId && !targetCellIds.empty()) {
                THROW_ERROR_EXCEPTION("At most one of \"cell_id\" and \"target_cell_ids\" must be specified");
            }

            if (hintCellId) {
                auto hintCell = GetTabletCellOrThrow(hintCellId);
                validateCellBundle(hintCell);
            } else {
                int tabletCount = lastTabletIndex - firstTabletIndex + 1;
                if (!targetCellIds.empty() && std::ssize(targetCellIds) != tabletCount) {
                    THROW_ERROR_EXCEPTION("\"target_cell_ids\" must either be empty or contain exactly "
                        "\"last_tablet_index\" - \"first_tablet_index\" + 1 entries (%v != %v - %v + 1)",
                        targetCellIds.size(),
                        lastTabletIndex,
                        firstTabletIndex);
                }

                for (auto cellId : targetCellIds) {
                    auto targetCell = GetTabletCellOrThrow(cellId);
                    if (!IsCellActive(targetCell)) {
                        THROW_ERROR_EXCEPTION("Cannot mount tablet into cell %v since it is not active",
                            cellId);
                    }
                    validateCellBundle(targetCell);
                }
            }
        } else {
            ValidateHasHealthyCells(table->TabletCellBundle().Get()); // may throw
        }

        const auto& allTablets = table->Tablets();

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = allTablets[index];
            tablet->ValidateMount(freeze);
        }

        if (IsTableType(table->GetType())) {
            PrepareMountTable(table->As<TTableNode>());
        }

        ValidateTabletStaticMemoryUpdate(
            table,
            firstTabletIndex,
            lastTabletIndex);

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "mount_table");
    }

    void PrepareMountTable(TTableNode* table)
    {
        auto tableSettings = GetTableSettings(table);
        ValidateTableMountConfig(table, tableSettings.MountConfig);

        if (table->GetReplicationCardId() && !table->IsSorted()) {
            if (table->GetCommitOrdering() != ECommitOrdering::Strong) {
                THROW_ERROR_EXCEPTION("Ordered dynamic table bound for chaos replication should have %Qlv commit ordering",
                    ECommitOrdering::Strong);
            }

            if (!table->GetSchema()->AsTableSchema()->FindColumn(TimestampColumnName)) {
                THROW_ERROR_EXCEPTION("Ordered dynamic table bound for chaos replication should have %Qlv column",
                    TimestampColumnName);
            }
        }

        if (auto backupState = table->GetAggregatedTabletBackupState();
            backupState != ETabletBackupState::None)
        {
            THROW_ERROR_EXCEPTION("Cannot mount table since it has invalid backup state %Qlv",
                backupState);
        }
    }

    void Mount(
        TTabletOwnerBase* table,
        const TString& path,
        int firstTabletIndex,
        int lastTabletIndex,
        TTabletCellId hintCellId,
        const std::vector<TTabletCellId>& targetCellIds,
        bool freeze,
        TTimestamp mountTimestamp)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (table->IsExternal()) {
            UpdateTabletState(table);
            return;
        }

        TTabletCell* hintCell = nullptr;
        if (hintCellId) {
            hintCell = FindTabletCell(hintCellId);
        }

        table->SetMountPath(path);

        const auto& allTablets = table->Tablets();

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex);

        std::vector<std::pair<TTabletBase*, TTabletCell*>> assignment;

        if (!targetCellIds.empty()) {
            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* tablet = allTablets[index];
                if (!tablet->GetCell()) {
                    auto* cell = FindTabletCell(targetCellIds[index - firstTabletIndex]);
                    assignment.emplace_back(tablet, cell);
                }
            }
        } else {
            std::vector<TTabletBase*> tabletsToMount;
            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* tablet = allTablets[index];
                if (!tablet->GetCell()) {
                    tabletsToMount.push_back(tablet);
                }
            }

            assignment = ComputeTabletAssignment(
                table,
                hintCell,
                std::move(tabletsToMount));
        }

        auto tableSettings = GetTabletOwnerSettings(table);
        auto serializedTableSettings = SerializeTabletOwnerSettings(tableSettings);

        DoMountTablets(
            table,
            serializedTableSettings,
            assignment,
            freeze,
            mountTimestamp);

        UpdateTabletState(table);
    }

    void PrepareUnmount(
        TTabletOwnerBase* table,
        bool force,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        table->ValidateUnmount();

        if (table->IsNative()) {
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            securityManager->ValidatePermission(table, EPermission::Mount);
        }

        if (table->IsExternal()) {
            return;
        }

        ParseTabletRangeOrThrow(table, &firstTabletIndex, &lastTabletIndex); // may throw

        if (!force) {
            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* tablet = table->Tablets()[index];
                tablet->ValidateUnmount();
            }
        }

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "unmount_table");
    }

    void Unmount(
        TTabletOwnerBase* table,
        bool force,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (table->IsExternal()) {
            UpdateTabletState(table);
            return;
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex);

        DoUnmount(table, force, firstTabletIndex, lastTabletIndex, /*onDestroy*/ false);
        UpdateTabletState(table);
    }

    void PrepareRemount(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        table->ValidateRemount();

        if (table->IsNative()) {
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            securityManager->ValidatePermission(table, EPermission::Mount);
        }

        if (table->IsExternal()) {
            return;
        }

        ParseTabletRangeOrThrow(table, &firstTabletIndex, &lastTabletIndex); // may throw

        if (IsTableType(table->GetType())) {
            PrepareRemountTable(table->As<TTableNode>(), firstTabletIndex, lastTabletIndex);
        }
    }

    void PrepareRemountTable(
        TTableNode* table,
        int /*firstTabletIndex*/,
        int lastTabletIndex)
    {
        if (!table->IsSorted() && table->GetReplicationCardId() && lastTabletIndex != std::ssize(table->Tablets()) - 1) {
            THROW_ERROR_EXCEPTION("Invalid last tablet index: expected %v, got %v",
                std::ssize(table->Tablets()) - 1,
                lastTabletIndex);
        }

        auto tableSettings = GetTableSettings(table);
        ValidateTableMountConfig(table, tableSettings.MountConfig);
    }

    void Remount(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (table->IsExternal()) {
            UpdateTabletState(table);
            return;
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex);

        auto resourceUsageBefore = table->GetTabletResourceUsage();

        DoRemount(table, firstTabletIndex, lastTabletIndex);

        if (resourceUsageBefore != table->GetTabletResourceUsage()) {
            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                "Tablet resource usage changed during table remount "
                "(TableId: %v, UsageBefore: %v, UsageAfter: %v)",
                table->GetId(),
                resourceUsageBefore,
                table->GetTabletResourceUsage());
        }
        UpdateResourceUsage(table, table->GetTabletResourceUsage() - resourceUsageBefore);
    }

    void PrepareFreeze(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        table->ValidateFreeze();

        if (table->IsNative()) {
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            securityManager->ValidatePermission(table, EPermission::Mount);
        }

        if (table->IsExternal()) {
            return;
        }

        ParseTabletRangeOrThrow(table, &firstTabletIndex, &lastTabletIndex); // may throw

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            tablet->ValidateFreeze();
        }

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "freeze_table");
    }

    void Freeze(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (table->IsExternal()) {
            UpdateTabletState(table);
            return;
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex);

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            DoFreezeTablet(tablet);
        }

        UpdateTabletState(table);
    }

    void PrepareUnfreeze(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        table->ValidateUnfreeze();

        if (table->IsNative()) {
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            securityManager->ValidatePermission(table, EPermission::Mount);
        }

        if (table->IsExternal()) {
            return;
        }

        ParseTabletRangeOrThrow(table, &firstTabletIndex, &lastTabletIndex); // may throw

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            tablet->ValidateUnfreeze();
        }

        // Do after all validations.
        TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "unfreeze_table");
    }

    void Unfreeze(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (table->IsExternal()) {
            UpdateTabletState(table);
            return;
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex);

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            DoUnfreezeTablet(tablet);
        }

        UpdateTabletState(table);
    }

    void PrepareReshard(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount,
        const std::vector<TLegacyOwningKey>& pivotKeys,
        bool create = false)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        table->ValidateReshard(
            Bootstrap_,
            firstTabletIndex,
            lastTabletIndex,
            newTabletCount,
            pivotKeys);

        if (!create && !table->IsForeign()) {
            const auto& securityManager = Bootstrap_->GetSecurityManager();
            securityManager->ValidatePermission(table, EPermission::Mount);
        }

        if (create) {
            int oldTabletCount = table->IsExternal() ? 0 : 1;
            ValidateResourceUsageIncrease(
                table,
                TTabletResources().SetTabletCount(newTabletCount - oldTabletCount));
        }

        if (table->IsExternal()) {
            return;
        }

        // Now check against tablets.
        // This is a job of secondary master in a two-phase commit.
        // Should not throw when table is created.

        const auto& tablets = table->Tablets();
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            if (auto* chunkList = table->GetChunkList(contentType)) {
                YT_VERIFY(tablets.size() == chunkList->Children().size());
            }
        }

        ParseTabletRangeOrThrow(table, &firstTabletIndex, &lastTabletIndex); // may throw

        int oldTabletCount = lastTabletIndex - firstTabletIndex + 1;

        if (tablets.size() - oldTabletCount + newTabletCount > MaxTabletCount) {
            THROW_ERROR_EXCEPTION("Tablet count cannot exceed the limit of %v",
                MaxTabletCount);
        }

        ValidateResourceUsageIncrease(
            table,
            TTabletResources().SetTabletCount(newTabletCount - oldTabletCount));

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = tablets[index];
            tablet->ValidateReshard();
        }

        if (newTabletCount < oldTabletCount) {
            for (int index = firstTabletIndex + newTabletCount; index < firstTabletIndex + oldTabletCount; ++index) {
                auto* tablet = tablets[index];
                tablet->ValidateReshardRemove();
            }
        }

        // Do after all validations.
        if (IsTableType(table->GetType())) {
            TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "reshard_table");
        }
    }

    void Reshard(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount,
        const std::vector<TLegacyOwningKey>& pivotKeys)
    {
        if (table->IsExternal()) {
            UpdateTabletState(table);
            return;
        }

        DoReshard(
            table,
            firstTabletIndex,
            lastTabletIndex,
            newTabletCount,
            pivotKeys);

        UpdateTabletState(table);
    }

    void DestroyTabletOwner(TTabletOwnerBase* table)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();

        if (!table->Tablets().empty()) {
            int firstTabletIndex = 0;
            int lastTabletIndex = static_cast<int>(table->Tablets().size()) - 1;

            TouchAffectedTabletActions(table, firstTabletIndex, lastTabletIndex, "remove");

            DoUnmount(
                table,
                /*force*/ true,
                firstTabletIndex,
                lastTabletIndex,
                /*onDestroy*/ true);

            for (auto* tablet : table->Tablets()) {
                tablet->SetOwner(nullptr);
                YT_VERIFY(tablet->GetState() == ETabletState::Unmounted);
                objectManager->UnrefObject(tablet);
            }

            YT_VERIFY(!table->IsExternal());

            const auto& bundle = table->TabletCellBundle();
            bundle->UpdateResourceUsage(-table->GetTabletResourceUsage());

            table->MutableTablets().clear();

            // NB: security manager has already been informed when node's account was reset.
        }

        if (table->GetType() == EObjectType::ReplicatedTable) {
            auto* replicatedTable = table->As<TReplicatedTableNode>();
            for (auto* replica : GetValuesSortedByKey(replicatedTable->Replicas())) {
                replica->SetTable(nullptr);
                replica->TransitioningTablets().clear();
                objectManager->UnrefObject(replica);
            }
            replicatedTable->Replicas().clear();

            if (!table->IsExternal()) {
                ReplicatedTableDestroyed_.Fire(table->GetId());
            }
        }

        const auto& transactionManager = Bootstrap_->GetTransactionManager();

        if (IsTableType(table->GetType())) {
            auto* typedTable = table->As<TTableNode>();
            for (auto [transactionId, lock] : typedTable->DynamicTableLocks()) {
                auto* transaction = transactionManager->FindTransaction(transactionId);
                if (!IsObjectAlive(transaction)) {
                    continue;
                }

                transaction->LockedDynamicTables().erase(typedTable);
            }

            if (auto* replicationCollocation = typedTable->GetReplicationCollocation()) {
                YT_VERIFY(table->GetType() == EObjectType::ReplicatedTable);
                const auto& tableManager = Bootstrap_->GetTableManager();
                tableManager->RemoveTableFromCollocation(typedTable, replicationCollocation);
            }
        }
    }

    void MergeTable(TTableNode* originatingNode, TTableNode* branchedNode)
    {
        YT_VERIFY(originatingNode->IsTrunk());

        auto updateMode = branchedNode->GetUpdateMode();
        if (updateMode == EUpdateMode::Append) {
            CopyChunkListsIfShared(originatingNode, 0, originatingNode->Tablets().size() - 1);
        }

        TChunkLists originatingChunkLists;
        TChunkLists branchedChunkLists;
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            originatingChunkLists[contentType] = originatingNode->GetChunkList(contentType);
            branchedChunkLists[contentType] = branchedNode->GetChunkList(contentType);
        }

        auto* transaction = branchedNode->GetTransaction();

        YT_VERIFY(originatingNode->IsPhysicallySorted());

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto& hiveManager = Bootstrap_->GetHiveManager();
        const auto& transactionManager = Bootstrap_->GetTransactionManager();

        transaction->LockedDynamicTables().erase(originatingNode);

        i64 totalMemorySizeDelta = 0;

        // Deaccumulate old tablet statistics.
        for (int index = 0; index < std::ssize(originatingNode->Tablets()); ++index) {
            auto* tablet = originatingNode->Tablets()[index];

            auto tabletStatistics = tablet->GetTabletStatistics();
            originatingNode->DiscountTabletStatistics(tabletStatistics);

            if (tablet->GetState() != ETabletState::Unmounted) {
                totalMemorySizeDelta -= tablet->GetTabletStaticMemorySize();

                auto* cell = tablet->GetCell();
                cell->GossipStatistics().Local() -= tabletStatistics;
            }
        }

        // Replace root chunk list.
        if (updateMode == EUpdateMode::Overwrite) {
            for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
                originatingChunkLists[contentType]->RemoveOwningNode(originatingNode);
                branchedChunkLists[contentType]->AddOwningNode(originatingNode);
                originatingNode->SetChunkList(contentType, branchedChunkLists[contentType]);
            }
        }

        // Merge tablet chunk lists and accumulate new tablet statistics.
        for (int index = 0; index < std::ssize(branchedChunkLists[EChunkListContentType::Main]->Children()); ++index) {
            auto* tablet = originatingNode->Tablets()[index]->As<TTablet>();
            if (updateMode == EUpdateMode::Overwrite) {
                AbandonDynamicStores(tablet);
            }

            std::vector<TChunkTree*> stores;
            for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
                if (updateMode == EUpdateMode::Append && contentType == EChunkListContentType::Hunk) {
                    continue;
                }

                auto* appendChunkList = branchedChunkLists[contentType]->Children()[index]->AsChunkList();
                auto* tabletChunkList = originatingChunkLists[contentType]->Children()[index]->AsChunkList();

                if (updateMode == EUpdateMode::Overwrite && contentType == EChunkListContentType::Main) {
                    YT_VERIFY(appendChunkList->GetKind() == EChunkListKind::SortedDynamicTablet);
                    appendChunkList->SetPivotKey(tabletChunkList->GetPivotKey());
                }

                if (updateMode == EUpdateMode::Append) {
                    if (!appendChunkList->Children().empty()) {
                        chunkManager->AttachToChunkList(tabletChunkList, appendChunkList);
                    }
                }

                if (originatingNode->GetInMemoryMode() != EInMemoryMode::None &&
                    tablet->GetState() != ETabletState::Unmounted &&
                    contentType == EChunkListContentType::Main)
                {
                    auto& nodeStatistics = tablet->NodeStatistics();
                    nodeStatistics.set_preload_pending_store_count(
                        nodeStatistics.preload_pending_store_count() +
                        ssize(appendChunkList->Children()));
                }

                if (tablet->GetState() != ETabletState::Unmounted &&
                    contentType == EChunkListContentType::Main)
                {
                    EnumerateStoresInChunkTree(appendChunkList, &stores);
                }
            }

            auto newStatistics = tablet->GetTabletStatistics();
            originatingNode->AccountTabletStatistics(newStatistics);

            if (tablet->GetState() == ETabletState::Unmounted) {
                continue;
            }

            auto newMemorySize = tablet->GetTabletStaticMemorySize();

            totalMemorySizeDelta += newMemorySize;

            if (updateMode == EUpdateMode::Overwrite) {
                tablet->SetStoresUpdatePreparedTransaction(nullptr);
            }

            auto* cell = tablet->GetCell();
            cell->GossipStatistics().Local() += newStatistics;

            TReqUnlockTablet req;
            ToProto(req.mutable_tablet_id(), tablet->GetId());
            ToProto(req.mutable_transaction_id(), transaction->GetId());
            req.set_mount_revision(tablet->GetMountRevision());
            req.set_commit_timestamp(static_cast<i64>(
                transactionManager->GetTimestampHolderTimestamp(transaction->GetId())));
            req.set_update_mode(ToProto<int>(updateMode));

            i64 startingRowIndex = 0;
            for (const auto* store : stores) {
                auto* descriptor = req.add_stores_to_add();
                FillStoreDescriptor(originatingNode, store, descriptor, &startingRowIndex);
            }

            if (updateMode == EUpdateMode::Overwrite &&
                tablet->GetState() == ETabletState::Mounted &&
                IsDynamicStoreReadEnabled(originatingNode))
            {
                CreateAndAttachDynamicStores(tablet, &req);
            }

            auto* mailbox = hiveManager->GetMailbox(tablet->GetCell()->GetId());
            hiveManager->PostMessage(mailbox, req);
        }

        // The rest of TChunkOwner::DoMerge later unconditionally replaces statistics of
        // originating node with the ones of branched node. Since dynamic stores are already
        // attached, we have to account them this way.
        branchedNode->SnapshotStatistics() = {};
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            branchedNode->SnapshotStatistics() += originatingNode->GetChunkList(contentType)->Statistics().ToDataStatistics();
        }

        auto resourceUsageDelta = TTabletResources()
            .SetTabletStaticMemory(totalMemorySizeDelta);

        UpdateResourceUsage(originatingNode, resourceUsageDelta);

        originatingNode->RemoveDynamicTableLock(transaction->GetId());

        if (updateMode == EUpdateMode::Append) {
            chunkManager->ClearChunkList(branchedChunkLists[EChunkListContentType::Main]);
        }
    }

    TReplicationProgress GatherReplicationProgress(const TTableNode* table)
    {
        if (table->IsExternal()) {
            return {};
        }

        std::vector<TReplicationProgress> progresses;
        std::vector<TLegacyKey> pivotKeys;
        std::vector<TLegacyOwningKey> buffer;

        for (int index = 0; index < std::ssize(table->Tablets()); ++index) {
            auto* tablet = table->Tablets()[index]->As<TTablet>();
            progresses.push_back(tablet->ReplicationProgress());
            pivotKeys.push_back(GetTabletReplicationProgressPivotKey(tablet, index, &buffer));
        }

        return NChaosClient::GatherReplicationProgress(std::move(progresses), pivotKeys, MaxKey().Get());
    }

    void ScatterReplicationProgress(NTableServer::TTableNode* table, TReplicationProgress progress)
    {
        if (table->IsExternal()) {
            return;
        }

        std::vector<TLegacyKey> pivotKeys;
        std::vector<TLegacyOwningKey> buffer;
        for (int index = 0; index < std::ssize(table->Tablets()); ++index) {
            auto* tablet = table->Tablets()[index]->As<TTablet>();
            pivotKeys.push_back(GetTabletReplicationProgressPivotKey(tablet, index, &buffer));
        }

        auto newProgresses = NChaosClient::ScatterReplicationProgress(
            std::move(progress),
            pivotKeys,
            MaxKey().Get());

        for (int index = 0; index < std::ssize(table->Tablets()); ++index) {
            auto* tablet = table->Tablets()[index]->As<TTablet>();
            tablet->ReplicationProgress() = std::move(newProgresses[index]);
        }
    }

    TLegacyKey GetTabletReplicationProgressPivotKey(
        TTablet* tablet,
        int tabletIndex,
        std::vector<TLegacyOwningKey>* buffer)
    {
        if (tablet->GetTable()->IsSorted()) {
            return tablet->GetPivotKey().Get();
        } else if (tabletIndex == 0) {
            return EmptyKey().Get();
        } else {
            buffer->push_back(MakeUnversionedOwningRow(tabletIndex));
            return buffer->back().Get();
        }
    }

    TGuid GenerateTabletBalancerCorrelationId() const
    {
        auto mutationContext = GetCurrentMutationContext();
        const auto& generator = mutationContext->RandomGenerator();
        ui64 lo = generator->Generate<ui64>();
        ui64 hi = generator->Generate<ui64>();
        return TGuid(lo, hi);
    }

    TTabletActionId SpawnTabletAction(const TReshardDescriptor& descriptor)
    {
        std::vector<TTabletId> tabletIds;
        for (const auto& tablet : descriptor.Tablets) {
            tabletIds.push_back(tablet->GetId());
        }

        auto* tablet = descriptor.Tablets[0];
        YT_VERIFY(tablet->GetType() == EObjectType::Tablet);

        auto* table = descriptor.Tablets[0]->As<TTablet>()->GetTable();

        auto correlationId = GenerateTabletBalancerCorrelationId();

        YT_LOG_DEBUG("Automatically resharding tablets "
            "(TableId: %v, TabletIds: %v, NewTabletCount: %v, TotalSize: %v, Bundle: %v, "
            "TabletBalancerCorrelationId: %v, Sync: true)",
            table->GetId(),
            tabletIds,
            descriptor.TabletCount,
            descriptor.DataSize,
            table->TabletCellBundle()->GetName(),
            correlationId);

        const auto& tablets = descriptor.Tablets;
        try {
            auto* action = CreateTabletAction(
                TObjectId{},
                ETabletActionKind::Reshard,
                std::vector<TTabletBase*>(tablets.begin(), tablets.end()),
                /*cells*/ {},
                /*pivotKeys*/ {},
                descriptor.TabletCount,
                /*skipFreezing*/ false,
                correlationId,
                TInstant::Zero(),
                /*expirationTimeout*/ std::nullopt);
            return action->GetId();
        } catch (const std::exception& ex) {
            YT_LOG_DEBUG(ex, "Failed to create tablet action during sync reshard (TabletBalancerCorrelationId: %v)",
                correlationId);
            return NullObjectId;
        }
    }

    TTabletActionId SpawnTabletAction(const TTabletMoveDescriptor& descriptor)
    {
        auto* tablet = descriptor.Tablet;
        YT_VERIFY(tablet->GetType() == EObjectType::Tablet);

        auto* table = tablet->GetTable();

        auto correlationId = GenerateTabletBalancerCorrelationId();

        YT_LOG_DEBUG("Moving tablet during cell balancing "
            "(TableId: %v, InMemoryMode: %v, TabletId: %v, SrcCellId: %v, DstCellId: %v, "
            "Bundle: %v, TabletBalancerCorrelationId: %v, Sync: true)",
            table->GetId(),
            table->GetInMemoryMode(),
            descriptor.Tablet->GetId(),
            descriptor.Tablet->GetCell()->GetId(),
            descriptor.TabletCellId,
            table->TabletCellBundle()->GetName(),
            correlationId);

        try {
            auto* action = CreateTabletAction(
                TObjectId{},
                ETabletActionKind::Move,
                {tablet},
                {GetTabletCellOrThrow(descriptor.TabletCellId)},
                /*pivotKeys*/ {},
                /*tabletCount*/ std::nullopt,
                /*skipFreezing*/ false,
                correlationId,
                TInstant::Zero(),
                /*expirationTimeout*/ std::nullopt);
            return action->GetId();
        } catch (const std::exception& ex) {
            YT_LOG_DEBUG("Failed to create tablet action during sync cells balancing (TabletBalancerCorrelationId: %v)",
                correlationId);
            return NullObjectId;
        }
    }

    std::vector<TTabletActionId> SyncBalanceCells(
        TTabletCellBundle* bundle,
        const std::optional<std::vector<TTableNode*>>& tables,
        bool keepActions)
    {
        if (bundle->GetActiveTabletActionCount() > 0) {
            THROW_ERROR_EXCEPTION("Bundle is already being balanced, try again later");
        }

        std::optional<THashSet<const TTableNode*>> tablesSet;
        if (tables) {
            tablesSet = THashSet<const TTableNode*>(tables->begin(), tables->end());
        }

        std::vector<TTabletActionId> actions;
        TTabletBalancerContext context;
        auto descriptors = ReassignInMemoryTablets(
            bundle,
            tablesSet,
            /*ignoreConfig*/ true);

        for (const auto& descriptor : descriptors) {
            if (auto actionId = SpawnTabletAction(descriptor)) {
                actions.push_back(actionId);
            }
        }

        if (keepActions) {
            SetSyncTabletActionsKeepalive(actions);
        }

        return actions;
    }

    std::vector<TTabletActionId> SyncBalanceTablets(
        TTableNode* table,
        bool keepActions)
    {
        ValidateSyncBalanceTablets(table);

        for (const auto& tablet : table->Tablets()) {
            if (tablet->GetAction()) {
                THROW_ERROR_EXCEPTION("Table is already being balanced, try again later")
                    << TErrorAttribute("tablet_id", tablet->GetId());
            }
        }

        std::vector<TTabletActionId> actions;
        TTabletBalancerContext context;

        std::vector<TTablet*> tablets;
        tablets.reserve(table->Tablets().size());
        for (auto* tablet : table->Tablets()) {
            tablets.push_back(tablet->As<TTablet>());
        }

        auto descriptors = MergeSplitTabletsOfTable(
            tablets,
            &context);

        for (const auto& descriptor : descriptors) {
            if (auto actionId = SpawnTabletAction(descriptor)) {
                actions.push_back(actionId);
            }
        }

        if (keepActions) {
            SetSyncTabletActionsKeepalive(actions);
        }
        return actions;
    }

    void ValidateSyncBalanceTablets(TTableNode* table)
    {
        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot reshard a static table");
        }

        if (table->IsPhysicallyLog()) {
            THROW_ERROR_EXCEPTION("Cannot automatically reshard table of type %Qlv",
                table->GetType());
        }
    }

    void ValidateCloneTabletOwner(
        TTabletOwnerBase* sourceNode,
        ENodeCloneMode mode,
        TAccount* account)
    {
        if (sourceNode->IsForeign()) {
            return;
        }

        auto* trunkSourceNode = sourceNode->GetTrunkNode();

        ValidateNodeCloneMode(trunkSourceNode, mode);

        if (const auto& cellBundle = trunkSourceNode->TabletCellBundle()) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->ValidateObjectLifeStage(cellBundle.Get());
        }

        ValidateResourceUsageIncrease(
            trunkSourceNode,
            TTabletResources().SetTabletCount(
                trunkSourceNode->GetTabletResourceUsage().TabletCount),
            account);
    }

    void ValidateBeginCopyTabletOwner(
        TTabletOwnerBase* sourceNode,
        ENodeCloneMode mode)
    {
        YT_VERIFY(sourceNode->IsNative());

        auto* trunkSourceNode = sourceNode->GetTrunkNode();
        if (const auto& cellBundle = trunkSourceNode->TabletCellBundle()) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->ValidateObjectLifeStage(cellBundle.Get());
        }

        if (IsTableType(sourceNode->GetType())) {
            ValidateBeginCopyTable(sourceNode->As<TTableNode>(), mode);
        } else {
            YT_ABORT();
        }
    }

    void ValidateBeginCopyTable(
        TTableNode* sourceTable,
        ENodeCloneMode mode)
    {
        auto* trunkSourceTable = sourceTable->GetTrunkNode();
        ValidateNodeCloneMode(trunkSourceTable, mode);
    }

    void CloneTabletOwner(
        TTabletOwnerBase* sourceNode,
        TTabletOwnerBase* clonedNode,
        ENodeCloneMode mode)
    {
        if (IsTableType(sourceNode->GetType())) {
            CloneTable(
                sourceNode->As<TTableNode>(),
                clonedNode->As<TTableNode>(),
                mode);
        } else {
            YT_ABORT();
        }
    }

    void CloneTable(
        TTableNode* sourceTable,
        TTableNode* clonedTable,
        ENodeCloneMode mode)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(sourceTable->IsExternal() == clonedTable->IsExternal());

        auto* trunkSourceTable = sourceTable->GetTrunkNode();
        auto* trunkClonedTable = clonedTable; // sic!
        bool isBackupAction = mode == ENodeCloneMode::Backup || mode == ENodeCloneMode::Restore;

        SetTabletCellBundle(clonedTable, trunkSourceTable->TabletCellBundle().Get());

        if (!sourceTable->IsDynamic()) {
            return;
        }

        if (mode == ENodeCloneMode::Backup) {
            trunkClonedTable->SetBackupState(ETableBackupState::BackupCompleted);
        } else if (mode == ENodeCloneMode::Restore) {
            trunkClonedTable->SetBackupState(ETableBackupState::None);
        } else {
            trunkClonedTable->SetBackupState(trunkSourceTable->GetTrunkNode()->GetBackupState());
        }

        if (sourceTable->IsExternal()) {
            return;
        }

        YT_VERIFY(!trunkSourceTable->Tablets().empty());
        YT_VERIFY(trunkClonedTable->Tablets().empty());

        try {
            switch (mode) {
                case ENodeCloneMode::Copy:
                    sourceTable->ValidateAllTabletsFrozenOrUnmounted("Cannot copy dynamic table");
                    break;

                case ENodeCloneMode::Move:
                    sourceTable->ValidateAllTabletsUnmounted("Cannot move dynamic table");
                    break;

                case ENodeCloneMode::Backup:
                case ENodeCloneMode::Restore:
                    break;

                default:
                    YT_ABORT();
            }
        } catch (const std::exception& ex) {
            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), ex, "Error cloning table (TableId: %v, %v)",
                sourceTable->GetId(),
                NRpc::GetCurrentAuthenticationIdentity());
        }

        // Undo the harm done in TChunkOwnerTypeHandler::DoClone.
        auto fakeClonedRootChunkLists = trunkClonedTable->GetChunkLists();
        for (auto* fakeClonedRootChunkList : fakeClonedRootChunkLists) {
            fakeClonedRootChunkList->RemoveOwningNode(trunkClonedTable);
        }

        const auto& sourceTablets = trunkSourceTable->Tablets();
        YT_VERIFY(!sourceTablets.empty());
        auto& clonedTablets = trunkClonedTable->MutableTablets();
        YT_VERIFY(clonedTablets.empty());

        const auto& chunkManager = Bootstrap_->GetChunkManager();

        TChunkLists clonedRootChunkLists;
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            auto* fakeClonedRootChunkList = fakeClonedRootChunkLists[contentType];
            auto* clonedRootChunkList = chunkManager->CreateChunkList(fakeClonedRootChunkList->GetKind());
            clonedRootChunkLists[contentType] = clonedRootChunkList;

            trunkClonedTable->SetChunkList(contentType, clonedRootChunkList);
            clonedRootChunkList->AddOwningNode(trunkClonedTable);
        }

        const auto& backupManager = Bootstrap_->GetBackupManager();

        clonedTablets.reserve(sourceTablets.size());

        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            auto* sourceRootChunkList = trunkSourceTable->GetChunkList(contentType);
            YT_VERIFY(sourceRootChunkList->Children().size() == sourceTablets.size());
        }

        for (int index = 0; index < static_cast<int>(sourceTablets.size()); ++index) {
            auto* sourceTablet = sourceTablets[index];

            auto* clonedTablet = CreateTablet(trunkClonedTable, EObjectType::Tablet)->As<TTablet>();
            clonedTablet->CopyFrom(*sourceTablet);

            for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
                auto* sourceRootChunkList = trunkSourceTable->GetChunkList(contentType);
                auto* tabletChunkList = sourceRootChunkList->Children()[index];
                chunkManager->AttachToChunkList(clonedRootChunkLists[contentType], tabletChunkList);
            }

            clonedTablets.push_back(clonedTablet);
            trunkClonedTable->AccountTabletStatistics(clonedTablet->GetTabletStatistics());

            backupManager->SetClonedTabletBackupState(
                clonedTablet,
                sourceTablet->As<TTablet>(),
                mode);
        }
        trunkClonedTable->RecomputeTabletMasterMemoryUsage();

        if (mode == ENodeCloneMode::Backup) {
            trunkClonedTable->SetBackupCheckpointTimestamp(
                trunkSourceTable->GetBackupCheckpointTimestamp());
            trunkSourceTable->SetBackupCheckpointTimestamp(NullTimestamp);

            trunkClonedTable->SetBackupMode(trunkSourceTable->GetBackupMode());
            trunkSourceTable->SetBackupMode(EBackupMode::None);
        } else if (mode != ENodeCloneMode::Restore) {
            trunkClonedTable->SetBackupCheckpointTimestamp(
                trunkSourceTable->GetBackupCheckpointTimestamp());
        }

        if (sourceTable->IsReplicated()) {
            auto* trunkReplicatedSourceTable = trunkSourceTable->As<TReplicatedTableNode>();
            auto* replicatedClonedTable = clonedTable->As<TReplicatedTableNode>();

            YT_VERIFY(mode != ENodeCloneMode::Move);

            for (const auto* replica : GetValuesSortedByKey(trunkReplicatedSourceTable->Replicas())) {
                const TTableReplicaBackupDescriptor* replicaBackupDescriptor = nullptr;

                if (isBackupAction) {
                    const auto& backupDescriptors = trunkReplicatedSourceTable->ReplicaBackupDescriptors();
                    for (const auto& descriptor : backupDescriptors) {
                        if (descriptor.ReplicaId == replica->GetId()) {
                            replicaBackupDescriptor = &descriptor;
                            break;
                        }
                    }

                    if (!replicaBackupDescriptor) {
                        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                            "Will not clone table replica since it does not participate in backup "
                            "(TableId: %v, ReplicaId: %v)",
                            trunkReplicatedSourceTable->GetId(),
                            replica->GetId());
                        continue;
                    }
                }

                std::vector<i64> committedReplicationRowIndexes;
                committedReplicationRowIndexes.reserve(sourceTablets.size());

                for (int tabletIndex = 0; tabletIndex < std::ssize(sourceTablets); ++tabletIndex) {
                    auto* tablet = trunkReplicatedSourceTable->Tablets()[tabletIndex]->As<TTablet>();
                    YT_VERIFY(tablet == sourceTablets[tabletIndex]);

                    TTableReplicaInfo* replicaInfo;
                    if (mode == ENodeCloneMode::Backup && tablet->GetCell()) {
                        if (tablet->BackedUpReplicaInfos().contains(replica->GetId())) {
                            replicaInfo = &GetOrCrash(tablet->BackedUpReplicaInfos(), replica->GetId());
                        } else {
                            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                                "Tablet does not contain replica info during backup (TabletId: %v, "
                                "TableId: %v, ReplicaId: %v)",
                                tablet->GetId(),
                                trunkReplicatedSourceTable->GetId(),
                                replica->GetId());
                            replicaInfo = tablet->GetReplicaInfo(replica);
                        }
                    } else {
                        replicaInfo = tablet->GetReplicaInfo(replica);
                    }

                    auto replicationRowIndex = replicaInfo->GetCommittedReplicationRowIndex();
                    committedReplicationRowIndexes.push_back(replicationRowIndex);
                }

                TString newReplicaPath = isBackupAction
                    ? replicaBackupDescriptor->ReplicaPath
                    : replica->GetReplicaPath();

                auto* clonedReplica = CreateTableReplica(
                    replicatedClonedTable,
                    replica->GetClusterName(),
                    newReplicaPath,
                    replica->GetMode(),
                    replica->GetPreserveTimestamps(),
                    replica->GetAtomicity(),
                    /*enabled*/ false,
                    replica->GetStartReplicationTimestamp(),
                    committedReplicationRowIndexes);

                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                    "Table replica cloned (OriginalReplicaId: %v, ClonedReplicaId: %v, "
                    "OriginalTableId: %v, ClonedTableId: %v, OriginalReplicaPath: %v, ClonedReplicaPath: %v)",
                    replica->GetId(),
                    clonedReplica->GetId(),
                    sourceTable->GetId(),
                    clonedTable->GetId(),
                    replica->GetReplicaPath(),
                    clonedReplica->GetReplicaPath());
            }

            if (isBackupAction) {
                trunkSourceTable->MutableReplicaBackupDescriptors().clear();
                trunkClonedTable->MutableReplicaBackupDescriptors().clear();

                for (auto& tablet : sourceTablets) {
                    tablet->As<TTablet>()->BackedUpReplicaInfos().clear();
                }
            }
        }

        if (mode == ENodeCloneMode::Backup) {
            backupManager->ReleaseBackupCheckpoint(
                trunkSourceTable,
                sourceTable->GetTransaction());
        }


        UpdateResourceUsage(
            trunkClonedTable,
            trunkClonedTable->GetTabletResourceUsage(),
            /*scheduleTableDataStatisticsUpdate*/ false);

        backupManager->UpdateAggregatedBackupState(trunkClonedTable);
    }


    void ValidateMakeTableDynamic(TTableNode* table)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (table->IsDynamic()) {
            return;
        }

        ValidateResourceUsageIncrease(table, TTabletResources().SetTabletCount(1));
    }

    void MakeTableDynamic(TTableNode* table)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (table->IsDynamic()) {
            return;
        }

        table->SetDynamic(true);

        if (table->IsExternal()) {
            return;
        }

        auto* oldChunkList = table->GetChunkList();

        auto chunks = EnumerateChunksInChunkTree(oldChunkList);

        // Compute last commit timestamp.
        auto lastCommitTimestamp = MinTimestamp;
        for (auto* chunk : chunks) {
            auto miscExt = chunk->ChunkMeta()->FindExtension<TMiscExt>();
            if (miscExt && miscExt->has_max_timestamp()) {
                lastCommitTimestamp = std::max(lastCommitTimestamp, static_cast<TTimestamp>(miscExt->max_timestamp()));
            }
        }

        table->SetLastCommitTimestamp(lastCommitTimestamp);

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* newChunkList = chunkManager->CreateChunkList(table->IsPhysicallySorted()
            ? EChunkListKind::SortedDynamicRoot
            : EChunkListKind::OrderedDynamicRoot);

        table->SetChunkList(newChunkList);
        newChunkList->AddOwningNode(table);

        auto* newHunkChunkList = chunkManager->CreateChunkList(EChunkListKind::HunkRoot);
        table->SetHunkChunkList(newHunkChunkList);
        newHunkChunkList->AddOwningNode(table);

        auto* tablet = CreateTablet(table, EObjectType::Tablet)->As<TTablet>();
        tablet->SetIndex(0);
        if (table->IsSorted()) {
            tablet->SetPivotKey(EmptyKey());
        }
        table->MutableTablets() = {tablet};
        table->RecomputeTabletMasterMemoryUsage();

        auto* tabletChunkList = chunkManager->CreateChunkList(table->IsPhysicallySorted()
            ? EChunkListKind::SortedDynamicTablet
            : EChunkListKind::OrderedDynamicTablet);
        if (table->IsPhysicallySorted()) {
            tabletChunkList->SetPivotKey(EmptyKey());
        }
        chunkManager->AttachToChunkList(newChunkList, tabletChunkList);

        std::vector<TChunkTree*> chunkTrees(chunks.begin(), chunks.end());
        chunkManager->AttachToChunkList(tabletChunkList, chunkTrees);

        auto* tabletHunkChunkList = chunkManager->CreateChunkList(EChunkListKind::Hunk);
        chunkManager->AttachToChunkList(newHunkChunkList, tabletHunkChunkList);

        oldChunkList->RemoveOwningNode(table);

        const auto& securityManager = this->Bootstrap_->GetSecurityManager();
        securityManager->UpdateMasterMemoryUsage(table);
        UpdateResourceUsage(
            table,
            table->GetTabletResourceUsage(),
            /*scheduleTableDataStatisticsUpdate*/ false);

        table->AccountTabletStatistics(tablet->GetTabletStatistics());

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table is switched to dynamic mode (TableId: %v)",
            table->GetId());
    }

    void ValidateMakeTableStatic(TTableNode* table)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (!table->IsDynamic()) {
            return;
        }

        table->ValidateNotBackup("Cannot switch backup table to static mode");

        if (table->IsPhysicallyLog()) {
            THROW_ERROR_EXCEPTION("Cannot switch mode from dynamic to static for table type %Qlv",
                table->GetType());
        }

        if (table->IsSorted()) {
            THROW_ERROR_EXCEPTION("Cannot switch mode from dynamic to static: table is sorted");
        }

        table->ValidateAllTabletsUnmounted("Cannot switch mode from dynamic to static");
    }

    void MakeTableStatic(TTableNode* table)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (!table->IsDynamic()) {
            return;
        }

        table->SetDynamic(false);

        if (table->IsExternal()) {
            return;
        }

        for (auto* tablet : table->Tablets()) {
            table->DiscountTabletStatistics(tablet->GetTabletStatistics());
        }

        auto tabletResourceUsage = table->GetTabletResourceUsage();

        auto* oldChunkList = table->GetChunkList();
        auto* oldHunkChunkList = table->GetHunkChunkList();

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* newChunkList = chunkManager->CreateChunkList(EChunkListKind::Static);

        const auto& objectManager = Bootstrap_->GetObjectManager();

        table->SetChunkList(newChunkList);
        newChunkList->AddOwningNode(table);

        table->SetHunkChunkList(nullptr);

        auto chunks = EnumerateChunksInChunkTree(oldChunkList);
        chunkManager->AttachToChunkList(newChunkList, std::vector<TChunkTree*>{chunks.begin(), chunks.end()});

        YT_VERIFY(EnumerateChunksInChunkTree(oldHunkChunkList).empty());

        oldChunkList->RemoveOwningNode(table);
        oldHunkChunkList->RemoveOwningNode(table);

        for (auto* tablet : table->Tablets()) {
            tablet->SetOwner(nullptr);
            objectManager->UnrefObject(tablet);
        }
        table->MutableTablets().clear();
        table->RecomputeTabletMasterMemoryUsage();

        table->SetLastCommitTimestamp(NullTimestamp);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->UpdateMasterMemoryUsage(table);
        UpdateResourceUsage(
            table,
            -tabletResourceUsage,
            /*scheduleTableDataStatisticsUpdate*/ false);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table is switched to static mode (TableId: %v)",
            table->GetId());
    }


    void LockDynamicTable(
        TTableNode* table,
        TTransaction* transaction,
        TTimestamp timestamp)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());

        if (!GetDynamicConfig()->EnableBulkInsert) {
            THROW_ERROR_EXCEPTION("Bulk insert is disabled");
        }

        if (table->DynamicTableLocks().contains(transaction->GetId())) {
            THROW_ERROR_EXCEPTION("Dynamic table is already locked by this transaction")
                << TErrorAttribute("transaction_id", transaction->GetId());
        }

        table->ValidateNotBackup("Bulk insert into backup tables is not supported");

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        int pendingTabletCount = 0;

        for (auto* tablet : table->Tablets()) {
            if (tablet->GetState() == ETabletState::Unmounted) {
                continue;
            }

            ++pendingTabletCount;
            YT_VERIFY(tablet->As<TTablet>()->UnconfirmedDynamicTableLocks().emplace(transaction->GetId()).second);

            auto* cell = tablet->GetCell();
            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            TReqLockTablet req;
            ToProto(req.mutable_tablet_id(), tablet->GetId());
            ToProto(req.mutable_lock()->mutable_transaction_id(), transaction->GetId());
            req.mutable_lock()->set_timestamp(static_cast<i64>(timestamp));
            hiveManager->PostMessage(mailbox, req);
        }

        transaction->LockedDynamicTables().insert(table);
        table->AddDynamicTableLock(transaction->GetId(), timestamp, pendingTabletCount);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Waiting for tablet lock confirmation (TableId: %v, TransactionId: %v, PendingTabletCount: %v)",
            table->GetId(),
            transaction->GetId(),
            pendingTabletCount);
    }

    void CheckDynamicTableLock(
        TTableNode* table,
        TTransaction* transaction,
        NTableClient::NProto::TRspCheckDynamicTableLock* response)
    {
        Bootstrap_->VerifyPersistentStateRead();

        YT_VERIFY(table->IsTrunk());

        auto it = table->DynamicTableLocks().find(transaction->GetId());
        response->set_confirmed(it && it->second.PendingTabletCount == 0);
    }

    void RecomputeTableTabletStatistics(TTableNode* table)
    {
        table->ResetTabletStatistics();
        for (const auto* tablet : table->Tablets()) {
            table->AccountTabletStatistics(tablet->GetTabletStatistics());
        }
    }

    void OnNodeStorageParametersUpdated(TChunkOwnerBase* node)
    {
        if (!IsTableType(node->GetType())) {
            return;
        }

        auto* tableNode = node->As<TTableNode>();
        if (!tableNode->IsDynamic()) {
            return;
        }

        YT_LOG_DEBUG("Table replication changed, will recompute tablet statistics "
            "(TableId: %v)",
            tableNode->GetId());
        RecomputeTableTabletStatistics(tableNode);
    }

    TTabletCell* GetTabletCellOrThrow(TTabletCellId id)
    {
        auto* cell = FindTabletCell(id);
        if (!IsObjectAlive(cell)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No such tablet cell %v",
                id);
        }
        return cell;
    }

    void ZombifyTabletCell(TTabletCell* cell)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto actions = cell->Actions();
        for (auto* action : actions) {
            // NB: If destination cell disappears, don't drop action - let it continue with some other cells.
            UnbindTabletActionFromCells(action);
            OnTabletActionDisturbed(action, TError("Tablet cell %v has been removed", cell->GetId()));
        }
        YT_VERIFY(cell->Actions().empty());
    }

    TNode* FindTabletLeaderNode(const TTabletBase* tablet) const
    {
        if (!tablet) {
            return nullptr;
        }

        auto* cell = tablet->GetCell();
        if (!cell) {
            return nullptr;
        }

        int leadingPeerId = cell->GetLeadingPeerId();
        if (leadingPeerId == InvalidPeerId) {
            return nullptr;
        }

        YT_VERIFY(leadingPeerId < std::ssize(cell->Peers()));

        return cell->Peers()[leadingPeerId].Node;
    }

    TTabletCellBundle* FindTabletCellBundle(TTabletCellBundleId id)
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        auto* cellBundle = cellManager->FindCellBundle(id);
        if (!cellBundle) {
            return nullptr;
        }
        return cellBundle->GetType() == EObjectType::TabletCellBundle
            ? cellBundle->As<TTabletCellBundle>()
            : nullptr;
    }

    TTabletCellBundle* GetTabletCellBundleOrThrow(TTabletCellBundleId id)
    {
        auto* cellBundle = FindTabletCellBundle(id);
        if (!cellBundle) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No such tablet cell bundle %v",
                id);
        }
        return cellBundle;
    }

    TTabletCellBundle* GetTabletCellBundleByNameOrThrow(const TString& name, bool activeLifeStageOnly)
    {
        auto* cellBundle = DoFindTabletCellBundleByName(name);
        if (!cellBundle) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::ResolveError,
                "No such tablet cell bundle %Qv",
                name);
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            objectManager->ValidateObjectLifeStage(cellBundle);
        }

        return cellBundle;
    }

    TTabletCellBundle* GetDefaultTabletCellBundle()
    {
        return GetBuiltin(DefaultTabletCellBundle_);
    }

    void SetTabletCellBundle(TTabletOwnerBase* table, TTabletCellBundle* newBundle)
    {
        YT_VERIFY(table->IsTrunk());

        auto* oldBundle = table->TabletCellBundle().Get();
        if (oldBundle == newBundle) {
            return;
        }

        if (IsTableType(table->GetType())) {
            DoSetTabletCellBundle(
                table->As<TTableNode>(),
                oldBundle,
                newBundle);
        } else if (table->GetType() == EObjectType::HunkStorage) {
            DoSetTabletCellBundle(
                table->As<THunkStorageNode>(),
                oldBundle,
                newBundle);
        } else {
            YT_ABORT();
        }

        table->TabletCellBundle().Assign(newBundle);
    }

    void DoSetTabletCellBundle(
        TTableNode* table,
        TTabletCellBundle* oldBundle,
        TTabletCellBundle* newBundle)
    {
        YT_VERIFY(table->IsTrunk());

        if (table->IsDynamic()) {
            DoSetTabletCellBundleImpl(table, oldBundle, newBundle);
        }
    }

    void DoSetTabletCellBundle(
        THunkStorageNode* hunkStorage,
        TTabletCellBundle* oldBundle,
        TTabletCellBundle* newBundle)
    {
        YT_VERIFY(hunkStorage->IsTrunk());

        DoSetTabletCellBundleImpl(hunkStorage, oldBundle, newBundle);
    }

    void DoSetTabletCellBundleImpl(
        TTabletOwnerBase* table,
        TTabletCellBundle* oldBundle,
        TTabletCellBundle* newBundle)
    {
        YT_VERIFY(table->IsTrunk());

        auto resourceUsageDelta = table->GetTabletResourceUsage();

        if (table->IsNative()) {
            table->ValidateAllTabletsUnmounted("Cannot change tablet cell bundle");
            if (newBundle && GetDynamicConfig()->EnableTabletResourceValidation) {
                newBundle->ValidateResourceUsageIncrease(resourceUsageDelta);
            }
        }

        if (!table->IsExternal()) {
            if (oldBundle) {
                oldBundle->UpdateResourceUsage(-resourceUsageDelta);
            }
            if (newBundle) {
                newBundle->UpdateResourceUsage(resourceUsageDelta);
            }
        }
    }

    void RecomputeTabletCellStatistics(TCellBase* cellBase)
    {
        if (!IsObjectAlive(cellBase) || cellBase->GetType() != EObjectType::TabletCell) {
            return;
        }

        auto* cell = cellBase->As<TTabletCell>();
        cell->RecomputeClusterStatistics();
    }

    void RecomputeAllTabletCellStatistics()
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        for (auto* cellBase : cellManager->Cells(ECellarType::Tablet)) {
            YT_VERIFY(cellBase->GetType() == EObjectType::TabletCell);
            auto* cell = cellBase->As<TTabletCell>();
            cell->GossipStatistics().Local() = NTabletServer::TTabletCellStatistics();
            for (const auto* tablet : cell->Tablets()) {
                cell->GossipStatistics().Local() += tablet->GetTabletStatistics();
            }
        }
    }

    void OnHunkJournalChunkSealed(TChunk* chunk)
    {
        YT_VERIFY(chunk->IsSealed());

        // TODO(gritukan): Copy chunk list if shared.
        // TODO(gritukan): Multiple parents.

        auto statistics = chunk->GetStatistics();
        AccumulateUniqueAncestorsStatistics(chunk, statistics);
    }

    void WrapWithBackupChunkViews(TTablet* tablet, TTimestamp maxClipTimestamp)
    {
        YT_VERIFY(tablet->GetState() == ETabletState::Unmounted);

        if (!maxClipTimestamp) {
            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                "Attempted to clip backup table by null timestamp (TableId: %v, TabletId: %v)",
                tablet->GetTable()->GetId(),
                tablet->GetId());
        }

        bool needFlatten = false;
        auto* chunkList = tablet->GetChunkList();
        for (const auto* child : chunkList->Children()) {
            if (child->GetType() == EObjectType::ChunkList) {
                needFlatten = true;
                break;
            }
        }

        auto* table = tablet->GetTable();
        CopyChunkListsIfShared(table, tablet->GetIndex(), tablet->GetIndex(), needFlatten);

        auto oldStatistics = tablet->GetTabletStatistics();
        table->DiscountTabletStatistics(oldStatistics);

        chunkList = tablet->GetChunkList();
        std::vector<TChunkTree*> storesToDetach;
        std::vector<TChunkTree*> storesToAttach;

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto& transactionManager = Bootstrap_->GetTransactionManager();

        for (auto* store : chunkList->Children()) {
            TTimestamp minTimestamp = MinTimestamp;
            TTimestamp maxTimestamp = MaxTimestamp;

            auto takeTimestampsFromChunk = [&] (const TChunk* chunk) {
                auto miscExt = chunk->ChunkMeta()->FindExtension<TMiscExt>();
                if (miscExt) {
                    if (miscExt->has_min_timestamp()) {
                        minTimestamp = miscExt->min_timestamp();
                    }
                    if (miscExt->has_max_timestamp()) {
                        maxTimestamp = miscExt->max_timestamp();
                    }
                }
            };

            if (IsChunkTabletStoreType(store->GetType())) {
                takeTimestampsFromChunk(store->AsChunk());
            } else if (store->GetType() == EObjectType::ChunkView) {
                auto* chunkView = store->AsChunkView();

                if (auto transactionId = chunkView->GetTransactionId()) {
                    auto overrideTimestamp = transactionManager->GetTimestampHolderTimestamp(transactionId);
                    minTimestamp = overrideTimestamp;
                    maxTimestamp = overrideTimestamp;
                } else {
                    auto* underlyingTree = chunkView->GetUnderlyingTree();
                    YT_VERIFY(IsChunkTabletStoreType(underlyingTree->GetType()));
                    takeTimestampsFromChunk(underlyingTree->AsChunk());
                }
            }

            if (maxTimestamp <= maxClipTimestamp) {
                continue;
            }

            storesToDetach.push_back(store);

            if (IsDynamicTabletStoreType(store->GetType()) &&
                !tablet->BackupCutoffDescriptor()->DynamicStoreIdsToKeep.contains(store->GetId()))
            {
                continue;
            }

            if (minTimestamp <= maxClipTimestamp) {
                auto* wrappedStore = chunkManager->CreateChunkView(
                    store,
                    TChunkViewModifier().WithMaxClipTimestamp(maxClipTimestamp));
                storesToAttach.push_back(wrappedStore);
            }
        }

        chunkManager->DetachFromChunkList(chunkList, storesToDetach, EChunkDetachPolicy::SortedTablet);
        chunkManager->AttachToChunkList(chunkList, storesToAttach);

        auto newStatistics = tablet->GetTabletStatistics();
        table->AccountTabletStatistics(newStatistics);
    }

    TError PromoteFlushedDynamicStores(TTablet* tablet)
    {
        if (auto error = CheckAllDynamicStoresFlushed(tablet); !error.IsOK()) {
            return error;
        }

        YT_VERIFY(tablet->GetState() == ETabletState::Unmounted);

        auto* table = tablet->GetTable();
        CopyChunkListsIfShared(table, tablet->GetIndex(), tablet->GetIndex());
        auto* chunkList = tablet->GetChunkList();

        auto oldStatistics = tablet->GetTabletStatistics();
        table->DiscountTabletStatistics(oldStatistics);

        std::vector<TChunkTree*> storesToDetach;
        std::vector<TChunkTree*> storesToAttach;

        const auto& chunkManager = Bootstrap_->GetChunkManager();

        for (auto* store : chunkList->Children()) {
            if (store->GetType() == EObjectType::ChunkView) {
                auto* chunkView = store->AsChunkView();

                auto* underlyingTree = chunkView->GetUnderlyingTree();
                if (!IsDynamicTabletStoreType(underlyingTree->GetType())) {
                    continue;
                }

                storesToDetach.push_back(chunkView);

                auto* dynamicStore = underlyingTree->AsDynamicStore();
                YT_VERIFY(dynamicStore->IsFlushed());
                auto* chunk = dynamicStore->GetFlushedChunk();

                if (chunk) {
                    // FIXME(ifsmirnov): chunk view is not always needed, check
                    // chunk min/max timestaps.
                    auto* wrappedStore = chunkManager->CreateChunkView(
                        chunk,
                        chunkView->Modifier());
                    storesToAttach.push_back(wrappedStore);
                }
            } else if (IsDynamicTabletStoreType(store->GetType())) {
                auto* dynamicStore = store->AsDynamicStore();
                YT_VERIFY(dynamicStore->IsFlushed());
                auto* chunk = dynamicStore->GetFlushedChunk();
                if (chunk) {
                    storesToAttach.push_back(chunk);
                }
                storesToDetach.push_back(dynamicStore);
            }
        }

        chunkManager->DetachFromChunkList(
            chunkList,
            storesToDetach,
            table->IsPhysicallySorted()
                ? EChunkDetachPolicy::SortedTablet
                : EChunkDetachPolicy::OrderedTabletSuffix);
        chunkManager->AttachToChunkList(chunkList, storesToAttach);

        auto newStatistics = tablet->GetTabletStatistics();
        table->AccountTabletStatistics(newStatistics);

        return {};
    }

    void UpdateExtraMountConfigKeys(std::vector<TString> keys)
    {
        for (auto&& key : keys) {
            auto [it, inserted] = MountConfigKeysFromNodes_.insert(std::move(key));
            if (inserted) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
                    "Registered new mount config key (Key: %v)",
                    *it);
            }
        }
    }

    TError ApplyBackupCutoff(TTablet* tablet)
    {
        if (!tablet->BackupCutoffDescriptor()) {
            return {};
        }

        auto backupMode = tablet->GetTable()->GetBackupMode();

        TError error;

        switch (backupMode) {
            case EBackupMode::OrderedStrongCommitOrdering:
            case EBackupMode::OrderedExact:
            case EBackupMode::OrderedAtLeast:
            case EBackupMode::OrderedAtMost:
            case EBackupMode::ReplicatedSorted:
                error = ApplyRowIndexBackupCutoff(tablet);
                break;

            case EBackupMode::SortedAsyncReplica:
                ApplyDynamicStoreListBackupCutoff(tablet);
                break;

            default:
                YT_ABORT();
        }

        return error;
    }

    void ParseTabletRangeOrThrow(
        const TTabletOwnerBase* table,
        int* first,
        int* last) const
    {
        TryParseTabletRange(table, first, last)
            .ThrowOnError();
    }

    DECLARE_ENTITY_MAP_ACCESSORS(Tablet, TTabletBase);
    DECLARE_ENTITY_MAP_ACCESSORS(TableReplica, TTableReplica);
    DECLARE_ENTITY_MAP_ACCESSORS(TabletAction, TTabletAction);

    DEFINE_SIGNAL_WITH_ACCESSOR(void(TReplicatedTableData), ReplicatedTableCreated);
    DEFINE_SIGNAL_WITH_ACCESSOR(void(TTableId), ReplicatedTableDestroyed);
    DEFINE_SIGNAL_WITH_ACCESSOR(void(TTableId, TReplicatedTableOptionsPtr), ReplicatedTableOptionsUpdated);
    DEFINE_SIGNAL(void(TReplicaData), ReplicaCreated);
    DEFINE_SIGNAL(void(TTableReplicaId), ReplicaDestroyed);
    DEFINE_SIGNAL(void(TTableReplicaId, ETableReplicaMode), ReplicaModeUpdated);
    DEFINE_SIGNAL(void(TTableReplicaId, bool), ReplicaEnablementUpdated);
    DEFINE_SIGNAL_WITH_ACCESSOR(void(TTableReplicaId, bool), ReplicaTrackingPolicyUpdated);

private:
    template <class T>
    class TEntityMapTypeTraits
    {
    public:
        explicit TEntityMapTypeTraits(TBootstrap* bootstrap)
            : Bootstrap_(bootstrap)
        { }

        std::unique_ptr<T> Create(TObjectId id) const
        {
            auto type = TypeFromId(id);
            const auto& objectManager = Bootstrap_->GetObjectManager();
            const auto& handler = objectManager->FindHandler(type);
            auto objectHolder = handler->InstantiateObject(id);
            return std::unique_ptr<T>(objectHolder.release()->As<T>());
        }

    private:
        TBootstrap* const Bootstrap_;
    };

    const TTabletServicePtr TabletService_;
    const TTabletBalancerPtr TabletBalancer_;
    const TTabletCellDecommissionerPtr TabletCellDecommissioner_;
    const TTabletActionManagerPtr TabletActionManager_;

    TEntityMap<TTabletBase, TEntityMapTypeTraits<TTabletBase>> TabletMap_;
    TEntityMap<TTableReplica> TableReplicaMap_;
    TEntityMap<TTabletAction> TabletActionMap_;

    TPeriodicExecutorPtr TabletCellStatisticsGossipExecutor_;
    TPeriodicExecutorPtr BundleResourceUsageGossipExecutor_;
    TPeriodicExecutorPtr ProfilingExecutor_;

    using TProfilerKey = std::tuple<std::optional<ETabletStoresUpdateReason>, TString, bool>;

    TTimeCounter TabletNodeHeartbeatCounter_ = TabletServerProfiler.TimeCounter("/tablet_node_heartbeat");
    THashMap<TProfilerKey, TProfilingCounters> Counters_;

    // Mount config keys received from nodes. Persisted.
    THashSet<TString> MountConfigKeysFromNodes_;
    // Mount config keys known to the binary (by the moment of most recent reign change). Persisted.
    THashSet<TString> LocalMountConfigKeys_;

    INodePtr BuildOrchidYson() const
    {
        std::vector<TString> extraMountConfigKeys;
        for (const auto& key : MountConfigKeysFromNodes_) {
            if (!LocalMountConfigKeys_.contains(key)) {
                extraMountConfigKeys.push_back(key);
            }
        }

        // NB: Orchid node is materialized explicitly because |opaque| is not applied
        // if BuildYsonFluently(consumer) is used, and we want to save some screen space.
        return BuildYsonNodeFluently()
            .BeginMap()
                .Item("extra_mount_config_keys").Value(extraMountConfigKeys)
                .Item("local_mount_config_keys")
                    .BeginAttributes()
                        .Item("opaque").Value(true)
                    .EndAttributes()
                    .Value(LocalMountConfigKeys_)
                .Item("mount_config_keys_from_nodes")
                    .BeginAttributes()
                        .Item("opaque").Value(true)
                    .EndAttributes()
                    .Value(MountConfigKeysFromNodes_)
            .EndMap();
    }

    TProfilingCounters* GetCounters(std::optional<ETabletStoresUpdateReason> reason, TTabletOwnerBase* owner)
    {
        static TProfilingCounters nullCounters;
        if (IsRecovery()) {
            return &nullCounters;
        }

        // TODO(gritukan)
        if (!IsTableType(owner->GetType())) {
            return &nullCounters;
        }

        auto* table = owner->As<TTableNode>();
        const auto& cellBundle = table->TabletCellBundle();
        if (!cellBundle) {
            return &nullCounters;
        }

        TProfilerKey key{reason, cellBundle->GetName(), table->IsPhysicallySorted()};
        auto it = Counters_.find(key);
        if (it != Counters_.end()) {
            return &it->second;
        }

        auto profiler = TabletServerProfiler
            .WithSparse()
            .WithTag("tablet_cell_bundle", std::get<TString>(key))
            .WithTag("table_type", table->IsPhysicallySorted() ? "sorted" : "ordered");

        if (reason) {
            profiler = profiler.WithTag("update_reason", FormatEnum(*reason));
        }

        it = Counters_.emplace(
            key,
            TProfilingCounters{profiler}).first;
        return &it->second;
    }

    THashMap<TTabletCellBundleId, TTabletCellBundleProfilingCounters>
        BundleIdToProfilingCounters_;

    TTabletCellBundleId DefaultTabletCellBundleId_;
    TTabletCellBundle* DefaultTabletCellBundle_ = nullptr;

    TTabletCellBundleId SequoiaTabletCellBundleId_;
    TTabletCellBundle* SequoiaTabletCellBundle_ = nullptr;

    bool EnableUpdateStatisticsOnHeartbeat_ = true;

    // Not a compat, actually.
    bool FillMountConfigKeys_ = false;

    // COMPAT(ifsmirnov)
    bool RecomputeAggregateTabletStatistics_ = false;
    // COMPAT(ifsmirnov)
    bool RecomputeHunkResourceUsage_ = false;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    TTabletCellBundleProfilingCounters GetOrCreateBundleProfilingCounters(
        TTabletCellBundle* bundle)
    {
        auto it = BundleIdToProfilingCounters_.find(bundle->GetId());
        if (it != BundleIdToProfilingCounters_.end()) {
            if (it->second.BundleName == bundle->GetName()) {
                return it->second;
            } else {
                BundleIdToProfilingCounters_.erase(it);
            }
        }

        TTabletCellBundleProfilingCounters counters(bundle->GetName());
        BundleIdToProfilingCounters_.emplace(bundle->GetId(), counters);
        return counters;
    }

    void OnTabletCellDecommissionStarted(TCellBase* cellBase)
    {
        if (cellBase->GetType() != EObjectType::TabletCell) {
            return;
        }

        auto* cell = cellBase->As<TTabletCell>();
        auto actions = cell->Actions();
        for (auto* action : actions) {
            // NB: If destination cell disappears, don't drop action - let it continue with some other cells.
            UnbindTabletActionFromCells(action);
            OnTabletActionDisturbed(action, TError("Tablet cell %v has been decommissioned", cell->GetId()));
        }

        CheckIfFullyUnmounted(cell);
    }

    void CheckIfFullyUnmounted(TTabletCell* tabletCell)
    {
        if (!tabletCell->IsDecommissionStarted()) {
            return;
        }
        if (tabletCell->GossipStatistics().Local().TabletCount == 0) {
            tabletCell->GossipStatus().Local().Decommissioned = true;
        }
    }

    TTabletCellBundle* DoFindTabletCellBundleByName(const TString& name)
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        auto* bundle = cellManager->FindCellBundleByName(name, ECellarType::Tablet, false);
        if (!bundle) {
            return nullptr;
        }
        YT_VERIFY(bundle->GetType() == EObjectType::TabletCellBundle);
        return bundle->As<TTabletCellBundle>();
    }

    TTabletCellBundle* FindTabletCellBundleByName(const TString& name, bool activeLifeStageOnly)
    {
        auto* bundle = DoFindTabletCellBundleByName(name);
        if (!bundle) {
            return bundle;
        }

        if (activeLifeStageOnly) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            return objectManager->IsObjectLifeStageValid(bundle)
                ? bundle
                : nullptr;
        } else {
            return bundle;
        }
    }

    TTabletAction* DoCreateTabletAction(
        TObjectId hintId,
        ETabletActionKind kind,
        ETabletActionState state,
        const std::vector<TTabletBase*>& tablets,
        const std::vector<TTabletCell*>& cells,
        const std::vector<NTableClient::TLegacyOwningKey>& pivotKeys,
        std::optional<int> tabletCount,
        bool freeze,
        bool skipFreezing,
        TGuid correlationId,
        TInstant expirationTime,
        std::optional<TDuration> expirationTimeout)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(state == ETabletActionState::Preparing || state == ETabletActionState::Orphaned);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::TabletAction, hintId);
        auto actionHolder = TPoolAllocator::New<TTabletAction>(id);
        auto* action = TabletActionMap_.Insert(id, std::move(actionHolder));
        objectManager->RefObject(action);

        for (auto* tablet : tablets) {
            YT_VERIFY(tablet->GetType() == EObjectType::Tablet);

            tablet->SetAction(action);

            if (state == ETabletActionState::Orphaned) {
                // Orphaned action can be created during mount if tablet cells are not available.
                // User can't create orphaned action directly because primary master need to know about mount.
                YT_VERIFY(tablet->GetState() == ETabletState::Unmounted);
                tablet->SetExpectedState(freeze
                    ? ETabletState::Frozen
                    : ETabletState::Mounted);
            }
        }
        for (auto* cell : cells) {
            cell->Actions().insert(action);
        }

        action->SetKind(kind);
        action->SetState(state);
        action->Tablets() = std::move(tablets);
        action->TabletCells() = std::move(cells);
        action->PivotKeys() = std::move(pivotKeys);
        action->SetTabletCount(tabletCount);
        action->SetSkipFreezing(skipFreezing);
        action->SetFreeze(freeze);
        action->SetCorrelationId(correlationId);
        action->SetExpirationTime(expirationTime);
        action->SetExpirationTimeout(expirationTimeout);
        const auto& bundle = action->Tablets()[0]->GetOwner()->TabletCellBundle();
        action->SetTabletCellBundle(bundle.Get());
        bundle->TabletActions().insert(action);
        bundle->IncreaseActiveTabletActionCount();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet action created (%v)",
            *action);

        return action;
    }

    void UnbindTabletActionFromCells(TTabletAction* action)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        for (auto* cell : action->TabletCells()) {
            cell->Actions().erase(action);
        }

        action->TabletCells().clear();
    }

    void UnbindTabletActionFromTablets(TTabletAction* action)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        for (auto* tablet : action->Tablets()) {
            YT_VERIFY(tablet->GetAction() == action);
            tablet->SetAction(nullptr);
        }

        action->SaveTabletIds();
        action->Tablets().clear();
    }

    void UnbindTabletAction(TTabletAction* action)
    {
        UnbindTabletActionFromTablets(action);
        UnbindTabletActionFromCells(action);
    }

    std::vector<TLegacyOwningKey> CalculatePivotKeys(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount)
    {
        ParseTabletRangeOrThrow(table, &firstTabletIndex, &lastTabletIndex);

        if (newTabletCount <= 0) {
            THROW_ERROR_EXCEPTION("Tablet count must be positive");
        }

        struct TEntry
        {
            TLegacyOwningKey MinKey;
            TLegacyOwningKey MaxKey;
            i64 Size;

            bool operator<(const TEntry& other) const
            {
                return MinKey < other.MinKey;
            }
        };

        std::vector<TEntry> entries;
        i64 totalSize = 0;

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index]->As<TTablet>();
            THashSet<TStoreId> edenStoreIds(
                tablet->EdenStoreIds().begin(),
                tablet->EdenStoreIds().end());

            auto chunksOrViews = EnumerateStoresInChunkTree(tablet->GetChunkList());
            for (const auto* chunkOrView : chunksOrViews) {
                const auto* chunk = chunkOrView->GetType() == EObjectType::ChunkView
                    ? chunkOrView->AsChunkView()->GetUnderlyingTree()->AsChunk()
                    : chunkOrView->AsChunk();
                if (chunk->GetChunkType() != EChunkType::Table) {
                    continue;
                }
                auto miscExt = chunk->ChunkMeta()->FindExtension<TMiscExt>();
                auto eden = miscExt && miscExt->eden();
                if (eden || edenStoreIds.contains(chunkOrView->GetId())) {
                    continue;
                }

                auto size = chunk->GetUncompressedDataSize();
                entries.push_back({
                    GetMinKeyOrThrow(chunkOrView),
                    GetUpperBoundKeyOrThrow(chunkOrView),
                    size
                });
                totalSize += size;
            }
        }

        std::sort(entries.begin(), entries.end());

        i64 desired = DivCeil<i64>(totalSize, newTabletCount);
        auto* tablet = table->Tablets()[firstTabletIndex]->As<TTablet>();
        std::vector<TLegacyOwningKey> pivotKeys{tablet->GetPivotKey()};
        TLegacyOwningKey lastKey;
        i64 current = 0;

        for (const auto& entry : entries) {
            if (lastKey && lastKey <= entry.MinKey) {
                if (current >= desired) {
                    current = 0;
                    pivotKeys.push_back(entry.MinKey);
                    lastKey = entry.MaxKey;
                    if (ssize(pivotKeys) == newTabletCount) {
                        break;
                    }
                }
            } else if (entry.MaxKey > lastKey) {
                lastKey = entry.MaxKey;
            }
            current += entry.Size;
        }

        return pivotKeys;
    }

    void MountMissedInActionTablets(TTabletAction* action)
    {
        for (auto* tablet : action->Tablets()) {
            try {
                if (!IsObjectAlive(tablet)) {
                    continue;
                }

                if (!IsObjectAlive(tablet->GetOwner())) {
                    continue;
                }

                switch (tablet->GetState()) {
                    case ETabletState::Mounted:
                        break;

                    case ETabletState::Unmounted:
                        MountTablet(tablet, nullptr, action->GetFreeze());
                        break;

                    case ETabletState::Frozen:
                        if (!action->GetFreeze()) {
                            DoUnfreezeTablet(tablet);
                        }
                        break;

                    default:
                        THROW_ERROR_EXCEPTION("Tablet %v is in unrecognized state %Qv",
                            tablet->GetId(),
                            tablet->GetState());
                }
            } catch (const std::exception& ex) {
                YT_LOG_ERROR_IF(IsMutationLoggingEnabled(), ex, "Error mounting missed in action tablet "
                    "(TabletId: %v, TableId: %v, Bundle: %v, ActionId: %v, TabletBalancerCorrelationId: %v)",
                    tablet->GetId(),
                    tablet->GetOwner()->GetId(),
                    action->GetTabletCellBundle()->GetName(),
                    action->GetId(),
                    action->GetCorrelationId());
            }
        }
    }

    void OnTabletActionTabletsTouched(
        TTabletAction* action,
        const THashSet<TTabletBase*>& touchedTablets,
        const TError& error)
    {
        bool touched = false;
        for (auto* tablet : action->Tablets()) {
            if (touchedTablets.find(tablet) != touchedTablets.end()) {
                YT_VERIFY(tablet->GetAction() == action);
                tablet->SetAction(nullptr);
                // Restore expected state YT-17492.
                tablet->SetState(tablet->GetState());
                touched = true;
            }
        }

        if (!touched) {
            return;
        }

        action->SaveTabletIds();

        auto& tablets = action->Tablets();
        tablets.erase(
            std::remove_if(
                tablets.begin(),
                tablets.end(),
                [&] (auto* tablet) {
                    return touchedTablets.find(tablet) != touchedTablets.end();
                }),
            tablets.end());

        UnbindTabletActionFromCells(action);
        OnTabletActionDisturbed(action, error);
    }

    void TouchAffectedTabletActions(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex,
        const TString& request)
    {
        YT_VERIFY(firstTabletIndex >= 0 && firstTabletIndex <= lastTabletIndex && lastTabletIndex < std::ssize(table->Tablets()));

        auto error = TError("User request %Qv interfered with the action", request);
        THashSet<TTabletBase*> touchedTablets;
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            touchedTablets.insert(table->Tablets()[index]);
        }
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            if (auto* action = table->Tablets()[index]->GetAction()) {
                OnTabletActionTabletsTouched(action, touchedTablets, error);
            }
        }
    }

    void ChangeTabletActionState(TTabletAction* action, ETabletActionState state, bool recursive = true)
    {
        action->SetState(state);
        if (action->IsFinished() && action->GetExpirationTimeout()) {
            action->SetExpirationTime(GetCurrentMutationContext()->GetTimestamp() + *action->GetExpirationTimeout());
        }

        auto tableId = action->Tablets().empty()
            ? TTableId{}
            : action->Tablets()[0]->GetOwner()->GetId();
        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Change tablet action state (ActionId: %v, State: %v, "
            "TableId: %v, Bundle: %v, TabletBalancerCorrelationId: %v),",
            action->GetId(),
            state,
            tableId,
            action->GetTabletCellBundle()->GetName(),
            action->GetCorrelationId());
        if (recursive) {
            OnTabletActionStateChanged(action);
        }
    }

    void OnTabletActionDisturbed(TTabletAction* action, const TError& error)
    {
        // Take care of a rare case when tablet action has been already removed (cf. YT-9754).
        if (!IsObjectAlive(action)) {
            return;
        }

        if (action->Tablets().empty()) {
            action->Error() = error;
            ChangeTabletActionState(action, ETabletActionState::Failed);
            return;
        }

        switch (action->GetState()) {
            case ETabletActionState::Unmounting:
            case ETabletActionState::Freezing:
                // Wait until tablets are unmounted, then mount them.
                action->Error() = error;
                break;

            case ETabletActionState::Mounting:
                // Nothing can be done here.
                action->Error() = error;
                ChangeTabletActionState(action, ETabletActionState::Failed);
                break;

            case ETabletActionState::Completed:
            case ETabletActionState::Failed:
                // All tablets have been already taken care of. Do nothing.
                break;

            case ETabletActionState::Mounted:
            case ETabletActionState::Frozen:
            case ETabletActionState::Unmounted:
            case ETabletActionState::Preparing:
            case ETabletActionState::Failing:
                // Transient states inside mutation. Nothing wrong should happen here.
                YT_ABORT();

            default:
                YT_ABORT();
        }
    }

    void OnTabletActionStateChanged(TTabletAction* action)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (!action) {
            return;
        }

        bool repeat;
        do {
            repeat = false;
            try {
                DoTabletActionStateChanged(action);
            } catch (const std::exception& ex) {
                YT_VERIFY(action->GetState() != ETabletActionState::Failing);
                action->Error() = TError(ex);
                if (action->GetState() != ETabletActionState::Unmounting) {
                    ChangeTabletActionState(action, ETabletActionState::Failing, false);
                }
                repeat = true;
            }
        } while (repeat);
    }

    void DoTabletActionStateChanged(TTabletAction* action)
    {
        switch (action->GetState()) {
            case ETabletActionState::Preparing: {
                if (action->GetSkipFreezing()) {
                    ChangeTabletActionState(action, ETabletActionState::Frozen);
                    break;
                }

                for (auto* tablet : action->Tablets()) {
                    DoFreezeTablet(tablet);
                }

                ChangeTabletActionState(action, ETabletActionState::Freezing);
                break;
            }

            case ETabletActionState::Freezing: {
                int freezingCount = 0;
                for (const auto* tablet : action->Tablets()) {
                    YT_VERIFY(IsObjectAlive(tablet));
                    if (tablet->GetState() == ETabletState::Freezing) {
                        ++freezingCount;
                    }
                }
                if (freezingCount == 0) {
                    auto state = action->Error().IsOK()
                        ? ETabletActionState::Frozen
                        : ETabletActionState::Failing;
                    ChangeTabletActionState(action, state);
                }
                break;
            }

            case ETabletActionState::Frozen: {
                for (auto* tablet : action->Tablets()) {
                    YT_VERIFY(IsObjectAlive(tablet));
                    UnmountTablet(tablet, /*force*/ false, /*onDestroy*/ false);
                }

                ChangeTabletActionState(action, ETabletActionState::Unmounting);
                break;
            }

            case ETabletActionState::Unmounting: {
                int unmountingCount = 0;
                for (const auto* tablet : action->Tablets()) {
                    YT_VERIFY(IsObjectAlive(tablet));
                    if (tablet->GetState() == ETabletState::Unmounting) {
                        ++unmountingCount;
                    }
                }
                if (unmountingCount == 0) {
                    auto state = action->Error().IsOK()
                        ? ETabletActionState::Unmounted
                        : ETabletActionState::Failing;
                    ChangeTabletActionState(action, state);
                }
                break;
            }

            case ETabletActionState::Unmounted: {
                YT_VERIFY(!action->Tablets().empty());
                auto* table = action->Tablets().front()->GetOwner();
                if (!IsObjectAlive(table)) {
                    THROW_ERROR_EXCEPTION("Table is not alive");
                }

                switch (action->GetKind()) {
                    case ETabletActionKind::Move:
                        break;

                    case ETabletActionKind::Reshard: {
                        int firstTabletIndex = action->Tablets().front()->GetIndex();
                        int lastTabletIndex = action->Tablets().back()->GetIndex();

                        auto expectedState = action->GetFreeze() ? ETabletState::Frozen : ETabletState::Mounted;

                        std::vector<TTabletBase*> oldTablets;
                        oldTablets.swap(action->Tablets());
                        for (auto* tablet : oldTablets) {
                            tablet->SetAction(nullptr);
                        }
                        for (auto* tablet : oldTablets) {
                            if (tablet->GetExpectedState() != expectedState) {
                                YT_LOG_ALERT_IF(IsMutationLoggingEnabled() && (tablet->GetExpectedState() != expectedState),
                                    "Unexpected tablet expected state, try fixing with unmount plus mount "
                                    "(TableId: %v, TabletId: %v, ActionId: %v, ActionExpected: %v, TabletExpected: %v)",
                                    tablet->As<TTablet>()->GetTable()->GetId(),
                                    tablet->GetId(),
                                    action->GetId(),
                                    expectedState,
                                    tablet->GetExpectedState());
                                THROW_ERROR_EXCEPTION("Tablet action canceled due to a bug");
                            }
                        }

                        int newTabletCount = action->GetTabletCount()
                            ? *action->GetTabletCount()
                            : action->PivotKeys().size();

                        try {
                            // TODO(ifsmirnov) Use custom locking to allow reshard when locked by operation and upload has not been started yet.
                            const auto& cypressManager = Bootstrap_->GetCypressManager();
                            cypressManager->LockNode(table, nullptr, ELockMode::Exclusive);

                            PrepareReshard(
                                table,
                                firstTabletIndex,
                                lastTabletIndex,
                                newTabletCount,
                                action->PivotKeys());
                            newTabletCount = DoReshard(
                                table,
                                firstTabletIndex,
                                lastTabletIndex,
                                newTabletCount,
                                action->PivotKeys());
                        } catch (const std::exception& ex) {
                            for (auto* tablet : oldTablets) {
                                YT_VERIFY(IsObjectAlive(tablet));
                                tablet->SetAction(action);
                            }
                            action->Tablets() = std::move(oldTablets);
                            throw;
                        }

                        action->Tablets() = std::vector<TTabletBase*>(
                            table->Tablets().begin() + firstTabletIndex,
                            table->Tablets().begin() + firstTabletIndex + newTabletCount);
                        for (auto* tablet : action->Tablets()) {
                            tablet->SetAction(action);
                            tablet->SetExpectedState(expectedState);
                        }

                        break;
                    }

                    default:
                        YT_ABORT();
                }

                auto tableSettings = GetTableSettings(table->As<TTableNode>());
                auto serializedTableSettings = SerializeTableSettings(tableSettings);

                std::vector<std::pair<TTabletBase*, TTabletCell*>> assignment;
                if (action->TabletCells().empty()) {
                    if (!CheckHasHealthyCells(table->TabletCellBundle().Get())) {
                        ChangeTabletActionState(action, ETabletActionState::Orphaned, false);
                        break;
                    }

                    assignment = ComputeTabletAssignment(
                        table,
                        nullptr,
                        action->Tablets());
                } else {
                    YT_VERIFY(action->TabletCells().size() >= action->Tablets().size());
                    for (int index = 0; index < std::ssize(action->Tablets()); ++index) {
                        assignment.emplace_back(
                            action->Tablets()[index],
                            action->TabletCells()[index]);
                    }
                }

                DoMountTablets(table, serializedTableSettings, assignment, action->GetFreeze());

                ChangeTabletActionState(action, ETabletActionState::Mounting);
                break;
            }

            case ETabletActionState::Mounting: {
                int mountedCount = 0;
                for (const auto* tablet : action->Tablets()) {
                    YT_VERIFY(IsObjectAlive(tablet));
                    if (tablet->GetState() == ETabletState::Mounted ||
                        tablet->GetState() == ETabletState::Frozen)
                    {
                        ++mountedCount;
                    }
                }

                if (mountedCount == std::ssize(action->Tablets())) {
                    ChangeTabletActionState(action, ETabletActionState::Mounted);
                }
                break;
            }

            case ETabletActionState::Mounted: {
                ChangeTabletActionState(action, ETabletActionState::Completed);
                break;
            }

            case ETabletActionState::Failing: {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), action->Error(), "Tablet action failed (ActionId: %v, TabletBalancerCorrelationId: %v)",
                    action->GetId(),
                    action->GetCorrelationId());

                MountMissedInActionTablets(action);
                UnbindTabletAction(action);
                ChangeTabletActionState(action, ETabletActionState::Failed);
                break;
            }

            case ETabletActionState::Completed:
                if (!action->Error().IsOK()) {
                    ChangeTabletActionState(action, ETabletActionState::Failed, false);
                }
                // No break intentionally.
            case ETabletActionState::Failed: {
                UnbindTabletAction(action);
                const auto now = GetCurrentMutationContext()->GetTimestamp();
                if (action->GetExpirationTime() <= now) {
                    const auto& objectManager = Bootstrap_->GetObjectManager();
                    objectManager->UnrefObject(action);
                }
                if (auto* bundle = action->GetTabletCellBundle()) {
                    bundle->DecreaseActiveTabletActionCount();
                }
                break;
            }

            default:
                YT_ABORT();
        }
    }

    void HydraKickOrphanedTabletActions(NProto::TReqKickOrphanedTabletActions* request)
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        THashSet<TCellBundle*> healthyBundles;
        for (auto* bundle : cellManager->CellBundles(ECellarType::Tablet)) {
            if (!IsObjectAlive(bundle)) {
                continue;
            }

            for (auto* cellBase : bundle->Cells()) {
                YT_VERIFY(cellBase->GetType() == EObjectType::TabletCell);
                auto* cell = cellBase->As<TTabletCell>();
                if (IsCellActive(cell)) {
                    healthyBundles.insert(cell->CellBundle().Get());
                    continue;
                }
            }
        }

        auto orphanedActionIds = FromProto<std::vector<TTabletActionId>>(request->tablet_action_ids());
        for (auto actionId : orphanedActionIds) {
            auto* action = FindTabletAction(actionId);
            if (IsObjectAlive(action) && action->GetState() == ETabletActionState::Orphaned) {
                const auto& bundle = action->Tablets().front()->GetOwner()->TabletCellBundle();
                if (healthyBundles.contains(bundle.Get())) {
                    ChangeTabletActionState(action, ETabletActionState::Unmounted);
                }
            }
        }
    }


    struct TTableSettings
    {
        TTableMountConfigPtr MountConfig;
        IMapNodePtr MountConfigNode;
        IMapNodePtr ExtraMountConfigAttributes;
        NTabletNode::TTabletStoreReaderConfigPtr StoreReaderConfig;
        NTabletNode::TTabletHunkReaderConfigPtr HunkReaderConfig;
        NTabletNode::TTabletStoreWriterConfigPtr StoreWriterConfig;
        NTabletNode::TTabletStoreWriterOptionsPtr StoreWriterOptions;
        NTabletNode::TTabletHunkWriterConfigPtr HunkWriterConfig;
        NTabletNode::TTabletHunkWriterOptionsPtr HunkWriterOptions;
    };

    TTableSettings GetTableSettings(TTableNode* table)
    {
        TTableSettings result;

        const auto& dynamicConfig = GetDynamicConfig();
        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto tableProxy = objectManager->GetProxy(table);
        const auto& tableAttributes = tableProxy->Attributes();

        // Parse and prepare mount config.
        try {
            // Handle builtin attributes.
            auto builtinMountConfig = ConvertTo<TBuiltinTableMountConfigPtr>(tableAttributes);
            if (!table->GetProfilingMode()) {
                builtinMountConfig->ProfilingMode = dynamicConfig->DynamicTableProfilingMode;
            }
            builtinMountConfig->EnableDynamicStoreRead = IsDynamicStoreReadEnabled(table);

            // Extract custom attributes and build combined node.
            auto combinedConfigNode = ConvertTo<IMapNodePtr>(builtinMountConfig);

            if (const auto* storage = table->FindMountConfigStorage();
                storage && !storage->IsEmpty())
            {
                auto [customConfigNode, unrecognizedCustomConfigNode] = storage->GetRecognizedConfig();

                if (unrecognizedCustomConfigNode->GetChildCount() > 0) {
                    result.ExtraMountConfigAttributes = unrecognizedCustomConfigNode;
                }

                combinedConfigNode = PatchNode(combinedConfigNode, customConfigNode)->AsMap();
            }

            // The next line is important for validation.
            result.MountConfig = ConvertTo<TTableMountConfigPtr>(combinedConfigNode);

            result.MountConfigNode = combinedConfigNode;
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing table mount configuration")
                << ex;
        }

        // Parse and prepare store reader config.
        try {
            result.StoreReaderConfig = UpdateYsonStruct(
                GetDynamicConfig()->StoreChunkReader,
                // TODO(babenko): rename to store_chunk_reader
                tableAttributes.FindYson(EInternedAttributeKey::ChunkReader.Unintern()));
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing store reader config")
                << ex;
        }

        // Parse and prepare hunk reader config.
        try {
            result.HunkReaderConfig = UpdateYsonStruct(
                GetDynamicConfig()->HunkChunkReader,
                tableAttributes.FindYson(EInternedAttributeKey::HunkChunkReader.Unintern()));
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing hunk reader config")
                << ex;
        }

        const auto& chunkReplication = table->Replication();
        auto primaryMediumIndex = table->GetPrimaryMediumIndex();
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* primaryMedium = chunkManager->GetMediumByIndex(primaryMediumIndex);
        auto replicationFactor = chunkReplication.Get(primaryMediumIndex).GetReplicationFactor();

        // Prepare store writer options.
        result.StoreWriterOptions = New<NTabletNode::TTabletStoreWriterOptions>();
        result.StoreWriterOptions->ReplicationFactor = replicationFactor;
        result.StoreWriterOptions->MediumName = primaryMedium->GetName();
        result.StoreWriterOptions->Account = table->GetAccount()->GetName();
        result.StoreWriterOptions->CompressionCodec = table->GetCompressionCodec();
        result.StoreWriterOptions->ErasureCodec = table->GetErasureCodec();
        result.StoreWriterOptions->EnableStripedErasure = table->GetEnableStripedErasure();
        result.StoreWriterOptions->ChunksVital = chunkReplication.GetVital();
        result.StoreWriterOptions->OptimizeFor = table->GetOptimizeFor();

        // Prepare hunk writer options.
        result.HunkWriterOptions = New<NTabletNode::TTabletHunkWriterOptions>();
        result.HunkWriterOptions->ReplicationFactor = replicationFactor;
        result.HunkWriterOptions->MediumName = primaryMedium->GetName();
        result.HunkWriterOptions->Account = table->GetAccount()->GetName();
        result.HunkWriterOptions->CompressionCodec = table->GetCompressionCodec();
        result.HunkWriterOptions->ErasureCodec = table->GetHunkErasureCodec();
        result.HunkWriterOptions->ChunksVital = chunkReplication.GetVital();

        // Parse and prepare store writer config.
        try {
            auto config = CloneYsonSerializable(GetDynamicConfig()->StoreChunkWriter);
            config->PreferLocalHost = primaryMedium->Config()->PreferLocalHostForDynamicTables;
            if (GetDynamicConfig()->IncreaseUploadReplicationFactor ||
                table->TabletCellBundle()->GetDynamicOptions()->IncreaseUploadReplicationFactor)
            {
                config->UploadReplicationFactor = replicationFactor;
            }

            result.StoreWriterConfig = UpdateYsonStruct(
                config,
                // TODO(babenko): rename to store_chunk_writer
                tableAttributes.FindYson(EInternedAttributeKey::ChunkWriter.Unintern()));
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing store writer config")
                << ex;
        }

        // Parse and prepare hunk writer config.
        try {
            auto config = CloneYsonSerializable(GetDynamicConfig()->HunkChunkWriter);
            config->PreferLocalHost = primaryMedium->Config()->PreferLocalHostForDynamicTables;
            config->UploadReplicationFactor = replicationFactor;

            result.HunkWriterConfig = UpdateYsonStruct(
                config,
                tableAttributes.FindYson(EInternedAttributeKey::HunkChunkWriter.Unintern()));
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing hunk writer config")
                << ex;
        }

        return result;
    }

    struct TSerializedTableSettings
    {
        TYsonString MountConfig;
        TYsonString ExtraMountConfigAttributes;
        TYsonString StoreReaderConfig;
        TYsonString HunkReaderConfig;
        TYsonString StoreWriterConfig;
        TYsonString StoreWriterOptions;
        TYsonString HunkWriterConfig;
        TYsonString HunkWriterOptions;
    };

    static TSerializedTableSettings SerializeTableSettings(const TTableSettings& tableSettings)
    {
        return {
            .MountConfig = ConvertToYsonString(tableSettings.MountConfigNode),
            .ExtraMountConfigAttributes = tableSettings.ExtraMountConfigAttributes
                ? ConvertToYsonString(tableSettings.ExtraMountConfigAttributes)
                : TYsonString{},
            .StoreReaderConfig = ConvertToYsonString(tableSettings.StoreReaderConfig),
            .HunkReaderConfig = ConvertToYsonString(tableSettings.HunkReaderConfig),
            .StoreWriterConfig = ConvertToYsonString(tableSettings.StoreWriterConfig),
            .StoreWriterOptions = ConvertToYsonString(tableSettings.StoreWriterOptions),
            .HunkWriterConfig = ConvertToYsonString(tableSettings.HunkWriterConfig),
            .HunkWriterOptions = ConvertToYsonString(tableSettings.HunkWriterOptions)
        };
    }

    template <class TRequest>
    void FillTableSettings(TRequest* request, const TSerializedTableSettings& serializedTableSettings)
    {
        auto* tableSettings = request->mutable_table_settings();
        tableSettings->set_mount_config(serializedTableSettings.MountConfig.ToString());
        if (serializedTableSettings.ExtraMountConfigAttributes) {
            tableSettings->set_extra_mount_config_attributes(
                serializedTableSettings.ExtraMountConfigAttributes.ToString());
        }
        tableSettings->set_store_reader_config(serializedTableSettings.StoreReaderConfig.ToString());
        tableSettings->set_hunk_reader_config(serializedTableSettings.HunkReaderConfig.ToString());
        tableSettings->set_store_writer_config(serializedTableSettings.StoreWriterConfig.ToString());
        tableSettings->set_store_writer_options(serializedTableSettings.StoreWriterOptions.ToString());
        tableSettings->set_hunk_writer_config(serializedTableSettings.HunkWriterConfig.ToString());
        tableSettings->set_hunk_writer_options(serializedTableSettings.HunkWriterOptions.ToString());
    }

    struct THunkStorageSettings
    {
        NTabletNode::THunkStorageMountConfigPtr MountConfig;
        NTabletNode::THunkStoreWriterConfigPtr HunkStoreConfig;
        NTabletNode::THunkStoreWriterOptionsPtr HunkStoreOptions;
    };

    THunkStorageSettings GetHunkStorageSettings(THunkStorageNode* hunkStorage)
    {
        THunkStorageSettings result;

        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto hunkStorageProxy = objectManager->GetProxy(hunkStorage);
        const auto& tableAttributes = hunkStorageProxy->Attributes();

        // Parse and prepare mount config.
        try {
            result.MountConfig = ConvertTo<NTabletNode::THunkStorageMountConfigPtr>(tableAttributes);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing hunk storage mount configuration")
                << ex;
        }

        // Parse and prepare store writer config.
        try {
            result.HunkStoreConfig = UpdateYsonStruct(
                GetDynamicConfig()->HunkStoreWriter,
                tableAttributes.FindYson(EInternedAttributeKey::HunkStoreWriter.Unintern()));
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing hunk store writer config")
                << ex;
        }

        const auto& chunkReplication = hunkStorage->Replication();
        auto primaryMediumIndex = hunkStorage->GetPrimaryMediumIndex();
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* primaryMedium = chunkManager->GetMediumByIndex(primaryMediumIndex);
        auto replicationFactor = chunkReplication.Get(primaryMediumIndex).GetReplicationFactor();

        // Prepare store writer options.
        auto storeWriterOptions = New<NTabletNode::THunkStoreWriterOptions>();
        storeWriterOptions->MediumName = primaryMedium->GetName();
        storeWriterOptions->Account = hunkStorage->GetAccount()->GetName();
        storeWriterOptions->ErasureCodec = hunkStorage->GetErasureCodec();
        storeWriterOptions->ReplicationFactor = replicationFactor;
        storeWriterOptions->ReadQuorum = hunkStorage->GetReadQuorum();
        storeWriterOptions->WriteQuorum = hunkStorage->GetWriteQuorum();
        storeWriterOptions->EnableMultiplexing = false;
        result.HunkStoreOptions = std::move(storeWriterOptions);

        return result;
    }

    struct TSerializedHunkStorageSettings
    {
        TYsonString MountConfig;
        TYsonString HunkStoreConfig;
        TYsonString HunkStoreOptions;
    };

    static TSerializedHunkStorageSettings SerializeHunkStorageSettings(const THunkStorageSettings& settings)
    {
        return {
            .MountConfig = ConvertToYsonString(settings.MountConfig),
            .HunkStoreConfig = ConvertToYsonString(settings.HunkStoreConfig),
            .HunkStoreOptions = ConvertToYsonString(settings.HunkStoreOptions),
        };
    }

    template <class TRequest>
    void FillHunkStorageSettings(TRequest* request, const TSerializedHunkStorageSettings& settings)
    {
        auto* hunkStorageSettings = request->mutable_hunk_storage_settings();
        hunkStorageSettings->set_mount_config(settings.MountConfig.ToString());
        hunkStorageSettings->set_hunk_store_config(settings.HunkStoreConfig.ToString());
        hunkStorageSettings->set_hunk_store_options(settings.HunkStoreOptions.ToString());
    }

    using TTabletOwnerSettings = std::variant<
        TTableSettings,
        THunkStorageSettings
    >;

    TTabletOwnerSettings GetTabletOwnerSettings(TTabletOwnerBase* table)
    {
        if (IsTableType(table->GetType())) {
            return GetTableSettings(table->As<TTableNode>());
        } else if (table->GetType() == EObjectType::HunkStorage) {
            return GetHunkStorageSettings(table->As<THunkStorageNode>());
        } else {
            YT_ABORT();
        }
    }

    using TSerializedTabletOwnerSettings = std::variant<
        TSerializedTableSettings,
        TSerializedHunkStorageSettings
    >;

    TSerializedTabletOwnerSettings SerializeTabletOwnerSettings(const TTabletOwnerSettings& settings)
    {
        return Visit(settings,
            [] (const TTableSettings& settings) -> TSerializedTabletOwnerSettings { return SerializeTableSettings(settings); },
            [] (const THunkStorageSettings& settings) -> TSerializedTabletOwnerSettings { return SerializeHunkStorageSettings(settings); },
            [] (auto) { YT_ABORT(); });
    }

    void MountTablet(
        TTabletBase* tablet,
        TTabletCell* cell,
        bool freeze)
    {
        auto* table = tablet->GetOwner();
        auto tableSettings = GetTabletOwnerSettings(table);
        auto serializedTableSettings = SerializeTabletOwnerSettings(tableSettings);
        auto assignment = ComputeTabletAssignment(
            table,
            cell,
            std::vector<TTabletBase*>{tablet});

        DoMountTablets(table, serializedTableSettings, assignment, freeze);
    }

    void DoMountTablets(
        TTabletOwnerBase* table,
        const TSerializedTabletOwnerSettings& serializedTableSettings,
        const std::vector<std::pair<TTabletBase*, TTabletCell*>>& assignment,
        bool freeze,
        TTimestamp mountTimestamp = NullTimestamp)
    {
        if (IsTableType(table->GetType())) {
            auto* typedTable = table->As<TTableNode>();
            typedTable->SetMountedWithEnabledDynamicStoreRead(IsDynamicStoreReadEnabled(typedTable));
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        TTabletResources resourceUsageDelta;
        const auto& allTablets = table->Tablets();
        for (auto [tablet, cell] : assignment) {
            YT_VERIFY(tablet->GetState() == ETabletState::Unmounted);

            if (!IsCellActive(cell) && tablet->GetType() == EObjectType::Tablet) {
                DoCreateTabletAction(
                    TObjectId(),
                    ETabletActionKind::Move,
                    ETabletActionState::Orphaned,
                    std::vector<TTabletBase*>{tablet},
                    std::vector<TTabletCell*>{},
                    std::vector<NTableClient::TLegacyOwningKey>{},
                    /*tabletCount*/ std::nullopt,
                    freeze,
                    /*skipFreezing*/ false,
                    /*correlationId*/ {},
                    /*expirationTime*/ TInstant::Zero(),
                    /*expirationTimeout*/ std::nullopt);
                continue;
            }

            for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
                if (auto* chunkList = table->GetChunkList(contentType)) {
                    const auto& chunkLists = chunkList->Children();
                    YT_VERIFY(allTablets.size() == chunkLists.size());
                }
            }

            tablet->SetCell(cell);
            YT_VERIFY(cell->Tablets().insert(tablet).second);
            objectManager->RefObject(cell);

            table->DiscountTabletStatistics(tablet->GetTabletStatistics());

            tablet->SetState(freeze ? ETabletState::FrozenMounting : ETabletState::Mounting);
            tablet->SetInMemoryMode(table->GetInMemoryMode());
            resourceUsageDelta.TabletStaticMemory += tablet->GetTabletStaticMemorySize();

            cell->GossipStatistics().Local() += tablet->GetTabletStatistics();
            table->AccountTabletStatistics(tablet->GetTabletStatistics());

            const auto* context = GetCurrentMutationContext();
            tablet->SetMountRevision(context->GetVersion().ToRevision());
            tablet->SetWasForcefullyUnmounted(false);

            switch (tablet->GetType()) {
                case EObjectType::Tablet:
                    DoMountTableTablet(
                        tablet->As<TTablet>(),
                        table->As<TTableNode>(),
                        std::get<TSerializedTableSettings>(serializedTableSettings),
                        cell,
                        freeze,
                        mountTimestamp);
                    break;

                case EObjectType::HunkTablet:
                    DoMountHunkTablet(
                        tablet->As<THunkTablet>(),
                        std::get<TSerializedHunkStorageSettings>(serializedTableSettings),
                        cell);
                    break;

                default:
                    YT_ABORT();
            }
        }

        UpdateResourceUsage(table, resourceUsageDelta);
    }

    void DoMountTableTablet(
        TTablet* tablet,
        TTableNode* table,
        const TSerializedTableSettings& serializedTableSettings,
        TTabletCell* cell,
        bool freeze,
        TTimestamp mountTimestamp)
    {
        const auto& hiveManager = Bootstrap_->GetHiveManager();
        auto* mailbox = hiveManager->GetMailbox(cell->GetId());

        auto tabletIndex = tablet->GetIndex();
        const auto& allTablets = table->Tablets();

        int preloadPendingStoreCount = 0;

        {
            TReqMountTablet req;

            req.set_retained_timestamp(tablet->GetRetainedTimestamp());
            req.set_path(table->GetMountPath());
            ToProto(req.mutable_tablet_id(), tablet->GetId());
            req.set_mount_revision(tablet->GetMountRevision());
            ToProto(req.mutable_table_id(), table->GetId());

            ToProto(req.mutable_schema_id(), table->GetSchema()->GetId());
            ToProto(req.mutable_schema(), *table->GetSchema()->AsTableSchema());

            if (table->IsSorted() && !table->IsReplicated()) {
                ToProto(req.mutable_pivot_key(), tablet->GetPivotKey());
                ToProto(req.mutable_next_pivot_key(), tablet->GetIndex() + 1 == std::ssize(allTablets)
                    ? MaxKey()
                    : allTablets[tabletIndex + 1]->As<TTablet>()->GetPivotKey());
            } else if (!table->IsSorted()) {
                auto lower = tabletIndex == 0
                    ? EmptyKey()
                    : MakeUnversionedOwningRow(tablet->GetIndex());
                auto upper = tabletIndex + 1 == std::ssize(allTablets)
                    ? MaxKey()
                    : MakeUnversionedOwningRow(tablet->GetIndex() + 1);
                ToProto(req.mutable_pivot_key(), lower);
                ToProto(req.mutable_next_pivot_key(), upper);
            }
            if (!table->IsPhysicallySorted()) {
                req.set_trimmed_row_count(tablet->GetTrimmedRowCount());
            }
            FillTableSettings(&req, serializedTableSettings);
            req.set_atomicity(ToProto<int>(table->GetAtomicity()));
            req.set_commit_ordering(ToProto<int>(table->GetCommitOrdering()));
            req.set_freeze(freeze);
            ToProto(req.mutable_upstream_replica_id(), table->GetUpstreamReplicaId());
            if (table->IsReplicated()) {
                auto* replicatedTable = table->As<TReplicatedTableNode>();
                for (auto* replica : GetValuesSortedByKey(replicatedTable->Replicas())) {
                    const auto* replicaInfo = tablet->GetReplicaInfo(replica);
                    PopulateTableReplicaDescriptor(req.add_replicas(), replica, *replicaInfo);
                }
            }

            if (table->GetReplicationCardId()) {
                if (tablet->ReplicationProgress().Segments.empty()) {
                    if (table->IsSorted()) {
                        tablet->ReplicationProgress().Segments.push_back({tablet->GetPivotKey(), MinTimestamp});
                        tablet->ReplicationProgress().UpperKey = tabletIndex + 1 == std::ssize(allTablets)
                            ? MaxKey()
                            : allTablets[tabletIndex + 1]->As<TTablet>()->GetPivotKey();
                    } else {
                        auto lower = tabletIndex == 0
                            ? EmptyKey()
                            : MakeUnversionedOwningRow(tablet->GetIndex());
                        auto upper = tabletIndex + 1 == std::ssize(allTablets)
                            ? MaxKey()
                            : MakeUnversionedOwningRow(tablet->GetIndex() + 1);
                        tablet->ReplicationProgress().Segments.push_back({std::move(lower), MinTimestamp});
                        tablet->ReplicationProgress().UpperKey = std::move(upper);
                    }
                }

                ToProto(req.mutable_replication_progress(), tablet->ReplicationProgress());
            }

            auto* chunkList = tablet->GetChunkList();
            const auto& chunkListStatistics = chunkList->Statistics();
            i64 startingRowIndex = chunkListStatistics.LogicalRowCount - chunkListStatistics.RowCount;

            std::vector<TChunkTree*> chunksOrViews;
            for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
                auto* chunkList = tablet->GetChunkList(contentType);
                EnumerateStoresInChunkTree(chunkList, &chunksOrViews);
            }
            for (const auto* chunkOrView : chunksOrViews) {
                if (IsHunkChunk(chunkOrView)) {
                    FillHunkChunkDescriptor(chunkOrView->AsChunk(), req.add_hunk_chunks());
                } else {
                    FillStoreDescriptor(table, chunkOrView, req.add_stores(), &startingRowIndex);
                }
            }

            for (auto [transactionId, lock] : table->DynamicTableLocks()) {
                auto* protoLock = req.add_locks();
                ToProto(protoLock->mutable_transaction_id(), transactionId);
                protoLock->set_timestamp(lock.Timestamp);
            }

            if (!freeze && IsDynamicStoreReadEnabled(table)) {
                CreateAndAttachDynamicStores(tablet, &req);
            }

            if (table->GetInMemoryMode() != EInMemoryMode::None) {
                preloadPendingStoreCount = chunksOrViews.size();
            }

            auto* mountHint = req.mutable_mount_hint();
            ToProto(mountHint->mutable_eden_store_ids(), tablet->EdenStoreIds());

            // TODO(gritukan): Does it make sense for hunk chunk lists?
            i64 cumulativeDataWeight = 0;
            for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
                cumulativeDataWeight += tablet->GetChunkList(contentType)->Statistics().LogicalDataWeight;
            }
            req.set_cumulative_data_weight(cumulativeDataWeight);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Mounting tablet (TableId: %v, TabletId: %v, CellId: %v, ChunkCount: %v, "
                "Atomicity: %v, CommitOrdering: %v, Freeze: %v, UpstreamReplicaId: %v)",
                table->GetId(),
                tablet->GetId(),
                cell->GetId(),
                chunksOrViews.size(),
                table->GetAtomicity(),
                table->GetCommitOrdering(),
                freeze,
                table->GetUpstreamReplicaId());

            hiveManager->PostMessage(mailbox, req);
        }

        {
            TTabletStatistics delta;

            // The latter should be zero, but we observe the formalities.
            delta.PreloadPendingStoreCount = preloadPendingStoreCount -
                tablet->NodeStatistics().preload_pending_store_count();
            table->AccountTabletStatisticsDelta(delta);

            // COMPAT(ifsmirnov)
            if (GetDynamicConfig()->AccumulatePreloadPendingStoreCountCorrectly) {
                cell->GossipStatistics().Local() += delta;
            }

            tablet->NodeStatistics().set_preload_pending_store_count(preloadPendingStoreCount);
        }

        for (auto it : GetIteratorsSortedByKey(tablet->Replicas())) {
            auto* replica = it->first;
            auto& replicaInfo = it->second;
            switch (replica->GetState()) {
                case ETableReplicaState::Enabled:
                case ETableReplicaState::Enabling: {
                    TReqAlterTableReplica req;
                    ToProto(req.mutable_tablet_id(), tablet->GetId());
                    ToProto(req.mutable_replica_id(), replica->GetId());
                    req.set_enabled(true);
                    hiveManager->PostMessage(mailbox, req);

                    if (replica->GetState() == ETableReplicaState::Enabled) {
                        StartReplicaTransition(tablet, replica, &replicaInfo, ETableReplicaState::Enabling);
                    }
                    break;
                }

                case ETableReplicaState::Disabled:
                case ETableReplicaState::Disabling:
                    replicaInfo.SetState(ETableReplicaState::Disabled);
                    break;

                default:
                    YT_ABORT();
            }
        }

        if (mountTimestamp != NullTimestamp) {
            tablet->NodeStatistics().set_unflushed_timestamp(mountTimestamp);
        }
    }

    void DoMountHunkTablet(
        THunkTablet* tablet,
        const TSerializedHunkStorageSettings& serializedSettings,
        TTabletCell* cell)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasHydraContext());

        TReqMountHunkTablet request;
        ToProto(request.mutable_tablet_id(), tablet->GetId());
        request.set_mount_revision(tablet->GetMountRevision());

        FillHunkStorageSettings(&request, serializedSettings);

        auto chunks = EnumerateChunksInChunkTree(tablet->GetChunkList());
        for (auto* chunk : chunks) {
            ToProto(request.add_store_ids(), chunk->GetId());
        }

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        auto* mailbox = hiveManager->GetMailbox(cell->GetId());
        hiveManager->PostMessage(mailbox, request);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Mounting hunk tablet (TabletId: %v, CellId: %v)",
            tablet->GetId(),
            cell->GetId());
    }

    void DoFreezeTablet(TTabletBase* tablet)
    {
        YT_VERIFY(tablet->GetType() == EObjectType::Tablet);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        auto* cell = tablet->GetCell();
        auto state = tablet->GetState();
        YT_VERIFY(state == ETabletState::Mounted ||
            state == ETabletState::FrozenMounting ||
            state == ETabletState::Frozen ||
            state == ETabletState::Freezing);

        if (tablet->GetState() == ETabletState::Mounted) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Freezing tablet (TableId: %v, TabletId: %v, CellId: %v)",
                tablet->GetOwner()->GetId(),
                tablet->GetId(),
                cell->GetId());

            tablet->SetState(ETabletState::Freezing);

            TReqFreezeTablet request;
            ToProto(request.mutable_tablet_id(), tablet->GetId());

            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            hiveManager->PostMessage(mailbox, request);
        }
    }

    void DoUnfreezeTablet(TTabletBase* tablet)
    {
        YT_VERIFY(tablet->GetType() == EObjectType::Tablet);
        auto* table = tablet->As<TTablet>()->GetTable();

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        auto* cell = tablet->GetCell();
        auto state = tablet->GetState();
        YT_VERIFY(state == ETabletState::Mounted ||
            state == ETabletState::Frozen ||
            state == ETabletState::Unfreezing);

        if (tablet->GetState() == ETabletState::Frozen) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Unfreezing tablet (TableId: %v, TabletId: %v, CellId: %v)",
                table->GetId(),
                tablet->GetId(),
                cell->GetId());

            tablet->SetState(ETabletState::Unfreezing);

            TReqUnfreezeTablet request;

            if (IsDynamicStoreReadEnabled(table)) {
                CreateAndAttachDynamicStores(tablet->As<TTablet>(), &request);
            }

            ToProto(request.mutable_tablet_id(), tablet->GetId());

            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            hiveManager->PostMessage(mailbox, request);
        }
    }

    void HydraOnTabletLocked(NProto::TRspLockTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::Tablet);

        auto* tabletBase = FindTablet(tabletId);
        if (!IsObjectAlive(tabletBase)) {
            return;
        }
        auto* tablet = tabletBase->As<TTablet>();
        auto* table = tablet->GetTable();

        auto transactionIds = FromProto<std::vector<TTransactionId>>(response->transaction_ids());

        for (auto transactionId : transactionIds) {
            if (auto it = tablet->UnconfirmedDynamicTableLocks().find(transactionId)) {
                tablet->UnconfirmedDynamicTableLocks().erase(it);
                table->ConfirmDynamicTableLock(transactionId);

                int pendingTabletCount = 0;
                if (auto it = table->DynamicTableLocks().find(transactionId)) {
                    pendingTabletCount = it->second.PendingTabletCount;
                }

                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Confirmed tablet lock (TabletId: %v, TableId: %v, TransactionId: %v, PendingTabletCount: %v)",
                    tabletId,
                    table->GetId(),
                    transactionId,
                    pendingTabletCount);
            }
        }
    }

    void OnTransactionAborted(TTransaction* transaction)
    {
        const auto& hiveManager = Bootstrap_->GetHiveManager();

        for (auto tableIt : GetSortedIterators(
            transaction->LockedDynamicTables(),
            TObjectIdComparer()))
        {
            auto* table = *tableIt;
            if (!IsObjectAlive(table)) {
                continue;
            }

            for (auto* tablet : table->Tablets()) {
                if (tablet->GetState() == ETabletState::Unmounted) {
                    continue;
                }

                tablet->As<TTablet>()->UnconfirmedDynamicTableLocks().erase(transaction->GetId());

                auto* cell = tablet->GetCell();
                auto* mailbox = hiveManager->GetMailbox(cell->GetId());
                TReqUnlockTablet req;
                ToProto(req.mutable_tablet_id(), tablet->GetId());
                ToProto(req.mutable_transaction_id(), transaction->GetId());
                req.set_mount_revision(tablet->GetMountRevision());
                // Aborted bulk insert should not conflict with concurrent tablet transactions.
                req.set_commit_timestamp(static_cast<i64>(MinTimestamp));

                hiveManager->PostMessage(mailbox, req);
            }

            table->RemoveDynamicTableLock(transaction->GetId());
        }

        transaction->LockedDynamicTables().clear();
    }

    TError CheckAllDynamicStoresFlushed(TTablet* tablet)
    {
        auto makeError = [&] (const TDynamicStore* dynamicStore) {
            const auto* originalTablet = dynamicStore->GetTablet();
            const TString& originalTablePath = IsObjectAlive(originalTablet) && IsObjectAlive(originalTablet->GetTable())
                ? originalTablet->GetTable()->GetMountPath()
                : "";
            return TError("Cannot restore table from backup since "
                "dynamic store %v in tablet %v is not flushed",
                dynamicStore->GetId(),
                tablet->GetId())
                << TErrorAttribute("original_table_path", originalTablePath)
                << TErrorAttribute("table_id", tablet->GetTable()->GetId());
        };

        auto children = EnumerateStoresInChunkTree(tablet->GetChunkList());

        for (const auto* child : children) {
            if (child->GetType() == EObjectType::ChunkView) {
                const auto* underlyingTree = child->AsChunkView()->GetUnderlyingTree();
                if (IsDynamicTabletStoreType(underlyingTree->GetType()) &&
                    !underlyingTree->AsDynamicStore()->IsFlushed())
                {
                    return makeError(underlyingTree->AsDynamicStore());
                }
            } else if (IsDynamicTabletStoreType(child->GetType())) {
                const auto* dynamicStore = child->AsDynamicStore();
                if (!dynamicStore->IsFlushed()) {
                    return makeError(dynamicStore);
                }
            }
        }

        return {};
    }

    TError ApplyRowIndexBackupCutoff(TTablet* tablet)
    {
        auto* chunkList = tablet->GetChunkList();
        YT_VERIFY(chunkList->GetKind() == EChunkListKind::OrderedDynamicTablet);

        const auto& descriptor = *tablet->BackupCutoffDescriptor();

        if (tablet->GetTrimmedRowCount() > descriptor.CutoffRowIndex) {
            return TError(
                "Cannot backup ordered tablet %v since it is trimmed "
                "beyond cutoff row index",
                tablet->GetId())
                << TErrorAttribute("tablet_id", tablet->GetId())
                << TErrorAttribute("table_id", tablet->GetTable()->GetId())
                << TErrorAttribute("trimmed_row_count", tablet->GetTrimmedRowCount())
                << TErrorAttribute("cutoff_row_index", descriptor.CutoffRowIndex);
        }

        if (!descriptor.NextDynamicStoreId) {
            // Tablet was not mounted when the descriptor was generated by the node.
            // Either it is fully flushed, then this check succeeds; or it is is not
            // (so the descriptor was generated during freeze/unmount workflow) and we
            // have to abort the backup if some of the stores are not flushed.
            for (const auto* child : tablet->GetChunkList()->Children()) {
                if (child && IsDynamicTabletStoreType(child->GetType())) {
                    const auto* dynamicStore = child->AsDynamicStore();
                    if (!dynamicStore->IsFlushed()) {
                        return TError(
                            "Cannot backup ordered tablet %v since it is not fully flushed "
                            "and its origin was not mounted during the backup",
                            tablet->GetId())
                            << TErrorAttribute("tablet_id", tablet->GetId())
                            << TErrorAttribute("table_id", tablet->GetTable()->GetId());
                    }
                }
            }
        }

        // CopyChunkListsIfShared must be done before cutoff chunk index is calculated
        // because it omits trimmed chunks and thus may shift chunk indexes.
        CopyChunkListsIfShared(tablet->GetTable(), tablet->GetIndex(), tablet->GetIndex());
        chunkList = tablet->GetChunkList();

        int cutoffChildIndex = 0;
        const auto& children = chunkList->Children();
        const auto& statistics = chunkList->CumulativeStatistics();

        auto wrapInternalErrorAndLog = [&] (TError innerError) {
            innerError = innerError
                << TErrorAttribute("table_id", tablet->GetTable()->GetId())
                << TErrorAttribute("tablet_id", tablet->GetId())
                << TErrorAttribute("cutoff_row_index", descriptor.CutoffRowIndex)
                << TErrorAttribute("next_dynamic_store_id", descriptor.NextDynamicStoreId)
                << TErrorAttribute("cutoff_child_index", cutoffChildIndex);
            auto error = TError("Cannot backup ordered tablet due to an internal error")
                << innerError;
            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), error, "Failed to perform backup cutoff");
            return error;
        };

        bool hitDynamicStore = false;

        while (cutoffChildIndex < ssize(children)) {
            i64 cumulativeRowCount = statistics.GetPreviousSum(cutoffChildIndex).RowCount;
            const auto* child = children[cutoffChildIndex];

            if (child && child->GetId() == descriptor.NextDynamicStoreId) {
                if (cumulativeRowCount > descriptor.CutoffRowIndex) {
                    auto error = TError("Cumulative row count at the cutoff dynamic store "
                        "is greater than expected")
                        << TErrorAttribute("cumulative_row_count", cumulativeRowCount);
                    return wrapInternalErrorAndLog(error);
                }

                hitDynamicStore = true;
                break;
            }

            if (cumulativeRowCount > descriptor.CutoffRowIndex) {
                auto error = TError("Cumulative row count exceeded cutoff row index")
                    << TErrorAttribute("cumulative_row_count", cumulativeRowCount);
                return wrapInternalErrorAndLog(error);

            }

            if (cumulativeRowCount == descriptor.CutoffRowIndex) {
                break;
            }

            ++cutoffChildIndex;
        }

        if (statistics.GetPreviousSum(cutoffChildIndex).RowCount != descriptor.CutoffRowIndex &&
            !hitDynamicStore)
        {
            auto error = TError("Row count at final cutoff child index does not match cutoff row index")
                << TErrorAttribute("cumulative_row_count", statistics.GetPreviousSum(cutoffChildIndex).RowCount);
            return wrapInternalErrorAndLog(error);
        }

        if (cutoffChildIndex == ssize(chunkList->Children())) {
            return {};
        }

        auto oldStatistics = tablet->GetTabletStatistics();
        auto* table = tablet->GetTable();
        table->DiscountTabletStatistics(oldStatistics);

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        std::vector<TChunkTree*> childrenToDetach(
            chunkList->Children().data() + cutoffChildIndex,
            chunkList->Children().data() + ssize(children));
        chunkManager->DetachFromChunkList(
            chunkList,
            childrenToDetach,
            EChunkDetachPolicy::OrderedTabletSuffix);

        auto newStatistics = tablet->GetTabletStatistics();
        table->AccountTabletStatistics(newStatistics);

        return {};
    }

    void ApplyDynamicStoreListBackupCutoff(TTablet* tablet)
    {
        auto* chunkList = tablet->GetChunkList();
        YT_VERIFY(chunkList->GetKind() == EChunkListKind::SortedDynamicTablet);

        const auto& descriptor = *tablet->BackupCutoffDescriptor();

        std::vector<TChunkTree*> storesToDetach;
        // NB: cannot use tablet->DynamicStores() since dynamic stores in the chunk list
        // in fact belong to the other tablet and are not linked with this one.
        for (auto* child : EnumerateStoresInChunkTree(tablet->GetChunkList())) {
            if (child->GetType() != EObjectType::SortedDynamicTabletStore) {
                continue;
            }
            if (!descriptor.DynamicStoreIdsToKeep.contains(child->GetId())) {
                storesToDetach.push_back(child);
            }
        }

        if (storesToDetach.empty()) {
            return;
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Detaching unneeded dynamic stores from tablet after backup "
            "(TabletId: %v, DynamicStoreIds: %v)",
            tablet->GetId(),
            MakeFormattableView(
                storesToDetach,
                TObjectIdFormatter{}));

        CopyChunkListsIfShared(tablet->GetTable(), tablet->GetIndex(), tablet->GetIndex());
        chunkList = tablet->GetChunkList();

        auto oldStatistics = tablet->GetTabletStatistics();
        auto* table = tablet->GetTable();
        table->DiscountTabletStatistics(oldStatistics);

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        chunkManager->DetachFromChunkList(
            chunkList,
            storesToDetach,
            EChunkDetachPolicy::SortedTablet);

        auto newStatistics = tablet->GetTabletStatistics();
        table->AccountTabletStatistics(newStatistics);
    }

    void DoRemount(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        if (IsTableType(table->GetType())) {
            return DoRemountTable(
                table->As<TTableNode>(),
                firstTabletIndex,
                lastTabletIndex);
        } else {
            YT_ABORT();
        }
    }

    void DoRemountTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        auto tableSettings = GetTableSettings(table);
        auto serializedTableSettings = SerializeTableSettings(tableSettings);

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            auto* cell = tablet->GetCell();
            auto state = tablet->GetState();

            if (state != ETabletState::Unmounted) {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Remounting tablet (TableId: %v, TabletId: %v, CellId: %v)",
                    table->GetId(),
                    tablet->GetId(),
                    cell->GetId());

                YT_VERIFY(tablet->GetInMemoryMode() == tableSettings.MountConfig->InMemoryMode);

                const auto& hiveManager = Bootstrap_->GetHiveManager();

                TReqRemountTablet request;
                ToProto(request.mutable_tablet_id(), tablet->GetId());
                FillTableSettings(&request, serializedTableSettings);

                auto* mailbox = hiveManager->GetMailbox(cell->GetId());
                hiveManager->PostMessage(mailbox, request);
            }
        }
    }

    int DoReshard(
        TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount,
        const std::vector<TLegacyOwningKey>& pivotKeys)
    {
        if (IsTableType(table->GetType())) {
            return DoReshardTable(
                table->As<TTableNode>(),
                firstTabletIndex,
                lastTabletIndex,
                newTabletCount,
                pivotKeys);
        } else if (table->GetType() == EObjectType::HunkStorage) {
            return DoReshardHunkStorage(
                table->As<THunkStorageNode>(),
                firstTabletIndex,
                lastTabletIndex,
                newTabletCount);
        } else {
            YT_ABORT();
        }
    }

    int DoReshardTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount,
        const std::vector<TLegacyOwningKey>& pivotKeys)
    {
        if (!pivotKeys.empty() || !table->IsPhysicallySorted()) {
            ReshardTableImpl(
                table,
                firstTabletIndex,
                lastTabletIndex,
                newTabletCount,
                pivotKeys);
            return newTabletCount;
        } else {
            auto newPivotKeys = CalculatePivotKeys(table, firstTabletIndex, lastTabletIndex, newTabletCount);
            newTabletCount = std::ssize(newPivotKeys);
            ReshardTableImpl(
                table,
                firstTabletIndex,
                lastTabletIndex,
                newTabletCount,
                newPivotKeys);
            return newTabletCount;
        }
    }

    int DoReshardHunkStorage(
        THunkStorageNode* hunkStorage,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(HasHydraContext());
        YT_VERIFY(hunkStorage->IsTrunk());
        YT_VERIFY(!hunkStorage->IsExternal());

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto& objectManager = Bootstrap_->GetObjectManager();

        auto& tablets = hunkStorage->MutableTablets();

        ParseTabletRange(hunkStorage, &firstTabletIndex, &lastTabletIndex);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Resharding hunk storage (HunkStorageId: %v, FirstTabletIndex: %v, LastTabletIndex: %v, TabletCount: %v)",
            hunkStorage->GetId(),
            firstTabletIndex,
            lastTabletIndex,
            newTabletCount);

        // Create new tablets.
        std::vector<THunkTablet*> newTablets;
        newTablets.reserve(newTabletCount);
        for (int index = 0; index < newTabletCount; ++index) {
            auto* newTablet = CreateTablet(hunkStorage, EObjectType::HunkTablet)->As<THunkTablet>();
            newTablets.push_back(newTablet);
        }

        // Drop old tablets.
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = tablets[index];
            hunkStorage->DiscountTabletStatistics(tablet->GetTabletStatistics());
            tablet->SetOwner(nullptr);

            objectManager->UnrefObject(tablet);
        }

        // Replace old tablets with new.
        tablets.erase(tablets.begin() + firstTabletIndex, tablets.begin() + (lastTabletIndex + 1));
        tablets.insert(tablets.begin() + firstTabletIndex, newTablets.begin(), newTablets.end());

        // Update tablet indices.
        for (int index = 0; index < std::ssize(tablets); ++index) {
            auto* tablet = tablets[index];
            tablet->SetIndex(index);
        }

        // Update chunk lists.
        auto* oldRootChunkList = hunkStorage->GetChunkList();
        auto* newRootChunkList = chunkManager->CreateChunkList(EChunkListKind::HunkStorageRoot);
        std::vector<TChunkTree*> newTabletChunkLists;
        newTabletChunkLists.reserve(newTabletCount);
        for (int index = 0; index < newTabletCount; ++index) {
            auto* newTabletChunkList = chunkManager->CreateChunkList(EChunkListKind::HunkTablet);
            newTabletChunkLists.push_back(newTabletChunkList);
        }

        // NB: When resharding during creation root chunk list is empty.
        if (oldRootChunkList) {
            const auto& oldTabletChunkLists = oldRootChunkList->Children();
            chunkManager->AttachToChunkList(
                newRootChunkList,
                oldTabletChunkLists.data(),
                oldTabletChunkLists.data() + firstTabletIndex);
        } else {
            YT_VERIFY(firstTabletIndex == 0);
        }

        chunkManager->AttachToChunkList(newRootChunkList, newTabletChunkLists);

        // NB: When resharding during creation root chunk list is empty.
        if (oldRootChunkList) {
            const auto& oldTabletChunkLists = oldRootChunkList->Children();
            chunkManager->AttachToChunkList(
                newRootChunkList,
                oldTabletChunkLists.data() + lastTabletIndex + 1,
                oldTabletChunkLists.data() + oldTabletChunkLists.size());
        } else {
            YT_VERIFY(firstTabletIndex == 0);
            YT_VERIFY(tablets.empty());
        }

        if (oldRootChunkList) {
            oldRootChunkList->RemoveOwningNode(hunkStorage);
        }
        hunkStorage->SetChunkList(newRootChunkList);
        newRootChunkList->AddOwningNode(hunkStorage);

        // Account new tablet statistics.
        for (auto* newTablet : newTablets) {
            hunkStorage->AccountTabletStatistics(newTablet->GetTabletStatistics());
        }

        return newTabletCount;
    }

    // If there are several otherwise identical chunk views with adjacent read ranges
    // we merge them into one chunk view with the joint range.
    std::vector<TChunkTree*> MergeChunkViewRanges(
        std::vector<TChunkView*> chunkViews,
        const TLegacyOwningKey& lowerPivot,
        const TLegacyOwningKey& upperPivot)
    {
        auto mergeResults = MergeAdjacentChunkViewRanges(std::move(chunkViews));
        std::vector<TChunkTree*> result;

        const auto& chunkManager = Bootstrap_->GetChunkManager();

        for (const auto& mergeResult : mergeResults) {
            auto* firstChunkView = mergeResult.FirstChunkView;
            auto* lastChunkView = mergeResult.LastChunkView;
            const auto& lowerLimit = firstChunkView->ReadRange().LowerLimit().HasLegacyKey()
                ? firstChunkView->ReadRange().LowerLimit().GetLegacyKey()
                : EmptyKey();
            const auto& upperLimit = lastChunkView->ReadRange().UpperLimit().HasLegacyKey()
                ? lastChunkView->ReadRange().UpperLimit().GetLegacyKey()
                : MaxKey();

            if (firstChunkView == lastChunkView &&
                lowerPivot <= lowerLimit &&
                upperLimit <= upperPivot)
            {
                result.push_back(firstChunkView);
                continue;
            } else {
                NChunkClient::TLegacyReadRange readRange;
                const auto& adjustedLower = std::max(lowerLimit, lowerPivot);
                const auto& adjustedUpper = std::min(upperLimit, upperPivot);
                YT_VERIFY(adjustedLower < adjustedUpper);
                if (adjustedLower != EmptyKey()) {
                    readRange.LowerLimit().SetLegacyKey(adjustedLower);
                }
                if (adjustedUpper != MaxKey()) {
                    readRange.UpperLimit().SetLegacyKey(adjustedUpper);
                }
                result.push_back(chunkManager->CloneChunkView(firstChunkView, readRange));
            }
        }

        return result;
    }

    void ReshardTableImpl(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        int newTabletCount,
        const std::vector<TLegacyOwningKey>& pivotKeys)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YT_VERIFY(table->IsTrunk());
        YT_VERIFY(!table->IsExternal());

        const auto& objectManager = Bootstrap_->GetObjectManager();
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex);

        auto resourceUsageBefore = table->GetTabletResourceUsage();

        auto& tablets = table->MutableTablets();
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            YT_VERIFY(tablets.size() == table->GetChunkList(contentType)->Children().size());
        }

        int oldTabletCount = lastTabletIndex - firstTabletIndex + 1;

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Resharding table (TableId: %v, FirstTabletIndex: %v, LastTabletIndex: %v, "
            "TabletCount %v, PivotKeys: %v)",
            table->GetId(),
            firstTabletIndex,
            lastTabletIndex,
            newTabletCount,
            pivotKeys);

        // Calculate retained timestamp for removed tablets.
        auto retainedTimestamp = MinTimestamp;
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            retainedTimestamp = std::max(retainedTimestamp, tablets[index]->As<TTablet>()->GetRetainedTimestamp());
        }

        // Save eden stores of removed tablets.
        // NB. Since new chunk views may be created over existing chunks, we mark underlying
        // chunks themselves as eden. It gives different result only in rare cases when a chunk
        // under a chunk view was in eden in some tablet but not in the adjacent tablet.
        THashSet<TStoreId> oldEdenStoreIds;
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            for (auto storeId : tablets[index]->As<TTablet>()->EdenStoreIds()) {
                if (auto chunkView = chunkManager->FindChunkView(storeId)) {
                    oldEdenStoreIds.insert(chunkView->GetUnderlyingTree()->GetId());
                } else {
                    oldEdenStoreIds.insert(storeId);
                }
            }
        }

        // Create new tablets.
        std::vector<TTablet*> newTablets;
        for (int index = 0; index < newTabletCount; ++index) {
            auto* newTablet = CreateTablet(table, EObjectType::Tablet)->As<TTablet>();
            auto* oldTablet = index < oldTabletCount ? tablets[index + firstTabletIndex]->As<TTablet>() : nullptr;
            if (table->IsSorted()) {
                newTablet->SetPivotKey(pivotKeys[index]);
            } else if (oldTablet) {
                newTablet->SetTrimmedRowCount(oldTablet->GetTrimmedRowCount());
            }
            newTablet->SetRetainedTimestamp(retainedTimestamp);
            newTablets.push_back(newTablet);

            if (table->IsReplicated()) {
                const auto* replicatedTable = table->As<TReplicatedTableNode>();
                for (auto* replica : GetValuesSortedByKey(replicatedTable->Replicas())) {
                    YT_VERIFY(newTablet->Replicas().emplace(replica, TTableReplicaInfo()).second);
                }
            }
        }

        // Copy replication progress.
        {
            std::vector<TReplicationProgress> progresses;
            std::vector<TLegacyKey> pivotKeys;
            std::vector<TLegacyOwningKey> buffer;
            bool nonEmpty = false;

            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* tablet = tablets[index]->As<TTablet>();
                if (!tablet->ReplicationProgress().Segments.empty()) {
                    nonEmpty = true;
                }
                progresses.push_back(std::move(tablet->ReplicationProgress()));
                pivotKeys.push_back(GetTabletReplicationProgressPivotKey(tablet, index, &buffer));
            }

            if (nonEmpty) {
                auto upperKey = lastTabletIndex + 1 < std::ssize(tablets)
                    ? tablets[lastTabletIndex + 1]->As<TTablet>()->GetPivotKey().Get()
                    : MaxKey().Get();
                auto progress = NChaosClient::GatherReplicationProgress(
                    std::move(progresses),
                    pivotKeys,
                    upperKey);
                pivotKeys.clear();
                for (int index = 0; index < std::ssize(newTablets); ++index) {
                    auto* tablet = newTablets[index];
                    pivotKeys.push_back(GetTabletReplicationProgressPivotKey(tablet, firstTabletIndex + index, &buffer));
                }

                auto newProgresses = NChaosClient::ScatterReplicationProgress(
                    std::move(progress),
                    pivotKeys,
                    upperKey);
                for (int index = 0; index < std::ssize(newTablets); ++index) {
                    auto* tablet = newTablets[index];
                    tablet->ReplicationProgress() = std::move(newProgresses[index]);
                }
            }
        }

        std::vector<TLegacyOwningKey> oldPivotKeys;

        // Drop old tablets.
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = tablets[index]->As<TTablet>();
            if (table->IsPhysicallySorted()) {
                oldPivotKeys.push_back(tablet->GetPivotKey());
            }
            table->DiscountTabletStatistics(tablet->GetTabletStatistics());
            tablet->SetOwner(nullptr);
            objectManager->UnrefObject(tablet);
        }

        if (table->IsPhysicallySorted()) {
            if (lastTabletIndex + 1 < std::ssize(tablets)) {
                oldPivotKeys.push_back(tablets[lastTabletIndex + 1]->As<TTablet>()->GetPivotKey());
            } else {
                oldPivotKeys.push_back(MaxKey());
            }
        }

        // NB: Evaluation order is important here, consider the case lastTabletIndex == -1.
        tablets.erase(tablets.begin() + firstTabletIndex, tablets.begin() + (lastTabletIndex + 1));
        tablets.insert(tablets.begin() + firstTabletIndex, newTablets.begin(), newTablets.end());
        // Update all indexes.
        for (int index = 0; index < static_cast<int>(tablets.size()); ++index) {
            auto* tablet = tablets[index];
            tablet->SetIndex(index);
        }

        // Copy chunk tree if somebody holds a reference.
        CopyChunkListsIfShared(table, firstTabletIndex, lastTabletIndex);

        TChunkLists oldRootChunkLists;
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            oldRootChunkLists[contentType] = table->GetChunkList(contentType);
        }

        TEnumIndexedVector<EChunkListContentType, std::vector<TChunkTree*>> newTabletChunkLists;
        for (auto& tabletChunkLists : newTabletChunkLists) {
            tabletChunkLists.reserve(newTabletCount);
        }

        TChunkLists newRootChunkLists;
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            auto* chunkList = chunkManager->CreateChunkList(oldRootChunkLists[contentType]->GetKind());
            newRootChunkLists[contentType] = chunkList;
        }

        // Initialize new tablet chunk lists.
        if (table->IsPhysicallySorted()) {
            // This excludes hunk chunks.
            std::vector<TChunkTree*> chunksOrViews;

            // Chunk views that were created to fit chunks into old tablet range
            // and may later become useless after MergeChunkViewRanges.
            // We ref them after creation and unref at the end so they are
            // properly destroyed.
            std::vector<TChunkView*> temporarilyReferencedChunkViews;

            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* mainTabletChunkList = oldRootChunkLists[EChunkListContentType::Main]->Children()[index]->AsChunkList();
                auto tabletStores = EnumerateStoresInChunkTree(mainTabletChunkList);

                const auto& lowerPivot = oldPivotKeys[index - firstTabletIndex];
                const auto& upperPivot = oldPivotKeys[index - firstTabletIndex + 1];

                for (auto* chunkTree : tabletStores) {
                    if (chunkTree->GetType() == EObjectType::ChunkView) {
                        auto* chunkView = chunkTree->AsChunkView();
                        auto readRange = chunkView->GetCompleteReadRange();

                        // Check if chunk view fits into the old tablet completely.
                        // This might not be the case if the chunk view comes from bulk insert and has no read range.
                        if (readRange.LowerLimit().GetLegacyKey() < lowerPivot ||
                            upperPivot < readRange.UpperLimit().GetLegacyKey())
                        {
                            if (!chunkView->GetTransactionId()) {
                                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Chunk view without transaction id is not fully inside its tablet "
                                    "(ChunkViewId: %v, UnderlyingTreeId: %v, "
                                    "EffectiveLowerLimit: %v, EffectiveUpperLimit: %v, "
                                    "PivotKey: %v, NextPivotKey: %v)",
                                    chunkView->GetId(),
                                    chunkView->GetUnderlyingTree()->GetId(),
                                    readRange.LowerLimit().GetLegacyKey(),
                                    readRange.UpperLimit().GetLegacyKey(),
                                    lowerPivot,
                                    upperPivot);
                            }

                            NChunkClient::TLegacyReadRange newReadRange;
                            if (readRange.LowerLimit().GetLegacyKey() < lowerPivot) {
                                newReadRange.LowerLimit().SetLegacyKey(lowerPivot);
                            }
                            if (upperPivot < readRange.UpperLimit().GetLegacyKey()) {
                                newReadRange.UpperLimit().SetLegacyKey(upperPivot);
                            }

                            auto* newChunkView = chunkManager->CreateChunkView(
                                chunkView,
                                TChunkViewModifier().WithReadRange(newReadRange));
                            objectManager->RefObject(newChunkView);
                            temporarilyReferencedChunkViews.push_back(newChunkView);

                            chunkTree = newChunkView;
                        }
                    }

                    chunksOrViews.push_back(chunkTree);
                }
            }

            SortUnique(chunksOrViews, TObjectIdComparer());

            auto keyColumnCount = table->GetSchema()->AsTableSchema()->GetKeyColumnCount();

            // Create new tablet chunk lists.
            for (int index = 0; index < newTabletCount; ++index) {
                auto* mainTabletChunkList = chunkManager->CreateChunkList(EChunkListKind::SortedDynamicTablet);
                mainTabletChunkList->SetPivotKey(pivotKeys[index]);
                newTabletChunkLists[EChunkListContentType::Main].push_back(mainTabletChunkList);

                auto* hunkTabletChunkList = chunkManager->CreateChunkList(EChunkListKind::Hunk);
                newTabletChunkLists[EChunkListContentType::Hunk].push_back(hunkTabletChunkList);
            }

            // Move chunks or views from the resharded tablets to appropriate chunk lists.
            std::vector<std::vector<TChunkView*>> newTabletChildrenToBeMerged(newTablets.size());
            std::vector<std::vector<TChunkTree*>> newTabletHunkChunks(newTablets.size());
            std::vector<std::vector<TStoreId>> newEdenStoreIds(newTablets.size());

            for (auto* chunkOrView : chunksOrViews) {
                NChunkClient::TLegacyReadRange readRange;
                TChunk* chunk;
                if (chunkOrView->GetType() == EObjectType::ChunkView) {
                    auto* chunkView = chunkOrView->AsChunkView();
                    chunk = chunkView->GetUnderlyingTree()->AsChunk();
                    readRange = chunkView->GetCompleteReadRange();
                } else if (IsPhysicalChunkType(chunkOrView->GetType())) {
                    chunk = chunkOrView->AsChunk();
                    auto keyPair = GetChunkBoundaryKeys(chunk->ChunkMeta()->GetExtension<NTableClient::NProto::TBoundaryKeysExt>(), keyColumnCount);
                    readRange = {
                        NChunkClient::TLegacyReadLimit(keyPair.first),
                        NChunkClient::TLegacyReadLimit(GetKeySuccessor(keyPair.second))
                    };
                } else {
                    YT_ABORT();
                }

                auto referencedHunkChunks = GetReferencedHunkChunks(chunk);

                auto tabletsRange = GetIntersectingTablets(newTablets, readRange);
                for (auto it = tabletsRange.first; it != tabletsRange.second; ++it) {
                    auto* tablet = *it;
                    const auto& lowerPivot = tablet->GetPivotKey();
                    const auto& upperPivot = tablet->GetIndex() == std::ssize(tablets) - 1
                        ? MaxKey()
                        : tablets[tablet->GetIndex() + 1]->As<TTablet>()->GetPivotKey();
                    int relativeIndex = it - newTablets.begin();

                    newTabletHunkChunks[relativeIndex].insert(
                        newTabletHunkChunks[relativeIndex].end(),
                        referencedHunkChunks.begin(),
                        referencedHunkChunks.end());

                    // Chunks or chunk views created directly from chunks may be attached to tablets as is.
                    // On the other hand, old chunk views may link to the same chunk and have adjacent ranges,
                    // so we handle them separately.
                    if (chunkOrView->GetType() == EObjectType::ChunkView) {
                        // Read range given by tablet's pivot keys will be enforced later.
                        newTabletChildrenToBeMerged[relativeIndex].push_back(chunkOrView->AsChunkView());
                    } else if (IsPhysicalChunkType(chunkOrView->GetType())) {
                        if (lowerPivot <= readRange.LowerLimit().GetLegacyKey() &&
                            readRange.UpperLimit().GetLegacyKey() <= upperPivot)
                        {
                            // Chunk fits into the tablet.
                            chunkManager->AttachToChunkList(
                                newTabletChunkLists[EChunkListContentType::Main][relativeIndex]->AsChunkList(),
                                chunk);
                            if (oldEdenStoreIds.contains(chunk->GetId())) {
                                newEdenStoreIds[relativeIndex].push_back(chunk->GetId());
                            }
                        } else {
                            // Chunk does not fit into the tablet, create chunk view.
                            NChunkClient::TLegacyReadRange newReadRange;
                            if (readRange.LowerLimit().GetLegacyKey() < lowerPivot) {
                                newReadRange.LowerLimit().SetLegacyKey(lowerPivot);
                            }
                            if (upperPivot < readRange.UpperLimit().GetLegacyKey()) {
                                newReadRange.UpperLimit().SetLegacyKey(upperPivot);
                            }
                            auto* newChunkView = chunkManager->CreateChunkView(
                                chunk,
                                TChunkViewModifier().WithReadRange(newReadRange));
                            chunkManager->AttachToChunkList(
                                newTabletChunkLists[EChunkListContentType::Main][relativeIndex]->AsChunkList(),
                                newChunkView);
                            if (oldEdenStoreIds.contains(chunk->GetId())) {
                                newEdenStoreIds[relativeIndex].push_back(newChunkView->GetId());
                            }
                        }
                    } else {
                        YT_ABORT();
                    }
                }
            }

            for (int relativeIndex = 0; relativeIndex < std::ssize(newTablets); ++relativeIndex) {
                auto* tablet = newTablets[relativeIndex];
                const auto& lowerPivot = tablet->GetPivotKey();
                const auto& upperPivot = tablet->GetIndex() == std::ssize(tablets) - 1
                    ? MaxKey()
                    : tablets[tablet->GetIndex() + 1]->As<TTablet>()->GetPivotKey();

                std::vector<TChunkTree*> mergedChunkViews;
                try {
                    mergedChunkViews = MergeChunkViewRanges(newTabletChildrenToBeMerged[relativeIndex], lowerPivot, upperPivot);
                } catch (const std::exception& ex) {
                    YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), ex, "Failed to merge chunk view ranges");
                }

                auto* newTabletChunkList = newTabletChunkLists[EChunkListContentType::Main][relativeIndex]->AsChunkList();
                chunkManager->AttachToChunkList(newTabletChunkList, mergedChunkViews);

                std::vector<TChunkTree*> hunkChunks;
                for (auto* chunkOrView : mergedChunkViews) {
                    if (oldEdenStoreIds.contains(chunkOrView->AsChunkView()->GetUnderlyingTree()->GetId())) {
                        newEdenStoreIds[relativeIndex].push_back(chunkOrView->GetId());
                    }
                }
            }

            for (int relativeIndex = 0; relativeIndex < std::ssize(newTablets); ++relativeIndex) {
                SetTabletEdenStoreIds(newTablets[relativeIndex], newEdenStoreIds[relativeIndex]);

                if (!newTabletHunkChunks[relativeIndex].empty()) {
                    SortUnique(newTabletHunkChunks[relativeIndex], TObjectIdComparer());
                    auto* hunkChunkList = newTabletChunkLists[EChunkListContentType::Hunk][relativeIndex]->AsChunkList();
                    chunkManager->AttachToChunkList(hunkChunkList, newTabletHunkChunks[relativeIndex]);
                }
            }

            for (auto* chunkView : temporarilyReferencedChunkViews) {
                if (objectManager->UnrefObject(chunkView) == 0) {
                    YT_LOG_DEBUG("Temporarily referenced chunk view dropped during reshard (ChunkViewId: %v)",
                        chunkView->GetId());
                }
            }
        } else {
            // If the number of tablets increases, just leave the new trailing ones empty.
            // If the number of tablets decreases, merge the original trailing ones.
            auto attachChunksToChunkList = [&] (TChunkList* chunkList, int firstTabletIndex, int lastTabletIndex) {
                std::vector<TChunk*> chunks;
                for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                    auto* mainTabletChunkList = oldRootChunkLists[EChunkListContentType::Main]->Children()[index]->AsChunkList();
                    EnumerateChunksInChunkTree(mainTabletChunkList, &chunks);
                }
                for (auto* chunk : chunks) {
                    chunkManager->AttachToChunkList(chunkList, chunk);
                }
            };
            for (int index = firstTabletIndex; index < firstTabletIndex + std::min(oldTabletCount, newTabletCount); ++index) {
                auto* oldChunkList = oldRootChunkLists[EChunkListContentType::Main]->Children()[index]->AsChunkList();
                auto* newChunkList = chunkManager->CloneTabletChunkList(oldChunkList);
                newTabletChunkLists[EChunkListContentType::Main].push_back(newChunkList);

                auto* newHunkChunkList = chunkManager->CreateChunkList(EChunkListKind::Hunk);
                newTabletChunkLists[EChunkListContentType::Hunk].push_back(newHunkChunkList);
            }
            if (oldTabletCount > newTabletCount) {
                auto* chunkList = newTabletChunkLists[EChunkListContentType::Main][newTabletCount - 1]->AsChunkList();
                attachChunksToChunkList(chunkList, firstTabletIndex + newTabletCount, lastTabletIndex);
            } else {
                for (int index = oldTabletCount; index < newTabletCount; ++index) {
                    newTabletChunkLists[EChunkListContentType::Main].push_back(chunkManager->CreateChunkList(EChunkListKind::OrderedDynamicTablet));
                    newTabletChunkLists[EChunkListContentType::Hunk].push_back(chunkManager->CreateChunkList(EChunkListKind::Hunk));
                }
            }

            for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
                YT_VERIFY(std::ssize(newTabletChunkLists[contentType]) == newTabletCount);
            }
        }

        // Update tablet chunk lists.
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            const auto& oldTabletChunkLists = oldRootChunkLists[contentType]->Children();
            chunkManager->AttachToChunkList(
                newRootChunkLists[contentType],
                oldTabletChunkLists.data(),
                oldTabletChunkLists.data() + firstTabletIndex);
            chunkManager->AttachToChunkList(
                newRootChunkLists[contentType],
                newTabletChunkLists[contentType]);
            chunkManager->AttachToChunkList(
                newRootChunkLists[contentType],
                oldTabletChunkLists.data() + lastTabletIndex + 1,
                oldTabletChunkLists.data() + oldTabletChunkLists.size());
        }

        // Replace root chunk list.
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            table->SetChunkList(contentType, newRootChunkLists[contentType]);
            newRootChunkLists[contentType]->AddOwningNode(table);
            oldRootChunkLists[contentType]->RemoveOwningNode(table);
        }

        // Account new tablet statistics.
        for (auto* newTablet : newTablets) {
            table->AccountTabletStatistics(newTablet->GetTabletStatistics());
        }

        // TODO(savrus) Looks like this is unnecessary. Need to check.
        table->SnapshotStatistics() = {};
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            table->SnapshotStatistics() += table->GetChunkList(contentType)->Statistics().ToDataStatistics();
        }

        auto resourceUsageDelta = table->GetTabletResourceUsage() - resourceUsageBefore;
        UpdateResourceUsage(table, resourceUsageDelta);

        table->RecomputeTabletMasterMemoryUsage();
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->UpdateMasterMemoryUsage(table);
    }

    std::vector<TChunk*> GetReferencedHunkChunks(TChunk* storeChunk)
    {
        auto hunkRefsExt = storeChunk->ChunkMeta()->FindExtension<THunkChunkRefsExt>();
        if (!hunkRefsExt) {
            return {};
        }

        std::vector<TChunk*> hunkChunks;

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        for (const auto& protoRef : hunkRefsExt->refs()) {
            auto hunkChunkId = FromProto<TChunkId>(protoRef.chunk_id());
            auto* hunkChunk = chunkManager->FindChunk(hunkChunkId);
            if (!IsObjectAlive(hunkChunk)) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Store references a non-existing hunk chunk (StoreId: %v, HunkChunkId: %v)",
                    storeChunk->GetId(),
                    hunkChunkId);
                continue;
            }
            hunkChunks.push_back(hunkChunk);
        }

        return hunkChunks;
    }

    void SetSyncTabletActionsKeepalive(const std::vector<TTabletActionId>& actionIds)
    {
        for (auto actionId : actionIds) {
            auto* action = GetTabletAction(actionId);
            action->SetExpirationTimeout(DefaultSyncTabletActionKeepalivePeriod);
        }
    }

    const TDynamicTabletManagerConfigPtr& GetDynamicConfig() const
    {
        return Bootstrap_->GetConfigManager()->GetConfig()->TabletManager;
    }

    void OnDynamicConfigChanged(TDynamicClusterConfigPtr oldConfig = nullptr)
    {
        const auto& config = GetDynamicConfig();

        {
            const auto& gossipConfig = config->MulticellGossip;

            if (TabletCellStatisticsGossipExecutor_) {
                TabletCellStatisticsGossipExecutor_->SetPeriod(gossipConfig->TabletCellStatisticsGossipPeriod);
            }
            if (BundleResourceUsageGossipExecutor_) {
                BundleResourceUsageGossipExecutor_->SetPeriod(gossipConfig->BundleResourceUsageGossipPeriod);
            }
            EnableUpdateStatisticsOnHeartbeat_ = gossipConfig->EnableUpdateStatisticsOnHeartbeat;
        }

        TabletCellDecommissioner_->Reconfigure(config->TabletCellDecommissioner);
        TabletActionManager_->Reconfigure(config->TabletActionManager);
        TabletBalancer_->Reconfigure(config->TabletBalancer);

        if (ProfilingExecutor_) {
            ProfilingExecutor_->SetPeriod(config->ProfilingPeriod);
        }

        // COMPAT(ifsmirnov)
        if (!oldConfig->TabletManager->AccumulatePreloadPendingStoreCountCorrectly &&
            config->AccumulatePreloadPendingStoreCountCorrectly)
        {
            YT_LOG_DEBUG("Recomputing statistics of all tablet cells");
            RecomputeAllTabletCellStatistics();
        }
    }


    void SaveKeys(NCellMaster::TSaveContext& context) const
    {
        TabletMap_.SaveKeys(context);
        TableReplicaMap_.SaveKeys(context);
        TabletActionMap_.SaveKeys(context);
    }

    void SaveValues(NCellMaster::TSaveContext& context) const
    {
        TabletMap_.SaveValues(context);
        TableReplicaMap_.SaveValues(context);
        TabletActionMap_.SaveValues(context);

        Save(context, MountConfigKeysFromNodes_);
        Save(context, LocalMountConfigKeys_);
    }


    void LoadKeys(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TabletMap_.LoadKeys(context);
        TableReplicaMap_.LoadKeys(context);
        TabletActionMap_.LoadKeys(context);
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TabletMap_.LoadValues(context);
        TableReplicaMap_.LoadValues(context);
        TabletActionMap_.LoadValues(context);

        Load(context, MountConfigKeysFromNodes_);
        Load(context, LocalMountConfigKeys_);

        // Update mount config keys whenever the reign changes.
        FillMountConfigKeys_ = context.GetVersion() != static_cast<EMasterReign>(GetCurrentReign());
    }

    void RecomputeHunkResourceUsage()
    {
        for (const auto& [id, tabletBase] : Tablets()) {
            if (tabletBase->GetType() != EObjectType::Tablet) {
                continue;
            }
            auto* tablet = tabletBase->As<TTablet>();

            auto* table = tablet->GetTable();
            if (!table) {
                continue;
            }

            YT_LOG_DEBUG("Recomputing hunk resource usage (TabletId: %v, TableId: %v, "
                "HunkUncompressedDataSize: %v, HunkCompressedDataSize: %v)",
                tablet->GetId(),
                table->GetId(),
                tablet->GetHunkUncompressedDataSize(),
                tablet->GetHunkCompressedDataSize());

            i64 memoryDelta = 0;
            switch (tablet->GetInMemoryMode()) {
                case EInMemoryMode::Uncompressed:
                    memoryDelta = -tablet->GetHunkUncompressedDataSize();
                    break;
                case EInMemoryMode::Compressed:
                    memoryDelta = -tablet->GetHunkCompressedDataSize();
                    break;
                case EInMemoryMode::None:
                    memoryDelta = 0;
                    break;
                default:
                    YT_ABORT();
            }

            TTabletStatistics statisticsDelta;
            statisticsDelta.HunkUncompressedDataSize = tablet->GetHunkUncompressedDataSize();
            statisticsDelta.HunkCompressedDataSize = tablet->GetHunkCompressedDataSize();
            statisticsDelta.MemorySize = memoryDelta;
            table->AccountTabletStatisticsDelta(statisticsDelta);

            auto resourcesDelta = TTabletResources{}
                .SetTabletStaticMemory(memoryDelta);
            UpdateResourceUsage(table, resourcesDelta);
        }
    }

    void OnBeforeSnapshotLoaded() override
    {
        TMasterAutomatonPart::OnBeforeSnapshotLoaded();

        RecomputeAggregateTabletStatistics_ = false;
        RecomputeHunkResourceUsage_ = false;
        FillMountConfigKeys_ = false;
    }

    void OnAfterSnapshotLoaded() override
    {
        TMasterAutomatonPart::OnAfterSnapshotLoaded();

        if (RecomputeAggregateTabletStatistics_) {
            THashSet<TTabletOwnerBase*> resetTables;
            for (const auto& [id, tablet] : Tablets()) {
                if (auto* table = tablet->GetOwner()) {
                    if (resetTables.insert(table).second) {
                        table->ResetTabletStatistics();
                    }
                    table->AccountTabletStatistics(tablet->GetTabletStatistics());
                }
            }
        }

        InitBuiltins();

        if (FillMountConfigKeys_) {
            auto mountConfig = New<NTabletNode::TTableMountConfig>();
            LocalMountConfigKeys_ = mountConfig->GetRegisteredKeys();
        }
    }

    void OnAfterCellManagerSnapshotLoaded()
    {
        InitBuiltins();

        const auto& cellManager = Bootstrap_->GetTamedCellManager();

        for (auto* cellBase : cellManager->Cells(ECellarType::Tablet)) {
            YT_VERIFY(cellBase->GetType() == EObjectType::TabletCell);
            auto* cell = cellBase->As<TTabletCell>();
            cell->GossipStatistics().Initialize(Bootstrap_);
        }

        for (auto* bundleBase : cellManager->CellBundles(ECellarType::Tablet)) {
            YT_VERIFY(bundleBase->GetType() == EObjectType::TabletCellBundle);
            auto* bundle = bundleBase->As<TTabletCellBundle>();
            bundle->ResourceUsage().Initialize(Bootstrap_);
        }

        // COMPAT(ifsmirnov)
        if (RecomputeHunkResourceUsage_) {
            RecomputeHunkResourceUsage();
        }

        for (auto [actionId, action] : TabletActionMap_) {
            // NB: Process non-alive objects to pair with DestroyTabletAction.
            auto bundle = action->GetTabletCellBundle();
            if (!bundle) {
                continue;
            }

            bundle->TabletActions().insert(action);
            if (!action->IsFinished()) {
                bundle->IncreaseActiveTabletActionCount();
            }
        }
    }

    void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::Clear();

        TabletMap_.Clear();
        TableReplicaMap_.Clear();
        TabletActionMap_.Clear();

        DefaultTabletCellBundle_ = nullptr;
        SequoiaTabletCellBundle_ = nullptr;
    }

    void SetZeroState() override
    {
        InitBuiltins();

        auto mountConfig = New<NTabletNode::TTableMountConfig>();
        LocalMountConfigKeys_ = mountConfig->GetRegisteredKeys();
    }

    template <class T>
    T* GetBuiltin(T*& builtin)
    {
        if (!builtin) {
            InitBuiltins();
        }
        YT_VERIFY(builtin);
        return builtin;
    }

    void InitBuiltins()
    {
        const auto& securityManager = Bootstrap_->GetSecurityManager();

        // Cell bundles

        // default
        if (EnsureBuiltinCellBundleInitialized(DefaultTabletCellBundle_, DefaultTabletCellBundleId_, DefaultTabletCellBundleName)) {
            DefaultTabletCellBundle_->Acd().AddEntry(TAccessControlEntry(
                ESecurityAction::Allow,
                securityManager->GetUsersGroup(),
                EPermission::Use));
            DefaultTabletCellBundle_->ResourceLimits().TabletCount = 100'000;
            DefaultTabletCellBundle_->ResourceLimits().TabletStaticMemory = 1_TB;
        }

        // sequoia
        if (EnsureBuiltinCellBundleInitialized(SequoiaTabletCellBundle_, SequoiaTabletCellBundleId_, SequoiaTabletCellBundleName)) {
            SequoiaTabletCellBundle_->Acd().AddEntry(TAccessControlEntry(
                ESecurityAction::Allow,
                securityManager->GetUsersGroup(),
                EPermission::Use));
            SequoiaTabletCellBundle_->ResourceLimits().TabletCount = 100'000;
            SequoiaTabletCellBundle_->ResourceLimits().TabletStaticMemory = 1_TB;

            auto options = SequoiaTabletCellBundle_->GetOptions();
            options->ChangelogAccount = NSecurityClient::SequoiaAccountName;
            options->SnapshotAccount = NSecurityClient::SequoiaAccountName;
            SequoiaTabletCellBundle_->SetOptions(std::move(options));
        }
    }

    bool EnsureBuiltinCellBundleInitialized(TTabletCellBundle*& cellBundle, TTabletCellBundleId id, const TString& name)
    {
        if (cellBundle) {
            return false;
        }
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        if (auto* bundle = cellManager->FindCellBundle(id)) {
            YT_VERIFY(bundle->GetType() == EObjectType::TabletCellBundle);
            cellBundle = bundle->As<TTabletCellBundle>();
            return false;
        }
        auto options = New<TTabletCellOptions>();
        options->ChangelogAccount = DefaultStoreAccountName;
        options->SnapshotAccount = DefaultStoreAccountName;

        auto holder = TPoolAllocator::New<TTabletCellBundle>(id);
        holder->ResourceUsage().Initialize(Bootstrap_);
        cellBundle = cellManager->CreateCellBundle(name, std::move(holder), std::move(options))
            ->As<TTabletCellBundle>();
        return true;
    }

    void OnTabletCellStatisticsGossip()
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (!multicellManager->IsLocalMasterCellRegistered()) {
            return;
        }

        YT_LOG_INFO("Sending tablet cell statistics gossip message");

        NProto::TReqSetTabletCellStatistics request;
        request.set_cell_tag(multicellManager->GetCellTag());

        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        for (auto* cellBase : GetValuesSortedByKey(cellManager->Cells(ECellarType::Tablet))) {
            if (!IsObjectAlive(cellBase)) {
                continue;
            }

            YT_VERIFY(cellBase->GetType() == EObjectType::TabletCell);
            auto* cell = cellBase->As<TTabletCell>();
            auto* entry = request.add_entries();
            ToProto(entry->mutable_tablet_cell_id(), cell->GetId());

            if (multicellManager->IsPrimaryMaster()) {
                ToProto(entry->mutable_statistics(), cell->GossipStatistics().Cluster());
            } else {
                ToProto(entry->mutable_statistics(), cell->GossipStatistics().Local());
            }
        }

        if (multicellManager->IsPrimaryMaster()) {
            multicellManager->PostToSecondaryMasters(request, false);
        } else {
            multicellManager->PostToPrimaryMaster(request, false);
        }
    }

    void HydraSetTabletCellStatistics(NProto::TReqSetTabletCellStatistics* request)
    {
        auto cellTag = request->cell_tag();

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsPrimaryMaster() || cellTag == multicellManager->GetPrimaryCellTag());

        if (!multicellManager->IsRegisteredMasterCell(cellTag)) {
            YT_LOG_ERROR_IF(IsMutationLoggingEnabled(), "Received tablet cell statistics gossip message from unknown cell (CellTag: %v)",
                cellTag);
            return;
        }

        YT_LOG_INFO_IF(IsMutationLoggingEnabled(), "Received tablet cell statistics gossip message (CellTag: %v)",
            cellTag);

        for (const auto& entry : request->entries()) {
            auto cellId = FromProto<TTabletCellId>(entry.tablet_cell_id());
            auto* cell = FindTabletCell(cellId);
            if (!IsObjectAlive(cell)) {
                continue;
            }

            auto newStatistics = FromProto<TTabletCellStatistics>(entry.statistics());

            if (multicellManager->IsPrimaryMaster()) {
                *cell->GossipStatistics().Remote(cellTag) = newStatistics;
            } else {
                cell->GossipStatistics().Cluster() = newStatistics;
            }
        }
    }

    void OnTabletNodeHeartbeat(
        TNode* node,
        NTabletNodeTrackerClient::NProto::TReqHeartbeat* request,
        NTabletNodeTrackerClient::NProto::TRspHeartbeat* /*response*/)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        NProfiling::TWallTimer timer;

        const auto& tableManager = Bootstrap_->GetTableManager();

        // Copy tablet statistics, update performance counters and table replica statistics.
        auto now = TInstant::Now();

        for (auto& tabletInfo : request->tablets()) {
            auto tabletId = FromProto<TTabletId>(tabletInfo.tablet_id());
            auto mountRevision = tabletInfo.mount_revision();

            auto* tabletBase = FindTablet(tabletId);
            if (!IsObjectAlive(tabletBase) ||
                tabletBase->GetState() == ETabletState::Unmounted ||
                mountRevision != tabletBase->GetMountRevision())
            {
                continue;
            }

            YT_VERIFY(tabletBase->GetType() == EObjectType::Tablet);
            auto* tablet = tabletBase->As<TTablet>();
            auto* cell = tablet->GetCell();
            if (!IsObjectAlive(cell)){
                continue;
            }

            auto* slot = node->FindCellSlot(cell);
            if (!slot || (slot->PeerState != EPeerState::Leading && slot->PeerState != EPeerState::LeaderRecovery)) {
                continue;
            }

            auto tabletStatistics = tablet->GetTabletStatistics();
            tablet->GetTable()->DiscountTabletStatistics(tabletStatistics);
            cell->GossipStatistics().Local() -= tabletStatistics;

            tablet->NodeStatistics() = tabletInfo.statistics();

            tabletStatistics = tablet->GetTabletStatistics();
            tablet->GetTable()->AccountTabletStatistics(tabletStatistics);
            cell->GossipStatistics().Local() += tabletStatistics;

            auto* table = tablet->GetTable();
            if (table) {
                table->SetLastCommitTimestamp(std::max(
                    table->GetLastCommitTimestamp(),
                    tablet->NodeStatistics().last_commit_timestamp()));

                if (tablet->NodeStatistics().has_modification_time()) {
                    table->SetModificationTime(std::max(
                        table->GetModificationTime(),
                        FromProto<TInstant>(tablet->NodeStatistics().modification_time())));
                }

                if (tablet->NodeStatistics().has_access_time()) {
                    table->SetAccessTime(std::max(
                        table->GetAccessTime(),
                        FromProto<TInstant>(tablet->NodeStatistics().access_time())));
                }

                if (EnableUpdateStatisticsOnHeartbeat_) {
                    tableManager->ScheduleStatisticsUpdate(table, true, false);
                }
            }

            #define XX(name, Name) tablet->PerformanceCounters().Name.Update( \
                tabletInfo.performance_counters().name ## _count(), \
                now);
            ITERATE_TABLET_PERFORMANCE_COUNTERS(XX)
            #undef XX

            tablet->SetTabletErrorCount(tabletInfo.error_count());

            int replicationErrorCount = 0;
            for (const auto& protoReplicaInfo : tabletInfo.replicas()) {
                auto replicaId = FromProto<TTableReplicaId>(protoReplicaInfo.replica_id());
                auto* replica = FindTableReplica(replicaId);
                if (!replica) {
                    continue;
                }

                auto* replicaInfo = tablet->FindReplicaInfo(replica);
                if (!replicaInfo) {
                    continue;
                }

                PopulateTableReplicaInfoFromStatistics(replicaInfo, protoReplicaInfo.statistics());

                replicaInfo->SetHasError(protoReplicaInfo.has_error());
                replicationErrorCount += protoReplicaInfo.has_error();
            }
            tablet->SetReplicationErrorCount(replicationErrorCount);

            TabletBalancer_->OnTabletHeartbeat(tablet);
        }

        TabletNodeHeartbeatCounter_.Add(timer.GetElapsedTime());
    }

    void HydraUpdateUpstreamTabletState(NProto::TReqUpdateUpstreamTabletState* request)
    {
        auto tableId = FromProto<TTableId>(request->table_id());
        auto transactionId = FromProto<TTransactionId>(request->last_mount_transaction_id());
        auto actualState = request->has_actual_tablet_state()
            ? std::make_optional(FromProto<ETabletState>(request->actual_tablet_state()))
            : std::nullopt;
        auto expectedState = request->has_expected_tablet_state()
            ? std::make_optional(FromProto<ETabletState>(request->expected_tablet_state()))
            : std::nullopt;

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* node = cypressManager->FindNode(TVersionedNodeId(tableId));
        if (!IsObjectAlive(node)) {
            return;
        }

        YT_VERIFY(IsTableType(node->GetType()));
        auto* table = node->As<TTableNode>();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Received update upstream tablet state request "
            "(TableId: %v, ActualTabletState: %v, ExpectedTabletState: %v, ExpectedLastMountTransactionId: %v, ActualLastMountTransactionId: %v)",
            tableId,
            actualState,
            expectedState,
            transactionId,
            table->GetLastMountTransactionId());

        if (actualState) {
            table->SetActualTabletState(*actualState);
        }

        if (transactionId == table->GetLastMountTransactionId()) {
            if (expectedState) {
                table->SetExpectedTabletState(*expectedState);
            }
            table->SetLastMountTransactionId(TTransactionId());
        }
    }

    void HydraUpdateTabletState(NProto::TReqUpdateTabletState* request)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsSecondaryMaster());

        auto tableId = FromProto<TTableId>(request->table_id());
        const auto& cypressManager = Bootstrap_->GetCypressManager();
        auto* node = cypressManager->FindNode(TVersionedNodeId(tableId));
        if (!IsObjectAlive(node)) {
            return;
        }

        YT_VERIFY(IsTableType(node->GetType()));
        auto* table = node->As<TTableNode>();
        auto transactionId = FromProto<TTransactionId>(request->last_mount_transaction_id());
        table->SetPrimaryLastMountTransactionId(transactionId);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table tablet state check request received (TableId: %v, LastMountTransactionId %v, PrimaryLastMountTransactionId %v)",
            table->GetId(),
            table->GetLastMountTransactionId(),
            table->GetPrimaryLastMountTransactionId());

        UpdateTabletState(table);
    }

    void UpdateTabletState(TTabletOwnerBase* table)
    {
        if (!IsObjectAlive(table)) {
            return;
        }

        if (table->IsExternal()) {
            // Primary master is the coordinator of 2pc and commits after secondary to hold the exclusive lock.
            // (It is necessary for primary master to hold the lock longer to prevent
            // user from locking the node while secondary master still performs 2pc.)
            // Thus, secondary master can commit and send updates when primary master is not ready yet.
            // Here we ask secondary master to resend tablet state.

            NProto::TReqUpdateTabletState request;
            ToProto(request.mutable_table_id(), table->GetId());
            ToProto(request.mutable_last_mount_transaction_id(), table->GetLastMountTransactionId());

            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            multicellManager->PostToMaster(request, table->GetExternalCellTag());

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table tablet state check requested (TableId: %v, LastMountTransactionId %v)",
                table->GetId(),
                table->GetLastMountTransactionId());
            return;
        }

        // TODO(savrus): Remove this after testing multicell on real cluster is done.
        YT_LOG_DEBUG("Table tablet state check started (TableId: %v, LastMountTransactionId: %v, PrimaryLastMountTransactionId: %v, TabletCountByState: %v, TabletCountByExpectedState: %v)",
            table->GetId(),
            table->GetLastMountTransactionId(),
            table->GetPrimaryLastMountTransactionId(),
            ConvertToYsonString(table->TabletCountByState(), EYsonFormat::Text).ToString(),
            ConvertToYsonString(table->TabletCountByExpectedState(), EYsonFormat::Text).ToString());


        if (table->TabletCountByExpectedState()[ETabletState::Unmounting] > 0 ||
            table->TabletCountByExpectedState()[ETabletState::Freezing] > 0 ||
            table->TabletCountByExpectedState()[ETabletState::FrozenMounting] > 0 ||
            table->TabletCountByExpectedState()[ETabletState::Mounting] > 0 ||
            table->TabletCountByExpectedState()[ETabletState::Unfreezing] > 0)
        {
            return;
        }

        {
            // Just sanity check.
            auto tabletCount =
                table->TabletCountByExpectedState()[ETabletState::Mounted] +
                table->TabletCountByExpectedState()[ETabletState::Unmounted] +
                table->TabletCountByExpectedState()[ETabletState::Frozen];
            YT_VERIFY(tabletCount == std::ssize(table->Tablets()));
        }

        auto actualState = table->ComputeActualTabletState();
        std::optional<ETabletState> expectedState;

        if (table->GetLastMountTransactionId()) {
            if (table->TabletCountByExpectedState()[ETabletState::Mounted] > 0) {
                expectedState = ETabletState::Mounted;
            } else if (table->TabletCountByExpectedState()[ETabletState::Frozen] > 0) {
                expectedState = ETabletState::Frozen;
            } else {
                expectedState = ETabletState::Unmounted;
            }
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table tablet state updated "
            "(TableId: %v, ActualTabletState: %v, ExpectedTabletState: %v, LastMountTransactionId: %v, PrimaryLastMountTransactionId: %v)",
            table->GetId(),
            actualState,
            expectedState,
            table->GetLastMountTransactionId(),
            table->GetPrimaryLastMountTransactionId());

        table->SetActualTabletState(actualState);
        if (expectedState) {
            table->SetExpectedTabletState(*expectedState);
        }

        if (table->IsNative()) {
            YT_VERIFY(!table->GetPrimaryLastMountTransactionId());
            table->SetLastMountTransactionId(TTransactionId());
        } else {
            const auto& multicellManager = Bootstrap_->GetMulticellManager();
            YT_VERIFY(multicellManager->IsSecondaryMaster());

            // Check that primary master is waiting to clear LastMountTransactionId.
            bool clearLastMountTransactionId = table->GetLastMountTransactionId() &&
                table->GetLastMountTransactionId() == table->GetPrimaryLastMountTransactionId();

            // Statistics should be correct before setting the tablet state.
            const auto& tableManager = Bootstrap_->GetTableManager();
            tableManager->SendStatisticsUpdate(table);

            NProto::TReqUpdateUpstreamTabletState request;
            ToProto(request.mutable_table_id(), table->GetId());
            request.set_actual_tablet_state(ToProto<int>(actualState));
            if (clearLastMountTransactionId) {
                ToProto(request.mutable_last_mount_transaction_id(), table->GetLastMountTransactionId());
            }
            if (expectedState) {
                request.set_expected_tablet_state(ToProto<int>(*expectedState));
            }

            multicellManager->PostToMaster(request, table->GetNativeCellTag());

            if (clearLastMountTransactionId) {
                table->SetLastMountTransactionId(TTransactionId());
                table->SetPrimaryLastMountTransactionId(TTransactionId());
            }
        }
    }

    void HydraOnTabletMounted(NProto::TRspMountTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::Tablet);

        auto frozen = response->frozen();
        OnTabletMounted(tabletId, frozen);
    }

    void HydraOnHunkTabletMounted(NProto::TRspMountHunkTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::HunkTablet);
        OnTabletMounted(tabletId, /*frozen*/ false);
    }

    void OnTabletMounted(TTabletId tabletId, bool frozen)
    {
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto state = tablet->GetState();
        if (state != ETabletState::Mounting && state != ETabletState::FrozenMounting) {
            if (!tablet->GetWasForcefullyUnmounted()) {
                // NB. This (and similar in HydraOnTabletXxx) alerts can actually occur. Consider the case:
                // - initially, the tablet is mounted
                // - the tablet is being frozen, master sends TReqFreezeTablet, the response is delayed
                // - the tablet is being forcefully unmounted
                // - the tablet is being mounted again
                // - TRspFreezeTablet finally arrives while the tablet is in mounting state
                // However, forced unmount should be done for this to happen, and only superusers
                // have the permission for it.
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Mounted notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                    state,
                    tabletId);
            }
            return;
        }

        auto* table = tablet->GetOwner();
        auto* cell = tablet->GetCell();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet mounted (TableId: %v, TabletId: %v, MountRevision: %x, CellId: %v, Frozen: %v)",
            table->GetId(),
            tablet->GetId(),
            tablet->GetMountRevision(),
            cell->GetId(),
            frozen);

        tablet->SetState(frozen ? ETabletState::Frozen : ETabletState::Mounted);

        OnTabletActionStateChanged(tablet->GetAction());
        UpdateTabletState(table);
    }

    void HydraOnTabletUnmounted(NProto::TRspUnmountTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::Tablet);

        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        if (!ValidateTabletUnmounted(tablet)) {
            return;
        }

        auto* typedTablet = tablet->As<TTablet>();
        if (response->has_replication_progress()) {
            typedTablet->ReplicationProgress() = FromProto<TReplicationProgress>(response->replication_progress());
        }

        SetTabletEdenStoreIds(typedTablet, FromProto<std::vector<TStoreId>>(response->mount_hint().eden_store_ids()));
        DiscardDynamicStores(typedTablet);

        DoTabletUnmounted(typedTablet, /*force*/ false);

        OnTabletActionStateChanged(tablet->GetAction());
    }

    void HydraOnHunkTabletUnmounted(NProto::TRspUnmountHunkTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::HunkTablet);

        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        if (!ValidateTabletUnmounted(tablet)) {
            return;
        }

        auto* hunkTablet = tablet->As<THunkTablet>();
        DoTabletUnmounted(hunkTablet, /*force*/ false);
    }

    void HydraOnTabletFrozen(NProto::TRspFreezeTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::Tablet);
        auto* tablet = FindTablet(tabletId)->As<TTablet>();
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto* table = tablet->GetTable();
        auto* cell = tablet->GetCell();

        auto state = tablet->GetState();
        if (state != ETabletState::Freezing) {
            if (!tablet->GetWasForcefullyUnmounted()) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Frozen notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                    state,
                    tabletId);
            }
            return;
        }

        SetTabletEdenStoreIds(tablet->As<TTablet>(), FromProto<std::vector<TStoreId>>(response->mount_hint().eden_store_ids()));

        DiscardDynamicStores(tablet->As<TTablet>());

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet frozen (TableId: %v, TabletId: %v, CellId: %v)",
            table->GetId(),
            tablet->GetId(),
            cell->GetId());

        tablet->SetState(ETabletState::Frozen);
        OnTabletActionStateChanged(tablet->GetAction());
        UpdateTabletState(table);
    }

    void HydraOnTabletUnfrozen(NProto::TRspUnfreezeTablet* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::Tablet);
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto* table = tablet->GetOwner();
        auto* cell = tablet->GetCell();

        auto state = tablet->GetState();
        if (state != ETabletState::Unfreezing) {
            if (!tablet->GetWasForcefullyUnmounted()) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Unfrozen notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                    state,
                    tabletId);
            }
            return;
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet unfrozen (TableId: %v, TabletId: %v, CellId: %v)",
            table->GetId(),
            tablet->GetId(),
            cell->GetId());

        tablet->SetState(ETabletState::Mounted);
        OnTabletActionStateChanged(tablet->GetAction());
        UpdateTabletState(table);
    }

    void HydraUpdateTableReplicaStatistics(NProto::TReqUpdateTableReplicaStatistics* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::Tablet);
        auto* tabletBase = FindTablet(tabletId);
        if (!IsObjectAlive(tabletBase)) {
            return;
        }

        auto* tablet = tabletBase->As<TTablet>();

        auto replicaId = FromProto<TTableReplicaId>(request->replica_id());
        auto* replica = FindTableReplica(replicaId);
        if (!IsObjectAlive(replica)) {
            return;
        }

        auto mountRevision = request->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        auto* replicaInfo = tablet->GetReplicaInfo(replica);
        PopulateTableReplicaInfoFromStatistics(replicaInfo, request->statistics());

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica statistics updated (TabletId: %v, ReplicaId: %v, "
            "CommittedReplicationRowIndex: %v, CurrentReplicationTimestamp: %v)",
            tabletId,
            replicaId,
            replicaInfo->GetCommittedReplicationRowIndex(),
            replicaInfo->GetCurrentReplicationTimestamp());
    }

    void HydraOnTableReplicaEnabled(NProto::TRspEnableTableReplica* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::Tablet);
        auto* tabletBase = FindTablet(tabletId);
        if (!IsObjectAlive(tabletBase)) {
            return;
        }

        auto* tablet = tabletBase->As<TTablet>();

        auto replicaId = FromProto<TTableReplicaId>(response->replica_id());
        auto* replica = FindTableReplica(replicaId);
        if (!IsObjectAlive(replica)) {
            return;
        }

        auto mountRevision = response->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        auto* replicaInfo = tablet->GetReplicaInfo(replica);
        if (replicaInfo->GetState() != ETableReplicaState::Enabling) {
            YT_LOG_WARNING_IF(IsMutationLoggingEnabled(), "Enabled replica notification received for a replica in a wrong state, "
                "ignored (TabletId: %v, ReplicaId: %v, State: %v)",
                tabletId,
                replicaId,
                replicaInfo->GetState());
            return;
        }

        StopReplicaTransition(tablet, replica, replicaInfo, ETableReplicaState::Enabled);
        CheckTransitioningReplicaTablets(replica);
    }

    void HydraOnTableReplicaDisabled(NProto::TRspDisableTableReplica* response)
    {
        auto tabletId = FromProto<TTabletId>(response->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::Tablet);
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        YT_VERIFY(tablet->GetType() == EObjectType::Tablet);

        auto replicaId = FromProto<TTableReplicaId>(response->replica_id());
        auto* replica = FindTableReplica(replicaId);
        if (!IsObjectAlive(replica)) {
            return;
        }

        auto mountRevision = response->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        auto* replicaInfo = tablet->As<TTablet>()->GetReplicaInfo(replica);
        if (replicaInfo->GetState() != ETableReplicaState::Disabling) {
            YT_LOG_WARNING_IF(IsMutationLoggingEnabled(), "Disabled replica notification received for a replica in a wrong state, "
                "ignored (TabletId: %v, ReplicaId: %v, State: %v)",
                tabletId,
                replicaId,
                replicaInfo->GetState());
            return;
        }

        StopReplicaTransition(tablet->As<TTablet>(), replica, replicaInfo, ETableReplicaState::Disabled);
        CheckTransitioningReplicaTablets(replica);
    }

    void StartReplicaTransition(TTablet* tablet, TTableReplica* replica, TTableReplicaInfo* replicaInfo, ETableReplicaState newState)
    {
        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica is now transitioning (TableId: %v, TabletId: %v, ReplicaId: %v, State: %v -> %v)",
            tablet->GetTable()->GetId(),
            tablet->GetId(),
            replica->GetId(),
            replicaInfo->GetState(),
            newState);
        replicaInfo->SetState(newState);
        YT_VERIFY(replica->TransitioningTablets().insert(tablet).second);
    }

    void StopReplicaTransition(TTablet* tablet, TTableReplica* replica, TTableReplicaInfo* replicaInfo, ETableReplicaState newState)
    {
        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica is no longer transitioning (TableId: %v, TabletId: %v, ReplicaId: %v, State: %v -> %v)",
            tablet->GetTable()->GetId(),
            tablet->GetId(),
            replica->GetId(),
            replicaInfo->GetState(),
            newState);
        replicaInfo->SetState(newState);
        YT_VERIFY(replica->TransitioningTablets().erase(tablet) == 1);
    }

    void CheckTransitioningReplicaTablets(TTableReplica* replica)
    {
        auto state = replica->GetState();
        if (state != ETableReplicaState::Enabling && state != ETableReplicaState::Disabling) {
            return;
        }

        if (!replica->TransitioningTablets().empty()) {
            return;
        }

        auto* table = replica->GetTable();

        bool enabled;
        switch (state) {
            case ETableReplicaState::Enabling:
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica enabled (TableId: %v, ReplicaId: %v)",
                    table->GetId(),
                    replica->GetId());
                replica->SetState(ETableReplicaState::Enabled);
                enabled = true;
                break;

            case ETableReplicaState::Disabling:
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica disabled (TableId: %v, ReplicaId: %v)",
                    table->GetId(),
                    replica->GetId());
                replica->SetState(ETableReplicaState::Disabled);
                enabled = false;
                break;

            default:
                YT_ABORT();
        }

        ReplicaEnablementUpdated_.Fire(replica->GetId(), enabled);
    }

    void DiscardDynamicStores(TTablet* tablet)
    {
        auto stores = EnumerateStoresInChunkTree(tablet->GetChunkList());

        std::vector<TChunkTree*> dynamicStores;
        for (auto* store : stores) {
            if (IsDynamicTabletStoreType(store->GetType())) {
                dynamicStores.push_back(store);
                store->AsDynamicStore()->SetFlushedChunk(nullptr);
            }
        }

        if (dynamicStores.empty()) {
            return;
        }

        // NB: Dynamic stores can be detached unambiguously since they are direct children of a tablet.
        CopyChunkListsIfShared(tablet->GetTable(), tablet->GetIndex(), tablet->GetIndex(), /*force*/ false);

        DetachChunksFromTablet(
            tablet,
            dynamicStores,
            tablet->GetTable()->IsPhysicallySorted()
                ? EChunkDetachPolicy::SortedTablet
                : EChunkDetachPolicy::OrderedTabletSuffix);

        auto* table = tablet->GetTable();
        table->SnapshotStatistics() = {};
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            table->SnapshotStatistics() += table->GetChunkList(contentType)->Statistics().ToDataStatistics();
        }
        const auto& tableManager = Bootstrap_->GetTableManager();
        tableManager->ScheduleStatisticsUpdate(
            table,
            /*updateDataStatistics*/ true,
            /*updateTabletStatistics*/ false);

        TTabletStatistics statisticsDelta;
        statisticsDelta.ChunkCount = -ssize(dynamicStores);
        tablet->GetCell()->GossipStatistics().Local() += statisticsDelta;
        table->AccountTabletStatisticsDelta(statisticsDelta);
    }

    void AbandonDynamicStores(TTablet* tablet)
    {
        // Making a copy since store->Abandon() will remove elements from tablet->DynamicStores().
        auto stores = tablet->DynamicStores();

        for (auto* store : stores) {
            store->Abandon();
        }
    }

    bool ValidateTabletUnmounted(TTabletBase* tablet)
    {
        auto state = tablet->GetState();
        if (state != ETabletState::Unmounting) {
            if (!tablet->GetWasForcefullyUnmounted()) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Unmounted notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                    state,
                    tablet->GetId());
            }
            return false;
        }

        return true;
    }

    void DoTabletUnmountedBase(TTabletBase* tablet, bool force)
    {
        auto* table = tablet->GetOwner();
        auto* cell = tablet->GetCell();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet unmounted (TableId: %v, TabletId: %v, CellId: %v)",
            table->GetId(),
            tablet->GetId(),
            cell->GetId());

        CheckIfFullyUnmounted(cell);

        auto resourceUsageDelta = TTabletResources()
            .SetTabletStaticMemory(tablet->GetTabletStaticMemorySize());

        tablet->SetInMemoryMode(EInMemoryMode::None);
        tablet->SetState(ETabletState::Unmounted);
        tablet->SetCell(nullptr);
        tablet->SetStoresUpdatePreparedTransaction(nullptr);
        tablet->SetMountRevision(NullRevision);
        tablet->SetWasForcefullyUnmounted(force);

        UpdateResourceUsage(table, -resourceUsageDelta);
        UpdateTabletState(table);

        const auto& objectManager = Bootstrap_->GetObjectManager();
        YT_VERIFY(cell->Tablets().erase(tablet) == 1);
        objectManager->UnrefObject(cell);
    }

    void DoTabletUnmounted(TTablet* tablet, bool force)
    {
        // NB: Cell may die in DoTabletUnmountedBase.
        auto cellId = tablet->GetCell()->GetId();

        auto tabletStatistics = tablet->GetTabletStatistics();
        {
            auto* cell = tablet->GetCell();
            cell->GossipStatistics().Local() -= tabletStatistics;
        }
        tablet->GetOwner()->DiscountTabletStatistics(tabletStatistics);
        tablet->NodeStatistics().Clear();
        tablet->PerformanceCounters() = TTabletPerformanceCounters();

        DoTabletUnmountedBase(tablet, force);

        if (tablet->GetBackupState() != ETabletBackupState::None) {
            const auto& backupManager = Bootstrap_->GetBackupManager();
            backupManager->OnBackupInterruptedByUnmount(tablet);
        }

        auto* table = tablet->GetTable();
        if (!table->IsPhysicallySorted()) {
            const auto& chunkListStatistics = tablet->GetChunkList()->Statistics();
            if (tablet->GetTrimmedRowCount() > chunkListStatistics.LogicalRowCount) {
                auto message = Format(
                    "Trimmed row count exceeds total row count of the tablet "
                    "and will be rolled back (TableId: %v, TabletId: %v, CellId: %v, "
                    "TrimmedRowCount: %v, LogicalRowCount: %v)",
                    table->GetId(),
                    tablet->GetId(),
                    cellId,
                    tablet->GetTrimmedRowCount(),
                    chunkListStatistics.LogicalRowCount);
                if (force) {
                    YT_LOG_WARNING_IF(IsMutationLoggingEnabled(), message);
                    tablet->SetTrimmedRowCount(chunkListStatistics.LogicalRowCount);
                } else {
                    YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), message);
                }
            }
        }

        for (auto it : GetIteratorsSortedByKey(tablet->Replicas())) {
            auto* replica = it->first;
            auto& replicaInfo = it->second;
            if (replica->TransitioningTablets().erase(tablet) == 1) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Table replica is still transitioning (TableId: %v, TabletId: %v, ReplicaId: %v, State: %v)",
                    tablet->GetTable()->GetId(),
                    tablet->GetId(),
                    replica->GetId(),
                    replicaInfo.GetState());
            } else {
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Table replica state updated (TableId: %v, TabletId: %v, ReplicaId: %v, State: %v -> %v)",
                    tablet->GetTable()->GetId(),
                    tablet->GetId(),
                    replica->GetId(),
                    replicaInfo.GetState(),
                    ETableReplicaState::None);
            }
            replicaInfo.SetState(ETableReplicaState::None);
            CheckTransitioningReplicaTablets(replica);
        }

        for (auto transactionId : tablet->UnconfirmedDynamicTableLocks()) {
            table->ConfirmDynamicTableLock(transactionId);
        }
        tablet->UnconfirmedDynamicTableLocks().clear();

        tablet->GetOwner()->AccountTabletStatistics(tablet->GetTabletStatistics());
    }

    void DoTabletUnmounted(THunkTablet* tablet, bool force)
    {
        DoTabletUnmountedBase(tablet, force);

        auto* owner = tablet->GetOwner();
        owner->AccountTabletStatistics(tablet->GetTabletStatistics());

        const auto& chunkManager = Bootstrap_->GetChunkManager();

        auto chunks = EnumerateChunksInChunkTree(tablet->GetChunkList());
        for (auto* chunk : chunks) {
            chunk->SetSealable(true);
            chunkManager->ScheduleChunkSeal(chunk);
        }
    }

    TDynamicStoreId GenerateDynamicStoreId(const TTablet* tablet, TDynamicStoreId hintId = NullObjectId)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto type = tablet->GetTable()->IsPhysicallySorted()
            ? EObjectType::SortedDynamicTabletStore
            : EObjectType::OrderedDynamicTabletStore;
        return objectManager->GenerateId(type, hintId);
    }

    TDynamicStore* CreateDynamicStore(TTablet* tablet, TDynamicStoreId hintId = NullObjectId)
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto id = GenerateDynamicStoreId(tablet, hintId);
        return chunkManager->CreateDynamicStore(id, tablet);
    }

    void AttachDynamicStoreToTablet(TTablet* tablet, TDynamicStore* dynamicStore)
    {
        auto* table = tablet->GetTable();

        CopyChunkListsIfShared(table, tablet->GetIndex(), tablet->GetIndex());

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        chunkManager->AttachToChunkList(tablet->GetChunkList(), dynamicStore);
        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Dynamic store attached to tablet (TabletId: %v, DynamicStoreId: %v)",
            tablet->GetId(),
            dynamicStore->GetId());

        table->SnapshotStatistics() = {};
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            table->SnapshotStatistics() += table->GetChunkList(contentType)->Statistics().ToDataStatistics();
        }

        const auto& tableManager = Bootstrap_->GetTableManager();
        tableManager->ScheduleStatisticsUpdate(
            table,
            /*updateDataStatistics*/ true,
            /*updateTabletStatistics*/ false);

        TTabletStatistics statisticsDelta;
        statisticsDelta.ChunkCount = 1;
        tablet->GetCell()->GossipStatistics().Local() += statisticsDelta;
        tablet->GetTable()->AccountTabletStatisticsDelta(statisticsDelta);
    }

    template <class TRequest>
    void CreateAndAttachDynamicStores(TTablet* tablet, TRequest* request)
    {
        for (int index = 0; index < DynamicStoreIdPoolSize; ++index) {
            auto* dynamicStore = CreateDynamicStore(tablet);
            AttachDynamicStoreToTablet(tablet, dynamicStore);
            ToProto(request->add_dynamic_store_ids(), dynamicStore->GetId());
        }
    }

    void CopyChunkListsIfShared(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        bool force = false)
    {
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            CopyChunkListIfShared(
                table,
                contentType,
                firstTabletIndex,
                lastTabletIndex,
                force);
        }
    }

public:
    void CopyChunkListIfShared(
        TTableNode* table,
        EChunkListContentType contentType,
        int firstTabletIndex,
        int lastTabletIndex,
        bool force = false)
    {
        TWallTimer timer;
        auto* counters = GetCounters({}, table);
        auto reportTimeGuard = Finally([&] {
            counters->CopyChunkListTime.Add(timer.GetElapsedTime());
        });

        int actionCount = 0;

        auto* oldRootChunkList = table->GetChunkList(contentType);
        auto& chunkLists = oldRootChunkList->Children();
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        auto checkStatisticsMatch = [] (const TChunkTreeStatistics& lhs, TChunkTreeStatistics rhs) {
            rhs.ChunkListCount = lhs.ChunkListCount;
            rhs.Rank = lhs.Rank;
            return lhs == rhs;
        };

        if (oldRootChunkList->GetObjectRefCounter(/*flushUnrefs*/ true) > 1) {
            auto statistics = oldRootChunkList->Statistics();
            auto* newRootChunkList = chunkManager->CreateChunkList(oldRootChunkList->GetKind());
            chunkManager->AttachToChunkList(
                newRootChunkList,
                chunkLists.data(),
                chunkLists.data() + firstTabletIndex);

            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* newTabletChunkList = chunkManager->CloneTabletChunkList(chunkLists[index]->AsChunkList());
                chunkManager->AttachToChunkList(newRootChunkList, newTabletChunkList);

                actionCount += newTabletChunkList->Statistics().ChunkCount;
            }

            chunkManager->AttachToChunkList(
                newRootChunkList,
                chunkLists.data() + lastTabletIndex + 1,
                chunkLists.data() + chunkLists.size());

            actionCount += newRootChunkList->Children().size();

            // Replace root chunk list.
            table->SetChunkList(contentType, newRootChunkList);
            newRootChunkList->AddOwningNode(table);
            oldRootChunkList->RemoveOwningNode(table);
            if (!checkStatisticsMatch(newRootChunkList->Statistics(), statistics)) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                    "Invalid new root chunk list statistics "
                    "(TableId: %v, ContentType: %v, NewRootChunkListStatistics: %v, Statistics: %v)",
                    table->GetId(),
                    contentType,
                    newRootChunkList->Statistics(),
                    statistics);
            }
        } else {
            auto statistics = oldRootChunkList->Statistics();

            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* oldTabletChunkList = chunkLists[index]->AsChunkList();
                if (force || oldTabletChunkList->GetObjectRefCounter(/*flushUnrefs*/ true) > 1) {
                    auto* newTabletChunkList = chunkManager->CloneTabletChunkList(oldTabletChunkList);
                    chunkManager->ReplaceChunkListChild(oldRootChunkList, index, newTabletChunkList);

                    actionCount += newTabletChunkList->Statistics().ChunkCount;

                    // ReplaceChunkListChild assumes that statistics are updated by caller.
                    // Here everything remains the same except for missing subtablet chunk lists.
                    int subtabletChunkListCount = oldTabletChunkList->Statistics().ChunkListCount - 1;
                    if (subtabletChunkListCount > 0) {
                        TChunkTreeStatistics delta{};
                        delta.ChunkListCount = -subtabletChunkListCount;
                        AccumulateUniqueAncestorsStatistics(newTabletChunkList, delta);
                    }
                }
            }

            if (!checkStatisticsMatch(oldRootChunkList->Statistics(), statistics)) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                    "Invalid old root chunk list statistics "
                    "(TableId: %v, ContentType: %v, OldRootChunkListStatistics: %v, Statistics: %v)",
                    table->GetId(),
                    contentType,
                    oldRootChunkList->Statistics(),
                    statistics);
            }
        }

        if (actionCount > 0) {
            counters->CopyChunkListIfSharedActionCount.Increment(actionCount);
        }
    }

private:
    static int GetFirstDynamicStoreIndex(const TChunkList* chunkList)
    {
        YT_VERIFY(chunkList->GetKind() == EChunkListKind::OrderedDynamicTablet);

        const auto& children = chunkList->Children();
        int firstDynamicStoreIndex = static_cast<int>(children.size()) - 1;
        YT_VERIFY(IsDynamicTabletStoreType(children[firstDynamicStoreIndex]->GetType()));
        while (firstDynamicStoreIndex > chunkList->GetTrimmedChildCount() &&
            IsDynamicTabletStoreType(children[firstDynamicStoreIndex - 1]->GetType()))
        {
            --firstDynamicStoreIndex;
        }

        return firstDynamicStoreIndex;
    }

    void ValidateTabletContainsStore(const TTablet* tablet, TChunkTree* const store)
    {
        const auto* tabletChunkList = tablet->GetChunkList();

        // Fast path: the store belongs to the tablet directly.
        if (tabletChunkList->ChildToIndex().contains(store)) {
            return;
        }

        auto onParent = [&] (TChunkTree* parent) {
            if (parent->GetType() != EObjectType::ChunkList) {
                return false;
            }
            auto* chunkList = parent->AsChunkList();
            if (chunkList->GetKind() != EChunkListKind::SortedDynamicSubtablet) {
                return false;
            }
            if (tabletChunkList->ChildToIndex().contains(chunkList)) {
                return true;
            }
            return false;
        };

        // NB: tablet chunk list has rank of at most 2, so it suffices to check only
        // one intermediate chunk list between store and tablet.
        if (IsChunkTabletStoreType(store->GetType())) {
            for (auto& [parent, multiplicity] : store->AsChunk()->Parents()) {
                if (onParent(parent)) {
                    return;
                }
            }
        } else if (store->GetType() == EObjectType::ChunkView) {
            for (auto* parent : store->AsChunkView()->Parents()) {
                if (onParent(parent)) {
                    return;
                }
            }
        } else if (IsDynamicTabletStoreType(store->GetType())) {
            for (auto* parent : store->AsDynamicStore()->Parents()) {
                if (onParent(parent)) {
                    return;
                }
            }
        }

        THROW_ERROR_EXCEPTION("Store %v does not belong to tablet %v",
            store->GetId(),
            tablet->GetId());
    }

    void HydraPrepareUpdateTabletStores(
        TTransaction* transaction,
        NProto::TReqUpdateTabletStores* request,
        const TTransactionPrepareOptions& options)
    {
        YT_VERIFY(options.Persistent);

        const auto& dynamicConfig = GetDynamicConfig();
        if ((request->hunk_chunks_to_add_size() > 0 || request->hunk_chunks_to_remove_size() > 0) && !dynamicConfig->EnableHunks) {
            THROW_ERROR_EXCEPTION("Hunks are not enabled");
        }

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = GetTabletOrThrow(tabletId);

        PrepareTabletStoresUpdateBase(tablet);

        auto validateStoreType = [&] (TObjectId id, TStringBuf action) {
            auto type = TypeFromId(id);
            if (!IsChunkTabletStoreType(type) &&
                !IsDynamicTabletStoreType(type) &&
                type != EObjectType::ChunkView)
            {
                THROW_ERROR_EXCEPTION("Cannot %v store %v of type %Qlv",
                    action,
                    id,
                    type);
            }
        };

        auto validateHunkChunkType = [&] (TChunkId id, TStringBuf action) {
            auto type = TypeFromId(id);
            if (!IsBlobChunkType(type)) {
                THROW_ERROR_EXCEPTION("Cannot %v hunk chunk %v of type %Qlv",
                    action,
                    id,
                    type);
            }
        };

        for (const auto& descriptor : request->stores_to_add()) {
            validateStoreType(FromProto<TObjectId>(descriptor.store_id()), "attach");
        }

        for (const auto& descriptor : request->stores_to_remove()) {
            validateStoreType(FromProto<TObjectId>(descriptor.store_id()), "detach");
        }

        for (const auto& descriptor : request->hunk_chunks_to_add()) {
            validateHunkChunkType(FromProto<TChunkId>(descriptor.chunk_id()), "attach");
        }

        for (const auto& descriptor : request->hunk_chunks_to_remove()) {
            validateHunkChunkType(FromProto<TChunkId>(descriptor.chunk_id()), "detach");
        }

        auto mountRevision = request->mount_revision();
        tablet->ValidateMountRevision(mountRevision);

        switch (tablet->GetType()) {
            case EObjectType::Tablet: {
                auto* table = tablet->As<TTablet>()->GetTable();
                if (table->IsPhysicallySorted()) {
                    PrepareSortedTabletStoresUpdate(tablet->As<TTablet>(), request);
                } else {
                    PrepareOrderedTabletStoresUpdate(tablet->As<TTablet>(), request);
                }
                break;
            }
            default:
                YT_ABORT();
        }

        tablet->SetStoresUpdatePreparedTransaction(transaction);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Tablet stores update prepared (TransactionId: %v, TableId: %v, TabletId: %v)",
            transaction->GetId(),
            tablet->GetOwner()->GetId(),
            tabletId);
    }

    void HydraPrepareUpdateHunkTabletStores(
        TTransaction* transaction,
        NProto::TReqUpdateHunkTabletStores* request,
        const TTransactionPrepareOptions& options)
    {
        YT_VERIFY(options.Persistent);

        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        YT_VERIFY(TypeFromId(tabletId) == EObjectType::HunkTablet);
        auto* tablet = GetTabletOrThrow(tabletId)->As<THunkTablet>();

        auto mountRevision = request->mount_revision();
        tablet->ValidateMountRevision(mountRevision);

        PrepareTabletStoresUpdateBase(tablet);

        tablet->SetStoresUpdatePreparedTransaction(transaction);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Hunk tablet stores update prepared "
            "(TransactionId: %v, HunkStorageId: %v, TabletId: %v)",
            transaction->GetId(),
            tablet->GetOwner()->GetId(),
            tabletId);
    }

    void PrepareTabletStoresUpdateBase(TTabletBase* tablet)
    {
        if (tablet->GetStoresUpdatePreparedTransaction()) {
            THROW_ERROR_EXCEPTION("Stores update for tablet %v is already prepared by transaction %v",
                tablet->GetId(),
                tablet->GetStoresUpdatePreparedTransaction()->GetId());
        }

        auto state = tablet->GetState();
        if (state != ETabletState::Mounted &&
            state != ETabletState::Unmounting &&
            state != ETabletState::Freezing)
        {
            THROW_ERROR_EXCEPTION("Cannot update stores while tablet %v is in %Qlv state",
                tablet->GetId(),
                state);
        }
    }

    void PrepareOrderedTabletStoresUpdate(
        TTablet* tablet,
        NProto::TReqUpdateTabletStores* request)
    {
        auto tabletId = tablet->GetId();
        const auto* table = tablet->GetTable();
        const auto* tabletChunkList = tablet->GetChunkList();

        if (request->stores_to_add_size() > 0) {
            if (request->stores_to_add_size() > 1) {
                THROW_ERROR_EXCEPTION("Cannot attach more than one store to an ordered tablet %v at once",
                    tabletId);
            }

            const auto& descriptor = request->stores_to_add(0);
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            YT_VERIFY(descriptor.has_starting_row_index());
            if (tabletChunkList->Statistics().LogicalRowCount != descriptor.starting_row_index()) {
                THROW_ERROR_EXCEPTION("Invalid starting row index of store %v in tablet %v: expected %v, got %v",
                    storeId,
                    tabletId,
                    tabletChunkList->Statistics().LogicalRowCount,
                    descriptor.starting_row_index());
            }
        }

        auto updateReason = FromProto<ETabletStoresUpdateReason>(request->update_reason());

        if (updateReason == ETabletStoresUpdateReason::Trim) {
            int childIndex = tabletChunkList->GetTrimmedChildCount();
            const auto& children = tabletChunkList->Children();
            for (const auto& descriptor : request->stores_to_remove()) {
                auto storeId = FromProto<TStoreId>(descriptor.store_id());
                if (TypeFromId(storeId) == EObjectType::OrderedDynamicTabletStore) {
                    continue;
                }

                if (childIndex >= std::ssize(children)) {
                    THROW_ERROR_EXCEPTION("Attempted to trim store %v which is not part of tablet %v",
                        storeId,
                        tabletId);
                }
                if (children[childIndex]->GetId() != storeId) {
                    THROW_ERROR_EXCEPTION("Invalid store to trim in tablet %v: expected %v, got %v",
                        tabletId,
                        children[childIndex]->GetId(),
                        storeId);
                }
                ++childIndex;
            }
        }

        if (updateReason == ETabletStoresUpdateReason::Flush &&
            IsDynamicStoreReadEnabled(table) &&
            !request->stores_to_remove().empty())
        {
            auto storeId = FromProto<TStoreId>(request->stores_to_remove(0).store_id());
            int firstDynamicStoreIndex = GetFirstDynamicStoreIndex(tabletChunkList);
            const auto* firstDynamicStore = tabletChunkList->Children()[firstDynamicStoreIndex];
            if (firstDynamicStore->GetId() != storeId) {
                THROW_ERROR_EXCEPTION("Attempted to flush ordered dynamic store out of order")
                    << TErrorAttribute("first_dynamic_store_id", firstDynamicStore->GetId())
                    << TErrorAttribute("flushed_store_id", storeId);
            }
        }
    }

    void PrepareSortedTabletStoresUpdate(
        TTablet* tablet,
        NProto::TReqUpdateTabletStores* request)
    {
        const auto* table = tablet->GetTable();

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        for (const auto& descriptor : request->stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            auto type = TypeFromId(storeId);

            if (IsChunkTabletStoreType(type)) {
                auto* chunk = chunkManager->GetChunkOrThrow(storeId);
                ValidateTabletContainsStore(tablet, chunk);
            } else if (type == EObjectType::ChunkView) {
                auto* chunkView = chunkManager->GetChunkViewOrThrow(storeId);
                ValidateTabletContainsStore(tablet, chunkView);
            } else if (IsDynamicTabletStoreType(type)) {
                if (table->GetMountedWithEnabledDynamicStoreRead()) {
                    auto* dynamicStore = chunkManager->GetDynamicStoreOrThrow(storeId);
                    ValidateTabletContainsStore(tablet, dynamicStore);
                }
            } else {
                THROW_ERROR_EXCEPTION("Cannot detach store %v of type %v from tablet %v",
                    storeId,
                    type,
                    tablet->GetId());
            }
        }
    }

    void AttachChunksToTablet(TTabletBase* tablet, const std::vector<TChunkTree*>& chunkTrees)
    {
        std::vector<TChunkTree*> storeChildren;
        std::vector<TChunkTree*> hunkChildren;
        storeChildren.reserve(chunkTrees.size());
        hunkChildren.reserve(chunkTrees.size());
        for (auto* child : chunkTrees) {
            if (IsHunkChunk(child)) {
                hunkChildren.push_back(child);
            } else {
                storeChildren.push_back(child);
            }
        }

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        chunkManager->AttachToChunkList(tablet->GetChunkList(), storeChildren);
        chunkManager->AttachToChunkList(tablet->GetHunkChunkList(), hunkChildren);
    }

    TChunkList* GetTabletChildParent(TTabletBase* tablet, TChunkTree* child)
    {
        if (IsHunkChunk(child)) {
            return tablet->GetHunkChunkList();
        } else {
            if (GetParentCount(child) == 1) {
                auto* parent = GetUniqueParent(child);
                YT_VERIFY(parent->GetType() == EObjectType::ChunkList);
                return parent;
            }
            return tablet->GetChunkList();
        }
    }

    void PruneEmptySubtabletChunkList(TChunkList* chunkList)
    {
        while (chunkList->GetKind() == EChunkListKind::SortedDynamicSubtablet && chunkList->Children().empty()) {
            auto* parent = GetUniqueParent(chunkList);
            const auto& chunkManager = Bootstrap_->GetChunkManager();
            chunkManager->DetachFromChunkList(parent, chunkList, EChunkDetachPolicy::SortedTablet);
            chunkList = parent;
        }
    }

    void DetachChunksFromTablet(
        TTabletBase* tablet,
        const std::vector<TChunkTree*>& chunkTrees,
        EChunkDetachPolicy policy)
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        if (policy == EChunkDetachPolicy::OrderedTabletPrefix ||
            policy == EChunkDetachPolicy::OrderedTabletSuffix)
        {
            chunkManager->DetachFromChunkList(tablet->GetChunkList(), chunkTrees, policy);
            return;
        }

        YT_VERIFY(
            policy == EChunkDetachPolicy::SortedTablet ||
            policy == EChunkDetachPolicy::HunkTablet);

        // Ensure deteministic ordering of keys.
        std::map<TChunkList*, std::vector<TChunkTree*>, TObjectIdComparer> childrenByParent;
        for (auto* child : chunkTrees) {
            auto* parent = GetTabletChildParent(tablet, child);
            YT_VERIFY(HasParent(child, parent));
            childrenByParent[parent].push_back(child);
        }

        for (const auto& [parent, children] : childrenByParent) {
            chunkManager->DetachFromChunkList(parent, children, policy);
            PruneEmptySubtabletChunkList(parent);
        }
    }

    void HydraCommitUpdateTabletStores(
        TTransaction* transaction,
        NProto::TReqUpdateTabletStores* request,
        const TTransactionCommitOptions& /*options*/)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        if (!CommitUpdateTabletStoresBase(transaction, tablet, request->mount_revision())) {
            return;
        }

        auto* owner = tablet->GetOwner();
        YT_VERIFY(IsObjectAlive(owner));

        TWallTimer timer;
        auto updateReason = FromProto<ETabletStoresUpdateReason>(request->update_reason());
        auto counters = GetCounters(updateReason, owner);
        auto updateTime = Finally([&] {
            counters->UpdateTabletStoreTime.Add(timer.GetElapsedTime());
        });

        CommitUpdateTabletStores(
            tablet->As<TTablet>(),
            transaction,
            request,
            updateReason);
    }

    void HydraCommitUpdateHunkTabletStores(
        TTransaction* transaction,
        NProto::TReqUpdateHunkTabletStores* request,
        const TTransactionCommitOptions& /*options*/)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        if (!CommitUpdateTabletStoresBase(transaction, tablet, request->mount_revision())) {
            return;
        }

        CommitUpdateHunkTabletStores(tablet->As<THunkTablet>(), transaction, request);
    }

    bool CommitUpdateTabletStoresBase(
        TTransaction* transaction,
        TTabletBase* tablet,
        NHydra::TRevision mountRevision)
    {
        if (tablet->GetStoresUpdatePreparedTransaction() != transaction) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet stores update commit for an improperly prepared tablet; ignored "
                "(TabletId: %v, ExpectedTransactionId: %v, ActualTransactionId: %v)",
                tablet->GetId(),
                transaction->GetId(),
                GetObjectId(tablet->GetStoresUpdatePreparedTransaction()));
            return false;
        }

        tablet->SetStoresUpdatePreparedTransaction(nullptr);

        if (tablet->GetMountRevision() != mountRevision) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Invalid mount revision on tablet stores update commit; ignored "
                "(TabletId: %v, TransactionId: %v, ExpectedMountRevision: %x, ActualMountRevision: %x)",
                tablet->GetId(),
                transaction->GetId(),
                mountRevision,
                tablet->GetMountRevision());
            return false;
        }

        auto* owner = tablet->GetOwner();
        if (!IsObjectAlive(owner)) {
            return false;
        }

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        cypressManager->SetModified(owner, EModificationType::Content);

        return true;
    }

    void CommitUpdateTabletStores(
        TTabletBase* tablet,
        TTransaction* transaction,
        NProto::TReqUpdateTabletStores* request,
        ETabletStoresUpdateReason updateReason)
    {
        switch (tablet->GetType()) {
            case EObjectType::Tablet: {
                CommitUpdateTabletStores(
                    tablet->As<TTablet>(),
                    transaction,
                    request,
                    updateReason);
                break;
            }
            default:
                YT_ABORT();
        }
    }

    void CommitUpdateTabletStores(
        TTablet* tablet,
        TTransaction* transaction,
        NProto::TReqUpdateTabletStores* request,
        ETabletStoresUpdateReason updateReason)
    {
        auto* table = tablet->GetTable();

        // Collect all changes first.
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        // Dynamic stores are also possible.
        std::vector<TChunkTree*> chunksToAttach;
        i64 attachedRowCount = 0;
        auto lastCommitTimestamp = table->GetLastCommitTimestamp();

        TChunk* flushedChunk = nullptr;

        auto validateChunkAttach = [&] (TChunk* chunk) {
            if (!IsObjectAlive(chunk)) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Attempt to attach a zombie chunk (ChunkId: %v)",
                    chunk->GetId());
            }
            if (chunk->HasParents()) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Attempt to attach a chunk that already has a parent (ChunkId: %v)",
                    chunk->GetId());
            }
        };

        for (const auto& descriptor : request->stores_to_add()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            auto type = TypeFromId(storeId);
            if (IsChunkTabletStoreType(type)) {
                auto* chunk = chunkManager->GetChunkOrThrow(storeId);
                validateChunkAttach(chunk);
                auto miscExt = chunk->ChunkMeta()->FindExtension<TMiscExt>();
                if (miscExt && miscExt->has_max_timestamp()) {
                    lastCommitTimestamp = std::max(lastCommitTimestamp, static_cast<TTimestamp>(miscExt->max_timestamp()));
                }

                attachedRowCount += chunk->GetRowCount();
                chunksToAttach.push_back(chunk);
            } else if (IsDynamicTabletStoreType(type)) {
                if (IsDynamicStoreReadEnabled(table)) {
                    YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Attempt to attach dynamic store to a table "
                        "with readable dynamic stores (TableId: %v, TabletId: %v, StoreId: %v, Reason: %v)",
                        table->GetId(),
                        tablet->GetId(),
                        storeId,
                        updateReason);
                }
            } else {
                YT_ABORT();
            }
        }

        for (const auto& descriptor : request->hunk_chunks_to_add()) {
            auto chunkId = FromProto<TChunkId>(descriptor.chunk_id());
            auto* chunk = chunkManager->GetChunkOrThrow(chunkId);
            validateChunkAttach(chunk);
            chunksToAttach.push_back(chunk);
        }

        if (updateReason == ETabletStoresUpdateReason::Flush) {
            YT_VERIFY(request->stores_to_add_size() <= 1);
            if (request->stores_to_add_size() == 1) {
                flushedChunk = chunksToAttach[0]->AsChunk();
            }

            if (request->request_dynamic_store_id()) {
                auto storeId = ReplaceTypeInId(
                    transaction->GetId(),
                    table->IsPhysicallySorted()
                        ? EObjectType::SortedDynamicTabletStore
                        : EObjectType::OrderedDynamicTabletStore);
                auto* dynamicStore = CreateDynamicStore(tablet, storeId);
                chunksToAttach.push_back(dynamicStore);
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Dynamic store attached to tablet during flush (TableId: %v, TabletId: %v, StoreId: %v)",
                    table->GetId(),
                    tablet->GetId(),
                    storeId);
            }
        }

        // Chunk views are also possible.
        std::vector<TChunkTree*> chunksOrViewsToDetach;
        i64 detachedRowCount = 0;
        bool flatteningRequired = false;
        for (const auto& descriptor : request->stores_to_remove()) {
            auto storeId = FromProto<TStoreId>(descriptor.store_id());
            if (IsChunkTabletStoreType(TypeFromId(storeId))) {
                auto* chunk = chunkManager->GetChunkOrThrow(storeId);
                detachedRowCount += chunk->GetRowCount();
                chunksOrViewsToDetach.push_back(chunk);
                flatteningRequired |= !CanUnambiguouslyDetachChild(tablet->GetChunkList(), chunk);
            } else if (TypeFromId(storeId) == EObjectType::ChunkView) {
                auto* chunkView = chunkManager->GetChunkViewOrThrow(storeId);
                auto* chunk = chunkView->GetUnderlyingTree()->AsChunk();
                detachedRowCount += chunk->GetRowCount();
                chunksOrViewsToDetach.push_back(chunkView);
                flatteningRequired |= !CanUnambiguouslyDetachChild(tablet->GetChunkList(), chunkView);
            } else if (IsDynamicTabletStoreType(TypeFromId(storeId))) {
                if (auto* dynamicStore = chunkManager->FindDynamicStore(storeId)) {
                    YT_VERIFY(updateReason == ETabletStoresUpdateReason::Flush);
                    dynamicStore->SetFlushedChunk(flushedChunk);
                    if (!table->IsSorted()) {
                        // NB: Dynamic stores at the end of the chunk list do not contribute to row count,
                        // so the logical row count of the chunk list is exactly the number of rows
                        // in all tablet chunks.
                        dynamicStore->SetTableRowIndex(tablet->GetChunkList()->Statistics().LogicalRowCount);
                    }
                    chunksOrViewsToDetach.push_back(dynamicStore);
                }
            } else {
                YT_ABORT();
            }
        }

        for (const auto& descriptor : request->hunk_chunks_to_remove()) {
            auto chunkId = FromProto<TStoreId>(descriptor.chunk_id());
            auto* chunk = chunkManager->GetChunkOrThrow(chunkId);
            chunksOrViewsToDetach.push_back(chunk);
        }

        // Update last commit timestamp.
        table->SetLastCommitTimestamp(lastCommitTimestamp);

        // Update retained timestamp.
        auto retainedTimestamp = std::max(
            tablet->GetRetainedTimestamp(),
            static_cast<TTimestamp>(request->retained_timestamp()));
        tablet->SetRetainedTimestamp(retainedTimestamp);

        // Copy chunk trees if somebody holds a reference or if children cannot be detached unambiguously.
        CopyChunkListsIfShared(table, tablet->GetIndex(), tablet->GetIndex(), flatteningRequired);

        // Save old tablet resource usage.
        auto oldMemorySize = tablet->GetTabletStaticMemorySize();
        auto oldStatistics = tablet->GetTabletStatistics();

        // Apply all requested changes.
        auto* tabletChunkList = tablet->GetChunkList();
        auto* cell = tablet->GetCell();

        if (!table->IsPhysicallySorted() && IsDynamicStoreReadEnabled(table) && updateReason == ETabletStoresUpdateReason::Flush) {
            // NB: Flushing ordered tablet requires putting a certain chunk in place of a certain dynamic store.

            const auto& children = tabletChunkList->Children();
            YT_VERIFY(!children.empty());

            auto* dynamicStoreToRemove = chunksOrViewsToDetach[0]->AsDynamicStore();
            int firstDynamicStoreIndex = GetFirstDynamicStoreIndex(tabletChunkList);
            YT_VERIFY(dynamicStoreToRemove == children[firstDynamicStoreIndex]);

            std::vector<TChunkTree*> allDynamicStores(
                children.begin() + firstDynamicStoreIndex,
                children.end());

            // +2 is due to that the accounting is not very precise at the node part.
            if (allDynamicStores.size() > NTabletNode::DynamicStoreCountLimit + 2) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Too many dynamic stores in ordered tablet chunk list "
                    "(TableId: %v, TabletId: %v, ChunkListId: %v, DynamicStoreCount: %v, "
                    "Limit: %v)",
                    table->GetId(),
                    tablet->GetId(),
                    tabletChunkList->GetId(),
                    allDynamicStores.size(),
                    NTabletNode::DynamicStoreCountLimit + 2);
            }

            chunkManager->DetachFromChunkList(
                tabletChunkList,
                allDynamicStores,
                EChunkDetachPolicy::OrderedTabletSuffix);

            if (flushedChunk) {
                chunkManager->AttachToChunkList(tabletChunkList, flushedChunk);
            }

            allDynamicStores.erase(allDynamicStores.begin());
            chunkManager->AttachToChunkList(tabletChunkList, allDynamicStores);

            if (request->request_dynamic_store_id()) {
                auto* dynamicStoreToAdd = chunksToAttach.back()->AsDynamicStore();
                chunkManager->AttachToChunkList(tabletChunkList, dynamicStoreToAdd);
            }
        } else {
            AttachChunksToTablet(tablet, chunksToAttach);
            DetachChunksFromTablet(
                tablet,
                chunksOrViewsToDetach,
                updateReason == ETabletStoresUpdateReason::Trim
                    ? EChunkDetachPolicy::OrderedTabletPrefix
                    : EChunkDetachPolicy::SortedTablet);
        }

        table->SnapshotStatistics() = {};
        for (auto contentType : TEnumTraits<EChunkListContentType>::GetDomainValues()) {
            table->SnapshotStatistics() += table->GetChunkList(contentType)->Statistics().ToDataStatistics();
        }

        // Get new tablet resource usage.
        auto newMemorySize = tablet->GetTabletStaticMemorySize();
        auto newStatistics = tablet->GetTabletStatistics();
        auto deltaStatistics = newStatistics - oldStatistics;

        // Update cell and table statistics.
        cell->GossipStatistics().Local() += deltaStatistics;
        table->DiscountTabletStatistics(oldStatistics);
        table->AccountTabletStatistics(newStatistics);

        // Update table resource usage.

        // Unstage just attached chunks.
        for (auto* chunk : chunksToAttach) {
            if (IsChunkTabletStoreType(chunk->GetType())) {
                chunkManager->UnstageChunk(chunk->AsChunk());
            }
        }

        // Requisition update pursues two goals: updating resource usage and
        // setting requisitions to correct values. The latter is required both
        // for detached chunks (for obvious reasons) and attached chunks
        // (because the protocol doesn't allow for creating chunks with correct
        // requisitions from the start).
        for (auto* chunk : chunksToAttach) {
            chunkManager->ScheduleChunkRequisitionUpdate(chunk);
        }
        for (auto* chunk : chunksOrViewsToDetach) {
            chunkManager->ScheduleChunkRequisitionUpdate(chunk);
        }

        UpdateResourceUsage(
            table,
            TTabletResources().SetTabletStaticMemory(newMemorySize - oldMemorySize));

        auto counters = GetCounters(updateReason, table);
        counters->UpdateTabletStoresStoreCount.Increment(chunksToAttach.size() + chunksOrViewsToDetach.size());

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet stores update committed (TransactionId: %v, TableId: %v, TabletId: %v, "
            "AttachedChunkIds: %v, DetachedChunkOrViewIds: %v, "
            "AttachedRowCount: %v, DetachedRowCount: %v, RetainedTimestamp: %v, UpdateReason: %v)",
            transaction->GetId(),
            table->GetId(),
            tablet->GetId(),
            MakeFormattableView(chunksToAttach, TObjectIdFormatter()),
            MakeFormattableView(chunksOrViewsToDetach, TObjectIdFormatter()),
            attachedRowCount,
            detachedRowCount,
            retainedTimestamp,
            updateReason);
    }

    void CommitUpdateHunkTabletStores(
        THunkTablet* tablet,
        TTransaction* transaction,
        NProto::TReqUpdateHunkTabletStores* request)
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();

        // Save old tablet resource usage.
        auto oldStatistics = tablet->GetTabletStatistics();

        std::vector<TChunkTree*> chunksToAdd;
        chunksToAdd.reserve(request->stores_to_add_size());
        for (const auto& storeToAdd : request->stores_to_add()) {
            auto chunkId = FromProto<NChunkClient::TSessionId>(storeToAdd.session_id()).ChunkId;
            auto* chunk = chunkManager->FindChunk(chunkId);
            if (!IsObjectAlive(chunk)) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                    "Requested to attach dead chunk to hunk tablet; ignored "
                    "(ChunkId: %v, HunkStorageId: %v, TableId: %v)",
                    chunk->GetId(),
                    tablet->GetId(),
                    tablet->GetOwner()->GetId());
                continue;
            }

            chunksToAdd.push_back(chunk);
        }

        AttachToTabletChunkLists(tablet, chunksToAdd);

        std::vector<TChunkTree*> chunksToRemove;
        chunksToRemove.reserve(request->stores_to_remove_size());
        for (const auto& storeToRemove : request->stores_to_remove()) {
            auto chunkId = FromProto<TChunkId>(storeToRemove.store_id());
            auto* chunk = chunkManager->FindChunk(chunkId);
            if (!IsObjectAlive(chunk)) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                    "Requested to detach dead chunk from hunk tablet; ignored "
                    "(ChunkId: %v, TabletId: %v, HunkStorageId: %v)",
                    chunk->GetId(),
                    tablet->GetId(),
                    tablet->GetOwner()->GetId());
                continue;
            }

            chunksToRemove.push_back(chunk);
        }

        DetachChunksFromTablet(tablet, chunksToRemove, EChunkDetachPolicy::HunkTablet);

        std::vector<TChunk*> chunksToMarkSealable;
        for (const auto& chunkToMarkSealable : request->stores_to_mark_sealable()) {
            auto chunkId = FromProto<TChunkId>(chunkToMarkSealable.store_id());
            auto* chunk = chunkManager->GetChunk(chunkId);
            if (!IsObjectAlive(chunk)) {
                YT_LOG_ALERT_IF(IsMutationLoggingEnabled(),
                    "Requested to mark dead chunk as sealable; ignored "
                    "(ChunkId: %v, TabletId: %v, HunkStorageId: %v)",
                    chunk->GetId(),
                    tablet->GetId(),
                    tablet->GetOwner()->GetId());
                continue;
            }

            chunksToMarkSealable.push_back(chunk);
        }

        for (auto* chunk : chunksToMarkSealable) {
            chunk->SetSealable(true);
            chunkManager->ScheduleChunkSeal(chunk);
        }

        tablet->SetStoresUpdatePreparedTransaction(nullptr);

        auto newStatistics = tablet->GetTabletStatistics();
        auto deltaStatistics = newStatistics - oldStatistics;

        auto* cell = tablet->GetCell();
        cell->GossipStatistics().Local() += deltaStatistics;

        auto* owner = tablet->GetOwner();
        owner->DiscountTabletStatistics(oldStatistics);
        owner->AccountTabletStatistics(newStatistics);

        for (auto* chunk : chunksToAdd) {
            chunkManager->ScheduleChunkRequisitionUpdate(chunk);
        }
        for (auto* chunk : chunksToRemove) {
            chunkManager->ScheduleChunkRequisitionUpdate(chunk);
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Hunk tablet stores update committed (TransactionId: %v, HunkStorageId: %v, TabletId: %v, "
            "AddedChunkIds: %v, RemovedChunkIds: %v, MarkSealableChunkIds: %v)",
            transaction->GetId(),
            tablet->GetOwner()->GetId(),
            tablet->GetId(),
            MakeFormattableView(chunksToAdd, TObjectIdFormatter()),
            MakeFormattableView(chunksToRemove, TObjectIdFormatter()),
            MakeFormattableView(chunksToMarkSealable, TObjectIdFormatter()));
    }

    void HydraAbortUpdateTabletStores(
        TTransaction* transaction,
        NProto::TReqUpdateTabletStores* request,
        const TTransactionAbortOptions& /*options*/)
    {
        AbortUpdateTabletStores(
            transaction,
            FromProto<TTabletId>(request->tablet_id()));
    }

    void HydraAbortUpdateHunkTabletStores(
        TTransaction* transaction,
        NProto::TReqUpdateHunkTabletStores* request,
        const TTransactionAbortOptions& /*options*/)
    {
        AbortUpdateTabletStores(
            transaction,
            FromProto<TTabletId>(request->tablet_id()));
    }

    void AbortUpdateTabletStores(
        TTransaction* transaction,
        TTabletId tabletId)
    {
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        if (tablet->GetStoresUpdatePreparedTransaction() != transaction) {
            return;
        }

        tablet->SetStoresUpdatePreparedTransaction(nullptr);

        const auto* table = tablet->GetOwner();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
            "Tablet stores update aborted (TransactionId: %v, TableId: %v, TabletId: %v)",
            transaction->GetId(),
            table->GetId(),
            tabletId);
    }

    void HydraUpdateTabletTrimmedRowCount(NProto::TReqUpdateTabletTrimmedRowCount* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        YT_VERIFY(tablet->GetType() == EObjectType::Tablet);

        auto mountRevision = request->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        if (tablet->GetState() == ETabletState::Unmounted) {
            return;
        }

        auto trimmedRowCount = request->trimmed_row_count();

        tablet->As<TTablet>()->SetTrimmedRowCount(trimmedRowCount);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Tablet trimmed row count updated (TabletId: %v, TrimmedRowCount: %v)",
            tabletId,
            trimmedRowCount);
    }

    void HydraAllocateDynamicStore(NProto::TReqAllocateDynamicStore* request)
    {
        auto tabletId = FromProto<TTabletId>(request->tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        YT_VERIFY(tablet->GetType() == EObjectType::Tablet);

        auto mountRevision = request->mount_revision();
        if (tablet->GetMountRevision() != mountRevision) {
            return;
        }

        auto* dynamicStore = CreateDynamicStore(tablet->As<TTablet>());
        AttachDynamicStoreToTablet(tablet->As<TTablet>(), dynamicStore);

        TRspAllocateDynamicStore rsp;
        ToProto(rsp.mutable_dynamic_store_id(), dynamicStore->GetId());
        ToProto(rsp.mutable_tablet_id(), tabletId);
        rsp.set_mount_revision(tablet->GetMountRevision());

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Dynamic store allocated (StoreId: %v, TabletId: %v, TableId: %v)",
            dynamicStore->GetId(),
            tabletId,
            tablet->As<TTablet>()->GetTable()->GetId());

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        auto* mailbox = hiveManager->GetMailbox(tablet->GetCell()->GetId());
        hiveManager->PostMessage(mailbox, rsp);
    }

    void HydraCreateTabletAction(NProto::TReqCreateTabletAction* request)
    {
        auto kind = ETabletActionKind(request->kind());
        auto tabletIds = FromProto<std::vector<TTabletId>>(request->tablet_ids());
        auto cellIds = FromProto<std::vector<TTabletId>>(request->cell_ids());
        auto pivotKeys = FromProto<std::vector<TLegacyOwningKey>>(request->pivot_keys());
        TInstant expirationTime = TInstant::Zero();
        if (request->has_expiration_time()) {
            expirationTime = FromProto<TInstant>(request->expiration_time());
        }
        auto expirationTimeout = request->has_expiration_timeout()
            ? std::make_optional(FromProto<TDuration>(request->expiration_timeout()))
            : std::nullopt;
        std::optional<int> tabletCount = request->has_tablet_count()
            ? std::make_optional(request->tablet_count())
            : std::nullopt;

        TGuid correlationId;
        if (request->has_correlation_id()) {
            FromProto(&correlationId, request->correlation_id());
        }

        std::vector<TTabletBase*> tablets;
        std::vector<TTabletCell*> cells;

        for (auto tabletId : tabletIds) {
            tablets.push_back(GetTabletOrThrow(tabletId));
        }

        for (auto cellId : cellIds) {
            cells.push_back(GetTabletCellOrThrow(cellId));
        }

        try {
            CreateTabletAction(
                NullObjectId,
                kind,
                tablets,
                cells,
                pivotKeys,
                tabletCount,
                /*skipFreezing*/ false,
                correlationId,
                expirationTime,
                expirationTimeout);
        } catch (const std::exception& ex) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), TError(ex), "Error creating tablet action (Kind: %v, "
                "Tablets: %v, TabletCells: %v, PivotKeys: %v, TabletCount: %v, TabletBalancerCorrelationId: %v)",
                kind,
                tablets,
                cells,
                pivotKeys,
                tabletCount,
                correlationId);
        }
    }

    void HydraDestroyTabletActions(NProto::TReqDestroyTabletActions* request)
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        auto actionIds = FromProto<std::vector<TTabletActionId>>(request->tablet_action_ids());
        for (const auto& id : actionIds) {
            auto* action = FindTabletAction(id);
            if (IsObjectAlive(action)) {
                UnbindTabletAction(action);
                objectManager->UnrefObject(action);
            }
        }
    }

    void HydraSetTabletCellBundleResourceUsage(NProto::TReqSetTabletCellBundleResourceUsage* request)
    {
        auto cellTag = request->cell_tag();
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        YT_VERIFY(multicellManager->IsPrimaryMaster() || cellTag == multicellManager->GetPrimaryCellTag());

        if (!multicellManager->IsRegisteredMasterCell(cellTag)) {
            YT_LOG_ERROR_IF(IsMutationLoggingEnabled(), "Received tablet cell bundle "
                "resource usage gossip message from unknown cell (CellTag: %v)",
                cellTag);
            return;
        }

        YT_LOG_INFO_IF(IsMutationLoggingEnabled(), "Received tablet cell bundle "
            "resource usage gossip message (CellTag: %v)",
            cellTag);

        for (const auto& entry : request->entries()) {
            auto bundleId = FromProto<TTabletCellBundleId>(entry.bundle_id());
            auto* bundle = FindTabletCellBundle(bundleId);
            if (!IsObjectAlive(bundle)) {
                continue;
            }

            auto newResourceUsage = FromProto<TTabletResources>(entry.resource_usage());
            if (multicellManager->IsPrimaryMaster()) {
                *bundle->ResourceUsage().Remote(cellTag) = newResourceUsage;
            } else {
                bundle->ResourceUsage().Cluster() = newResourceUsage;
            }
        }
    }

    void HydraUpdateTabletCellBundleResourceUsage(NProto::TReqUpdateTabletCellBundleResourceUsage* /*request*/)
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        const auto& cellManager = Bootstrap_->GetTamedCellManager();

        YT_VERIFY(multicellManager->IsPrimaryMaster());

        for (auto* bundleBase : GetValuesSortedByKey(cellManager->CellBundles(ECellarType::Tablet))) {
            if (!IsObjectAlive(bundleBase)) {
                continue;
            }

            YT_VERIFY(bundleBase->GetType() == EObjectType::TabletCellBundle);
            auto* bundle = bundleBase->As<TTabletCellBundle>();
            bundle->RecomputeClusterResourceUsage();
        }
    }

    void OnTabletCellBundleResourceUsageGossip()
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (!multicellManager->IsLocalMasterCellRegistered()) {
            return;
        }

        YT_LOG_INFO("Sending tablet cell bundle resource usage gossip");

        NProto::TReqSetTabletCellBundleResourceUsage request;
        request.set_cell_tag(multicellManager->GetCellTag());

        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        for (auto* bundleBase : cellManager->CellBundles(ECellarType::Tablet)) {
            if (!IsObjectAlive(bundleBase)) {
                continue;
            }

            YT_VERIFY(bundleBase->GetType() == EObjectType::TabletCellBundle);
            auto* bundle = bundleBase->As<TTabletCellBundle>();
            auto* entry = request.add_entries();
            ToProto(entry->mutable_bundle_id(), bundle->GetId());

            if (multicellManager->IsPrimaryMaster()) {
                ToProto(entry->mutable_resource_usage(), bundle->ResourceUsage().Cluster());
            } else {
                ToProto(entry->mutable_resource_usage(), bundle->ResourceUsage().Local());
            }
        }

        if (multicellManager->IsPrimaryMaster()) {
            multicellManager->PostToSecondaryMasters(request, false);
        } else {
            multicellManager->PostToMaster(request, PrimaryMasterCellTagSentinel, false);
        }

        if (multicellManager->IsMulticell() && multicellManager->IsPrimaryMaster()) {
            NProto::TReqUpdateTabletCellBundleResourceUsage request;
            const auto& hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
            CreateMutation(hydraManager, request)
                ->CommitAndLog(Logger);
        }
    }

    void ValidateResourceUsageIncrease(
        const TTabletOwnerBase* table,
        const TTabletResources& delta,
        TAccount* account = nullptr) const
    {
        // Old-fashioned account validation.
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidateResourceUsageIncrease(
            account ? account : table->GetAccount(),
            ConvertToClusterResources(delta));

        // Brand-new bundle validation.
        if (GetDynamicConfig()->EnableTabletResourceValidation) {
            const auto& bundle = table->TabletCellBundle();
            if (!bundle) {
                YT_LOG_ALERT("Failed to validate tablet resource usage increase since table lacks tablet cell bundle "
                    "(TableId: %v, Delta: %v)",
                    table->GetId(),
                    delta);
                return;
            }
            bundle->ValidateResourceUsageIncrease(delta);
        }
    }

    void UpdateResourceUsage(
        TTabletOwnerBase* table,
        const TTabletResources& delta,
        bool scheduleTableDataStatisticsUpdate = true)
    {
        // Old-fashioned account accounting.
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->UpdateTabletResourceUsage(
            table,
            ConvertToClusterResources(delta));

        // Brand-new bundle accounting.
        const auto& bundle = table->TabletCellBundle();
        if (!bundle) {
            YT_LOG_ALERT("Failed to update tablet resource usage since table lacks tablet cell bundle "
                "(TableId: %v, Delta: %v)",
                table->GetId(),
                delta);
            return;
        }
        bundle->UpdateResourceUsage(delta);

        const auto& tableManager = Bootstrap_->GetTableManager();
        tableManager->ScheduleStatisticsUpdate(table, scheduleTableDataStatisticsUpdate);
    }

    void OnProfiling()
    {
        if (!IsLeader()) {
            return;
        }

        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        if (!multicellManager->IsPrimaryMaster()) {
            return;
        }

        const auto& cellManager = Bootstrap_->GetTamedCellManager();

        for (auto* bundleBase : cellManager->CellBundles(ECellarType::Tablet)) {
            if (!IsObjectAlive(bundleBase)) {
                continue;
            }

            YT_VERIFY(bundleBase->GetType() == EObjectType::TabletCellBundle);
            auto* bundle = bundleBase->As<TTabletCellBundle>();
            auto counters = GetOrCreateBundleProfilingCounters(bundle);
            bundle->OnProfiling(&counters);
        }
    }

    void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnLeaderActive();

        const auto& dynamicConfig = GetDynamicConfig();

        TabletCellDecommissioner_->Start();
        TabletBalancer_->Start();
        TabletActionManager_->Start();

        TabletCellStatisticsGossipExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::TabletGossip),
            BIND(&TImpl::OnTabletCellStatisticsGossip, MakeWeak(this)),
            dynamicConfig->MulticellGossip->TabletCellStatisticsGossipPeriod);
        TabletCellStatisticsGossipExecutor_->Start();

        BundleResourceUsageGossipExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::TabletGossip),
            BIND(&TImpl::OnTabletCellBundleResourceUsageGossip, MakeWeak(this)),
            dynamicConfig->MulticellGossip->BundleResourceUsageGossipPeriod);
        BundleResourceUsageGossipExecutor_->Start();

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::Periodic),
            BIND(&TImpl::OnProfiling, MakeWeak(this)),
            dynamicConfig->ProfilingPeriod);
        ProfilingExecutor_->Start();

        Bootstrap_->GetNewReplicatedTableTracker()->EnableTracking();
    }

    void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnStopLeading();

        TabletCellDecommissioner_->Stop();
        TabletBalancer_->Stop();
        TabletActionManager_->Stop();

        if (TabletCellStatisticsGossipExecutor_) {
            TabletCellStatisticsGossipExecutor_->Stop();
            TabletCellStatisticsGossipExecutor_.Reset();
        }

        if (BundleResourceUsageGossipExecutor_) {
            BundleResourceUsageGossipExecutor_->Stop();
            BundleResourceUsageGossipExecutor_.Reset();
        }

        if (ProfilingExecutor_) {
            ProfilingExecutor_->Stop();
            ProfilingExecutor_.Reset();
        }

        BundleIdToProfilingCounters_.clear();

        Bootstrap_->GetNewReplicatedTableTracker()->DisableTracking();
    }

    void OnRecoveryComplete() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnRecoveryComplete();

        Bootstrap_->GetNewReplicatedTableTracker()->Initialize();
    }


    bool CheckHasHealthyCells(TTabletCellBundle* bundle)
    {
        for (auto* cellBase : bundle->Cells()) {
            if (cellBase->GetType() != EObjectType::TabletCell) {
                continue;
            }

            auto* cell = cellBase->As<TTabletCell>();
            if (!IsCellActive(cell)) {
                continue;
            }
            if (cell->IsHealthy()) {
                return true;
            }
        }

        return false;
    }

    void ValidateHasHealthyCells(TTabletCellBundle* bundle)
    {
        if (!CheckHasHealthyCells(bundle)) {
            THROW_ERROR_EXCEPTION("No healthy tablet cells in bundle %Qv",
                bundle->GetName());
        }
    }

    bool IsCellActive(TTabletCell* cell)
    {
        return IsObjectAlive(cell) && !cell->IsDecommissionStarted();
    }

    TTabletCell* FindTabletCell(TTabletCellId id)
    {
        const auto& cellManager = Bootstrap_->GetTamedCellManager();
        auto* cell = cellManager->FindCell(id);
        if (cell && cell->GetType() != EObjectType::TabletCell) {
            return nullptr;
        }
        return cell->As<TTabletCell>();
    }


    std::vector<std::pair<TTabletBase*, TTabletCell*>> ComputeTabletAssignment(
        TTabletOwnerBase* table,
        TTabletCell* hintCell,
        std::vector<TTabletBase*> tabletsToMount)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (IsCellActive(hintCell)) {
            std::vector<std::pair<TTabletBase*, TTabletCell*>> assignment;
            for (auto* tablet : tabletsToMount) {
                assignment.emplace_back(tablet, hintCell);
            }
            return assignment;
        }

        struct TCellKey
        {
            i64 Size;
            TTabletCell* Cell;

            //! Compares by |(size, cellId)|.
            bool operator < (const TCellKey& other) const
            {
                if (Size < other.Size) {
                    return true;
                } else if (Size > other.Size) {
                    return false;
                }
                return Cell->GetId() < other.Cell->GetId();
            }
        };

        auto mutationContext = GetCurrentMutationContext();

        auto getCellSize = [&] (const TTabletCell* cell) -> i64 {
            i64 result = 0;
            i64 tabletCount;
            switch (table->GetInMemoryMode()) {
                case EInMemoryMode::None:
                    result = mutationContext->RandomGenerator()->Generate<i64>();
                    break;
                case EInMemoryMode::Uncompressed:
                case EInMemoryMode::Compressed: {
                    result += cell->GossipStatistics().Local().MemorySize;
                    tabletCount = cell->GossipStatistics().Local().TabletCountPerMemoryMode[EInMemoryMode::Uncompressed] +
                        cell->GossipStatistics().Local().TabletCountPerMemoryMode[EInMemoryMode::Compressed];
                    result += tabletCount * GetDynamicConfig()->TabletDataSizeFootprint;
                    break;
                }
                default:
                    YT_ABORT();
            }
            return result;
        };

        std::vector<TCellKey> cellKeys;
        for (auto* cellBase : GetValuesSortedByKey(table->TabletCellBundle()->Cells())) {
            if (cellBase->GetType() != EObjectType::TabletCell) {
                continue;
            }

            auto* cell = cellBase->As<TTabletCell>();
            if (!IsCellActive(cell)) {
                continue;
            }

            if (cell->CellBundle() == table->TabletCellBundle()) {
                cellKeys.push_back(TCellKey{getCellSize(cell), cell});
            }
        }
        if (cellKeys.empty()) {
            cellKeys.push_back(TCellKey{0, nullptr});
        }
        std::sort(cellKeys.begin(), cellKeys.end());

        auto getTabletSize = [&] (const TTabletBase* tablet) -> i64 {
            i64 result = 0;
            auto statistics = tablet->GetTabletStatistics();
            switch (table->GetInMemoryMode()) {
                case EInMemoryMode::None:
                case EInMemoryMode::Uncompressed:
                    result += statistics.UncompressedDataSize;
                    break;
                case EInMemoryMode::Compressed:
                    result += statistics.CompressedDataSize;
                    break;
                default:
                    YT_ABORT();
            }
            result += GetDynamicConfig()->TabletDataSizeFootprint;
            return result;
        };

        // Sort tablets by decreasing size to improve greedy heuristic performance.
        std::sort(
            tabletsToMount.begin(),
            tabletsToMount.end(),
            [&] (const TTabletBase* lhs, const TTabletBase* rhs) {
                return
                    std::make_tuple(getTabletSize(lhs), lhs->GetId()) >
                    std::make_tuple(getTabletSize(rhs), rhs->GetId());
            });

        // Assign tablets to cells iteratively looping over cell array.
        int cellIndex = 0;
        std::vector<std::pair<TTabletBase*, TTabletCell*>> assignment;
        for (auto* tablet : tabletsToMount) {
            assignment.emplace_back(tablet, cellKeys[cellIndex].Cell);
            if (++cellIndex == std::ssize(cellKeys)) {
                cellIndex = 0;
            }
        }

        return assignment;
    }

    void DoUnmount(
        TTabletOwnerBase* table,
        bool force,
        int firstTabletIndex,
        int lastTabletIndex,
        bool onDestroy)
    {
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            UnmountTablet(tablet, force, onDestroy);
        }
    }

    void DoUnmountTabletBase(TTabletBase* tablet, bool force)
    {
        auto state = tablet->GetState();
        if (!force) {
            YT_VERIFY(
                state == ETabletState::Mounted ||
                state == ETabletState::Frozen ||
                state == ETabletState::Freezing ||
                state == ETabletState::Unmounting);
        }

        auto* table = tablet->GetOwner();

        auto* cell = tablet->GetCell();
        YT_VERIFY(cell);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Unmounting tablet (TableId: %v, TabletId: %v, CellId: %v, Force: %v)",
            table->GetId(),
            tablet->GetId(),
            cell->GetId(),
            force);

        tablet->SetState(ETabletState::Unmounting);
    }

    void DoUnmountTablet(
        TTablet* tablet,
        bool force,
        bool onDestroy)
    {
        if (tablet->GetState() == ETabletState::Unmounted) {
            return;
        }

        // NB: Cell can be destroyed.
        auto cellId = tablet->GetCell()->GetId();

        DoUnmountTabletBase(tablet, force);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        {
            TReqUnmountTablet request;
            ToProto(request.mutable_tablet_id(), tablet->GetId());
            request.set_force(force);
            auto* mailbox = hiveManager->GetMailbox(cellId);
            hiveManager->PostMessage(mailbox, request);
        }

        for (auto it : GetIteratorsSortedByKey(tablet->Replicas())) {
            auto* replica = it->first;
            auto& replicaInfo = it->second;
            if (replica->TransitioningTablets().count(tablet) > 0) {
                StopReplicaTransition(tablet, replica, &replicaInfo, ETableReplicaState::None);
            }
            CheckTransitioningReplicaTablets(replica);
        }

        if (force) {
            AbandonDynamicStores(tablet);
            // NB: CopyChunkListIfShared may be called. It expects the table to be the owning node
            // of the root chunk list, which is not the case upon destruction.
            if (!onDestroy) {
                DiscardDynamicStores(tablet);
            }
            SetTabletEdenStoreIds(tablet, {});
            DoTabletUnmounted(tablet, /*force*/ true);
        }
    }

    void DoUnmountHunkTablet(THunkTablet* tablet, bool force)
    {
        if (tablet->GetState() == ETabletState::Unmounted) {
            return;
        }

        // NB: Cell can be destroyed in DoUnmountTabletBase.
        auto cellId = tablet->GetCell()->GetId();

        DoUnmountTabletBase(tablet, force);

        const auto& hiveManager = Bootstrap_->GetHiveManager();
        {
            TReqUnmountHunkTablet request;
            ToProto(request.mutable_tablet_id(), tablet->GetId());
            request.set_force(force);
            auto* mailbox = hiveManager->GetMailbox(cellId);
            hiveManager->PostMessage(mailbox, request);
        }

        if (force) {
            DoTabletUnmounted(tablet, force);
        }
    }

    void UnmountTablet(
        TTabletBase* tablet,
        bool force,
        bool onDestroy)
    {
        switch (tablet->GetType()) {
            case EObjectType::Tablet:
                DoUnmountTablet(tablet->As<TTablet>(), force, onDestroy);
                break;
            case EObjectType::HunkTablet:
                DoUnmountHunkTablet(tablet->As<THunkTablet>(), force);
                break;
            default:
                YT_ABORT();
        }
    }

    void ValidateTabletStaticMemoryUpdate(
        const TTabletOwnerBase* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        i64 memorySize = 0;

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            const auto* tablet = table->Tablets()[index];
            if (tablet->GetState() != ETabletState::Unmounted) {
                continue;
            }
            memorySize += tablet->GetTabletStaticMemorySize(table->GetInMemoryMode());
        }

        ValidateResourceUsageIncrease(
            table,
            TTabletResources().SetTabletStaticMemory(memorySize));
    }

    void ValidateTableMountConfig(
        const TTableNode* table,
        const TTableMountConfigPtr& mountConfig)
    {
        if (table->IsPhysicallyLog() && mountConfig->InMemoryMode != EInMemoryMode::None) {
            THROW_ERROR_EXCEPTION("Cannot mount dynamic table of type %Qlv in memory",
                table->GetType());
        }
        if (!table->IsPhysicallySorted() && mountConfig->EnableLookupHashTable) {
            THROW_ERROR_EXCEPTION("\"enable_lookup_hash_table\" can be \"true\" only for sorted dynamic table");
        }

        if (Bootstrap_->GetConfigManager()->GetConfig()->TabletManager->ForbidArbitraryDataVersionsInRetentionConfig) {
            if (mountConfig->MinDataVersions > 1) {
                THROW_ERROR_EXCEPTION("\"min_data_versions\" must be not greater than 1");
            }

            if (mountConfig->MaxDataVersions > 1) {
                THROW_ERROR_EXCEPTION("\"max_data_versions\" must be not greater than 1");
            }

            if (mountConfig->MinDataVersions > mountConfig->MaxDataVersions) {
                THROW_ERROR_EXCEPTION("\"min_data_versions\" must be not greater than \"max_data_versions\"");
            }
        }
    }

    bool IsDynamicStoreReadEnabled(const TTableNode* table)
    {
        if (table->IsPhysicallyLog() && !table->IsReplicated()) {
            return false;
        }

        if (table->GetActualTabletState() == ETabletState::Unmounted) {
            return table->GetEnableDynamicStoreRead().value_or(
                GetDynamicConfig()->EnableDynamicStoreReadByDefault);
        } else {
            return table->GetMountedWithEnabledDynamicStoreRead();
        }
    }

    static TError TryParseTabletRange(
        const TTabletOwnerBase* table,
        int* first,
        int* last)
    {
        const auto& tablets = table->Tablets();
        if (*first == -1 && *last == -1) {
            *first = 0;
            *last = static_cast<int>(tablets.size() - 1);
        } else {
            if (*first < 0 || *first >= std::ssize(tablets)) {
                return TError("First tablet index %v is out of range [%v, %v]",
                    *first,
                    0,
                    tablets.size() - 1);
            }
            if (*last < 0 || *last >= std::ssize(tablets)) {
                return TError("Last tablet index %v is out of range [%v, %v]",
                    *last,
                    0,
                    tablets.size() - 1);
            }
            if (*first > *last) {
               return TError("First tablet index is greater than last tablet index");
            }
        }

        return TError();
    }

    static void ParseTabletRange(
        TTabletOwnerBase* owner,
        int* first,
        int* last)
    {
        auto error = TryParseTabletRange(owner, first, last);
        YT_VERIFY(error.IsOK());
    }

    std::pair<std::vector<TTablet*>::iterator, std::vector<TTablet*>::iterator> GetIntersectingTablets(
        std::vector<TTablet*>& tablets,
        const NChunkClient::TLegacyReadRange readRange)
    {
        YT_VERIFY(readRange.LowerLimit().HasLegacyKey());
        YT_VERIFY(readRange.UpperLimit().HasLegacyKey());
        const auto& minKey = readRange.LowerLimit().GetLegacyKey();
        const auto& maxKey = readRange.UpperLimit().GetLegacyKey();

        auto beginIt = std::upper_bound(
            tablets.begin(),
            tablets.end(),
            minKey,
            [] (const TLegacyOwningKey& key, const TTablet* tablet) {
                return key < tablet->GetPivotKey();
            });

        if (beginIt != tablets.begin()) {
            --beginIt;
        }

        auto endIt = beginIt;
        while (endIt != tablets.end() && maxKey > (*endIt)->GetPivotKey()) {
            ++endIt;
        }

        return std::make_pair(beginIt, endIt);
    }

    static EStoreType GetStoreType(
        const TTableNode* table,
        const TChunkTree* chunkOrView)
    {
        if (IsPhysicalChunkType(chunkOrView->GetType())) {
            const auto* chunk = chunkOrView->AsChunk();
            if (chunk->GetChunkType() == EChunkType::Hunk) {
                return EStoreType::HunkChunk;
            }
        }
        return table->IsPhysicallySorted() ? EStoreType::SortedChunk : EStoreType::OrderedChunk;
    }

    void FillStoreDescriptor(
        const TTableNode* table,
        const TChunkTree* chunkOrView,
        NTabletNode::NProto::TAddStoreDescriptor* descriptor,
        i64* startingRowIndex)
    {
        descriptor->set_store_type(ToProto<int>(GetStoreType(table, chunkOrView)));
        ToProto(descriptor->mutable_store_id(), chunkOrView->GetId());

        const TChunk* chunk;
        if (chunkOrView->GetType() == EObjectType::ChunkView) {
            const auto* chunkView = chunkOrView->AsChunkView();
            chunk = chunkView->GetUnderlyingTree()->AsChunk();
            auto* viewDescriptor = descriptor->mutable_chunk_view_descriptor();
            ToProto(viewDescriptor->mutable_chunk_view_id(), chunkView->GetId());
            ToProto(viewDescriptor->mutable_underlying_chunk_id(), chunk->GetId());
            ToProto(viewDescriptor->mutable_read_range(), chunkView->ReadRange());

            const auto& transactionManager = Bootstrap_->GetTransactionManager();
            if (auto overrideTimestamp = transactionManager->GetTimestampHolderTimestamp(
                chunkView->GetTransactionId()))
            {
                viewDescriptor->set_override_timestamp(overrideTimestamp);
            }

            if (auto maxClipTimestamp = chunkView->GetMaxClipTimestamp()) {
                viewDescriptor->set_max_clip_timestamp(maxClipTimestamp);
            }
        } else {
            chunk = chunkOrView->AsChunk();
        }

        ToProto(descriptor->mutable_chunk_meta(), chunk->ChunkMeta());
        descriptor->set_starting_row_index(*startingRowIndex);
        *startingRowIndex += chunk->GetRowCount();
    }

    void FillHunkChunkDescriptor(
        const TChunk* chunk,
        NTabletNode::NProto::TAddHunkChunkDescriptor* descriptor)
    {
        ToProto(descriptor->mutable_chunk_id(), chunk->GetId());
        ToProto(descriptor->mutable_chunk_meta(), chunk->ChunkMeta());
    }

    void SetTabletEdenStoreIds(TTablet* tablet, std::vector<TStoreId> edenStoreIds)
    {
        i64 masterMemoryUsageDelta = -static_cast<i64>(tablet->EdenStoreIds().size() * sizeof(TStoreId));
        if (edenStoreIds.size() <= EdenStoreIdsSizeLimit) {
            tablet->EdenStoreIds() = std::move(edenStoreIds);
            tablet->EdenStoreIds().shrink_to_fit();
        } else {
            tablet->EdenStoreIds() = {};
        }
        masterMemoryUsageDelta += tablet->EdenStoreIds().size() * sizeof(TStoreId);

        auto* table = tablet->GetTable();
        table->SetTabletMasterMemoryUsage(
            table->GetTabletMasterMemoryUsage() + masterMemoryUsageDelta);

        const auto& securityManager = Bootstrap_->GetSecurityManager();
        securityManager->UpdateMasterMemoryUsage(table);
    }

    void ValidateNodeCloneMode(TTabletOwnerBase* trunkNode, ENodeCloneMode mode)
    {
        if (IsTableType(trunkNode->GetType())) {
            DoValidateNodeCloneMode(trunkNode->As<TTableNode>(), mode);
        } else if (trunkNode->GetType() == EObjectType::HunkStorage) {
            DoValidateNodeCloneMode(trunkNode->As<THunkStorageNode>(), mode);
        } else {
            YT_ABORT();
        }
    }

    void DoValidateNodeCloneMode(TTableNode* trunkNode, ENodeCloneMode mode)
    {
        try {
            switch (mode) {
                case ENodeCloneMode::Copy:
                    trunkNode->ValidateAllTabletsFrozenOrUnmounted("Cannot copy dynamic table");
                    break;

                case ENodeCloneMode::Move:
                    if (trunkNode->IsPhysicallyLog()) {
                        THROW_ERROR_EXCEPTION("Cannot move a table of type %Qlv",
                            trunkNode->GetType());
                    }
                    trunkNode->ValidateAllTabletsUnmounted("Cannot move dynamic table");
                    break;

                case ENodeCloneMode::Backup:
                    trunkNode->ValidateNotBackup("Cannot backup a backup table");
                    if (trunkNode->IsPhysicallyLog() && !trunkNode->IsReplicated()) {
                        THROW_ERROR_EXCEPTION("Cannot backup a table of type %Qlv",
                            trunkNode->GetType());
                    }
                    break;

                case ENodeCloneMode::Restore:
                    if (trunkNode->GetBackupState() != ETableBackupState::BackupCompleted) {
                        THROW_ERROR_EXCEPTION("Cannot restore table since it is not a backup table");
                    }
                    if (trunkNode->IsPhysicallyLog() && !trunkNode->IsReplicated()) {
                        THROW_ERROR_EXCEPTION("Cannot restore a table of type %Qlv",
                            trunkNode->GetType());
                    }
                    break;

                default:
                    YT_ABORT();
            }
        } catch (const std::exception& ex) {
            const auto& cypressManager = Bootstrap_->GetCypressManager();
            THROW_ERROR_EXCEPTION("Error cloning table %v",
                cypressManager->GetNodePath(trunkNode->GetTrunkNode(), trunkNode->GetTransaction()))
                << ex;
        }
    }

    void DoValidateNodeCloneMode(THunkStorageNode* trunkNode, ENodeCloneMode mode)
    {
        try {
            switch (mode) {
                case ENodeCloneMode::Move:
                case ENodeCloneMode::Copy:
                case ENodeCloneMode::Backup:
                case ENodeCloneMode::Restore:
                    THROW_ERROR_EXCEPTION("Hunk storage does not support clone mode %Qlv", mode);

                default:
                    YT_ABORT();
            }
        } catch (const std::exception& ex) {
            const auto& cypressManager = Bootstrap_->GetCypressManager();
            THROW_ERROR_EXCEPTION("Error cloning hunk storage %v",
                cypressManager->GetNodePath(trunkNode->GetTrunkNode(), trunkNode->GetTransaction()))
                << ex;
        }
    }

    void AttachToTabletChunkLists(
        TTabletBase* tablet,
        const std::vector<TChunkTree*>& children)
    {
        std::vector<TChunkTree*> storeChildren;
        std::vector<TChunkTree*> hunkChildren;
        storeChildren.reserve(children.size());
        hunkChildren.reserve(children.size());
        for (auto* child : children) {
            if (IsHunkChunk(child)) {
                hunkChildren.push_back(child);
            } else {
                storeChildren.push_back(child);
            }
        }

        const auto& chunkManager = Bootstrap_->GetChunkManager();
        chunkManager->AttachToChunkList(tablet->GetChunkList(), storeChildren);
        chunkManager->AttachToChunkList(tablet->GetHunkChunkList(), hunkChildren);
    }


    static void PopulateTableReplicaDescriptor(TTableReplicaDescriptor* descriptor, const TTableReplica* replica, const TTableReplicaInfo& info)
    {
        ToProto(descriptor->mutable_replica_id(), replica->GetId());
        descriptor->set_cluster_name(replica->GetClusterName());
        descriptor->set_replica_path(replica->GetReplicaPath());
        descriptor->set_start_replication_timestamp(replica->GetStartReplicationTimestamp());
        descriptor->set_mode(ToProto<int>(replica->GetMode()));
        descriptor->set_preserve_timestamps(replica->GetPreserveTimestamps());
        descriptor->set_atomicity(ToProto<int>(replica->GetAtomicity()));
        info.Populate(descriptor->mutable_statistics());
    }

    static void PopulateTableReplicaInfoFromStatistics(
        TTableReplicaInfo* info,
        const NTabletClient::NProto::TTableReplicaStatistics& statistics)
    {
        // Updates may be reordered but we can rely on monotonicity here.
        info->SetCommittedReplicationRowIndex(std::max(
            info->GetCommittedReplicationRowIndex(),
            statistics.committed_replication_row_index()));
        info->SetCurrentReplicationTimestamp(std::max(
            info->GetCurrentReplicationTimestamp(),
            statistics.current_replication_timestamp()));
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, Tablet, TTabletBase, TabletMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, TableReplica, TTableReplica, TableReplicaMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, TabletAction, TTabletAction, TabletActionMap_)

////////////////////////////////////////////////////////////////////////////////

TTabletManager::TTabletManager(NCellMaster::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(bootstrap))
{ }

TTabletManager::~TTabletManager() = default;

void TTabletManager::Initialize()
{
    return Impl_->Initialize();
}

IYPathServicePtr TTabletManager::GetOrchidService()
{
    return Impl_->GetOrchidService();
}

void TTabletManager::PrepareMount(
    TTabletOwnerBase* table,
    int firstTabletIndex,
    int lastTabletIndex,
    TTabletCellId hintCellId,
    const std::vector<TTabletCellId>& targetCellIds,
    bool freeze)
{
    Impl_->PrepareMount(
        table,
        firstTabletIndex,
        lastTabletIndex,
        hintCellId,
        targetCellIds,
        freeze);
}

void TTabletManager::PrepareUnmount(
    TTabletOwnerBase* table,
    bool force,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->PrepareUnmount(
        table,
        force,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::PrepareRemount(
    TTabletOwnerBase* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->PrepareRemount(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::PrepareFreeze(
    TTabletOwnerBase* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->PrepareFreeze(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::PrepareUnfreeze(
    TTabletOwnerBase* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->PrepareUnfreeze(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::PrepareReshard(
    TTabletOwnerBase* table,
    int firstTabletIndex,
    int lastTabletIndex,
    int newTabletCount,
    const std::vector<TLegacyOwningKey>& pivotKeys,
    bool create)
{
    Impl_->PrepareReshard(
        table,
        firstTabletIndex,
        lastTabletIndex,
        newTabletCount,
        pivotKeys,
        create);
}

void TTabletManager::ValidateMakeTableDynamic(TTableNode* table)
{
    Impl_->ValidateMakeTableDynamic(table);
}

void TTabletManager::ValidateMakeTableStatic(TTableNode* table)
{
    Impl_->ValidateMakeTableStatic(table);
}

void TTabletManager::Mount(
    TTabletOwnerBase* table,
    const TString& path,
    int firstTabletIndex,
    int lastTabletIndex,
    TTabletCellId hintCellId,
    const std::vector<TTabletCellId>& targetCellIds,
    bool freeze,
    TTimestamp mountTimestamp)
{
    Impl_->Mount(
        table,
        path,
        firstTabletIndex,
        lastTabletIndex,
        hintCellId,
        targetCellIds,
        freeze,
        mountTimestamp);
}

void TTabletManager::Unmount(
    TTabletOwnerBase* table,
    bool force,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->Unmount(
        table,
        force,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::Remount(
    TTabletOwnerBase* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->Remount(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::Freeze(
    TTabletOwnerBase* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->Freeze(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::Unfreeze(
    TTabletOwnerBase* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->Unfreeze(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::Reshard(
    TTabletOwnerBase* table,
    int firstTabletIndex,
    int lastTabletIndex,
    int newTabletCount,
    const std::vector<TLegacyOwningKey>& pivotKeys)
{
    Impl_->Reshard(
        table,
        firstTabletIndex,
        lastTabletIndex,
        newTabletCount,
        pivotKeys);
}

void TTabletManager::ValidateCloneTabletOwner(
    TTabletOwnerBase* sourceNode,
    ENodeCloneMode mode,
    TAccount* account)
{
    return Impl_->ValidateCloneTabletOwner(
        sourceNode,
        mode,
        account);
}

void TTabletManager::ValidateBeginCopyTabletOwner(
    TTabletOwnerBase* sourceNode,
    ENodeCloneMode mode)
{
    return Impl_->ValidateBeginCopyTabletOwner(
        sourceNode,
        mode);
}

void TTabletManager::CloneTabletOwner(
    TTabletOwnerBase* sourceNode,
    TTabletOwnerBase* clonedNode,
    ENodeCloneMode mode)
{
    return Impl_->CloneTabletOwner(
        sourceNode,
        clonedNode,
        mode);
}

void TTabletManager::MakeTableDynamic(TTableNode* table)
{
    Impl_->MakeTableDynamic(table);
}

void TTabletManager::MakeTableStatic(TTableNode* table)
{
    Impl_->MakeTableStatic(table);
}

void TTabletManager::LockDynamicTable(
    TTableNode* table,
    TTransaction* transaction,
    TTimestamp timestamp)
{
    Impl_->LockDynamicTable(table, transaction, timestamp);
}

void TTabletManager::CheckDynamicTableLock(
    TTableNode* table,
    TTransaction* transaction,
    NTableClient::NProto::TRspCheckDynamicTableLock* response)
{
    Impl_->CheckDynamicTableLock(table, transaction, response);
}

TTabletBase* TTabletManager::GetTabletOrThrow(TTabletId id)
{
    return Impl_->GetTabletOrThrow(id);
}

TTabletCell* TTabletManager::GetTabletCellOrThrow(TTabletCellId id)
{
    return Impl_->GetTabletCellOrThrow(id);
}

TTabletCellBundle* TTabletManager::GetTabletCellBundleOrThrow(TTabletCellBundleId id)
{
    return Impl_->GetTabletCellBundleOrThrow(id);
}

TTabletCellBundle* TTabletManager::FindTabletCellBundle(TTabletCellBundleId id)
{
    return Impl_->FindTabletCellBundle(id);
}

TTabletCellBundle* TTabletManager::GetTabletCellBundleByNameOrThrow(const TString& name, bool activeLifeStageOnly)
{
    return Impl_->GetTabletCellBundleByNameOrThrow(name, activeLifeStageOnly);
}

TTabletCellBundle* TTabletManager::GetDefaultTabletCellBundle()
{
    return Impl_->GetDefaultTabletCellBundle();
}

void TTabletManager::SetTabletCellBundle(TTabletOwnerBase* owner, TTabletCellBundle* cellBundle)
{
    Impl_->SetTabletCellBundle(owner, cellBundle);
}

void TTabletManager::ZombifyTabletCell(TTabletCell* cell)
{
    Impl_->ZombifyTabletCell(cell);
}

void TTabletManager::DestroyTablet(TTabletBase* tablet)
{
    Impl_->DestroyTablet(tablet);
}

void TTabletManager::DestroyTabletOwner(TTabletOwnerBase* table)
{
    Impl_->DestroyTabletOwner(table);
}

TNode* TTabletManager::FindTabletLeaderNode(const TTabletBase* tablet) const
{
    return Impl_->FindTabletLeaderNode(tablet);
}

void TTabletManager::UpdateExtraMountConfigKeys(std::vector<TString> keys)
{
    Impl_->UpdateExtraMountConfigKeys(std::move(keys));
}

TTableReplica* TTabletManager::CreateTableReplica(
    TReplicatedTableNode* table,
    const TString& clusterName,
    const TYPath& replicaPath,
    ETableReplicaMode mode,
    bool preserveTimestamps,
    EAtomicity atomicity,
    bool enabled,
    TTimestamp startReplicationTimestamp,
    const  std::optional<std::vector<i64>>& startReplicationRowIndexes)
{
    return Impl_->CreateTableReplica(
        table,
        clusterName,
        replicaPath,
        mode,
        preserveTimestamps,
        atomicity,
        enabled,
        startReplicationTimestamp,
        startReplicationRowIndexes);
}

void TTabletManager::DestroyTableReplica(TTableReplica* replica)
{
    Impl_->DestroyTableReplica(replica);
}

void TTabletManager::AlterTableReplica(
    TTableReplica* replica,
    std::optional<bool> enabled,
    std::optional<ETableReplicaMode> mode,
    std::optional<EAtomicity> atomicity,
    std::optional<bool> preserveTimestamps,
    std::optional<bool> enableReplicatedTableTracker)
{
    Impl_->AlterTableReplica(
        replica,
        std::move(enabled),
        std::move(mode),
        std::move(atomicity),
        std::move(preserveTimestamps),
        std::move(enableReplicatedTableTracker));
}

std::vector<TTabletActionId> TTabletManager::SyncBalanceCells(
    TTabletCellBundle* bundle,
    const std::optional<std::vector<TTableNode*>>& tables,
    bool keepActions)
{
    return Impl_->SyncBalanceCells(bundle, tables, keepActions);
}

std::vector<TTabletActionId> TTabletManager::SyncBalanceTablets(TTableNode* table, bool keepActions)
{
    return Impl_->SyncBalanceTablets(table, keepActions);
}

TTabletAction* TTabletManager::CreateTabletAction(
    NObjectClient::TObjectId hintId,
    ETabletActionKind kind,
    const std::vector<TTabletBase*>& tablets,
    const std::vector<TTabletCell*>& cells,
    const std::vector<NTableClient::TLegacyOwningKey>& pivotKeys,
    const std::optional<int>& tabletCount,
    bool skipFreezing,
    TGuid correlationId,
    TInstant expirationTime,
    std::optional<TDuration> expirationTimeout)
{
    return Impl_->CreateTabletAction(
        hintId,
        kind,
        tablets,
        cells,
        pivotKeys,
        tabletCount,
        skipFreezing,
        correlationId,
        expirationTime,
        expirationTimeout);
}

void TTabletManager::DestroyTabletAction(TTabletAction* action)
{
    Impl_->DestroyTabletAction(action);
}

void TTabletManager::MergeTable(TTableNode* originatingNode, NTableServer::TTableNode* branchedNode)
{
    Impl_->MergeTable(originatingNode, branchedNode);
}

TReplicationProgress TTabletManager::GatherReplicationProgress(const TTableNode* table)
{
    return Impl_->GatherReplicationProgress(table);
}

void TTabletManager::ScatterReplicationProgress(TTableNode* table, TReplicationProgress progress)
{
    Impl_->ScatterReplicationProgress(table, std::move(progress));
}

void TTabletManager::OnNodeStorageParametersUpdated(TChunkOwnerBase* node)
{
    Impl_->OnNodeStorageParametersUpdated(node);
}

void TTabletManager::RecomputeTabletCellStatistics(TCellBase* cellBase)
{
    return Impl_->RecomputeTabletCellStatistics(cellBase);
}

void TTabletManager::ParseTabletRangeOrThrow(
    const TTabletOwnerBase* owner,
    int* first,
    int* last) const
{
    Impl_->ParseTabletRangeOrThrow(owner, first, last);
}

void TTabletManager::OnHunkJournalChunkSealed(TChunk* chunk)
{
    Impl_->OnHunkJournalChunkSealed(chunk);
}

void TTabletManager::WrapWithBackupChunkViews(TTablet* tablet, TTimestamp timestamp)
{
    Impl_->WrapWithBackupChunkViews(tablet, timestamp);
}

TError TTabletManager::PromoteFlushedDynamicStores(TTablet* tablet)
{
    return Impl_->PromoteFlushedDynamicStores(tablet);
}

TError TTabletManager::ApplyBackupCutoff(TTablet* tablet)
{
    return Impl_->ApplyBackupCutoff(tablet);
}

void TTabletManager::CopyChunkListIfShared(
    TTableNode* table,
    EChunkListContentType contentType,
    int tabletIndex)
{
    return Impl_->CopyChunkListIfShared(
        table,
        contentType,
        /*firstTabletIndex*/ tabletIndex,
        /*lastTabletIndex*/ tabletIndex,
        /*force*/ false);
}

DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, Tablet, TTabletBase, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, TableReplica, TTableReplica, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, TabletAction, TTabletAction, *Impl_)

DELEGATE_SIGNAL_WITH_ACCESSOR(TTabletManager, void(TReplicatedTableData), ReplicatedTableCreated, *Impl_);
DELEGATE_SIGNAL_WITH_ACCESSOR(TTabletManager, void(TTableId), ReplicatedTableDestroyed, *Impl_);
DELEGATE_SIGNAL_WITH_ACCESSOR(TTabletManager, void(TTableId, TReplicatedTableOptionsPtr), ReplicatedTableOptionsUpdated, *Impl_);
DELEGATE_SIGNAL(TTabletManager, void(TReplicaData), ReplicaCreated, *Impl_);
DELEGATE_SIGNAL(TTabletManager, void(TTableReplicaId), ReplicaDestroyed, *Impl_);
DELEGATE_SIGNAL(TTabletManager, void(TTableReplicaId, ETableReplicaMode), ReplicaModeUpdated, *Impl_);
DELEGATE_SIGNAL(TTabletManager, void(TTableReplicaId, bool), ReplicaEnablementUpdated, *Impl_);
DELEGATE_SIGNAL_WITH_ACCESSOR(TTabletManager, void(TTableReplicaId, bool), ReplicaTrackingPolicyUpdated, *Impl_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
