#include "tablet_manager.h"
#include "private.h"
#include "config.h"
#include "cypress_integration.h"
#include "tablet.h"
#include "tablet_cell.h"
#include "tablet_cell_proxy.h"
#include "tablet_cell_bundle_proxy.h"
#include "tablet_proxy.h"
#include "tablet_tracker.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>
#include <yt/server/cell_master/serialize.h>

#include <yt/server/chunk_server/chunk_list.h>
#include <yt/server/chunk_server/chunk_manager.h>
#include <yt/server/chunk_server/chunk_tree_traversing.h>

#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/hive/hive_manager.h>

#include <yt/server/node_tracker_server/node.h>
#include <yt/server/node_tracker_server/node_tracker.h>

#include <yt/server/object_server/object_manager.h>
#include <yt/server/object_server/type_handler_detail.h>

#include <yt/server/security_server/security_manager.h>

#include <yt/server/table_server/table_node.h>

#include <yt/server/tablet_node/config.h>
#include <yt/server/tablet_node/tablet_manager.pb.h>

#include <yt/server/tablet_server/tablet_manager.pb.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/config.h>

#include <yt/ytlib/election/config.h>

#include <yt/ytlib/hive/cell_directory.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/ytlib/tablet_client/config.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/misc/address.h>
#include <yt/core/misc/collection_helpers.h>
#include <yt/core/misc/string.h>

#include <util/random/random.h>

#include <algorithm>

namespace NYT {
namespace NTabletServer {

using namespace NConcurrency;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NObjectClient;
using namespace NObjectClient::NProto;
using namespace NObjectServer;
using namespace NYTree;
using namespace NSecurityServer;
using namespace NTableServer;
using namespace NTabletClient;
using namespace NHydra;
using namespace NHive;
using namespace NTransactionServer;
using namespace NTabletServer::NProto;
using namespace NNodeTrackerServer;
using namespace NNodeTrackerServer::NProto;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NTabletNode::NProto;
using namespace NChunkServer;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NCypressServer;
using namespace NCypressClient;
using namespace NCellMaster;

using NTabletNode::TTableMountConfigPtr;
using NTabletNode::EInMemoryMode;
using NNodeTrackerServer::NProto::TReqIncrementalHeartbeat;
using NNodeTrackerClient::TNodeDescriptor;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletServerLogger;
static const auto CleanupPeriod = TDuration::Seconds(10);

////////////////////////////////////////////////////////////////////////////////

class TTabletManager::TTabletCellBundleTypeHandler
    : public TObjectTypeHandlerWithMapBase<TTabletCellBundle>
{
public:
    explicit TTabletCellBundleTypeHandler(TImpl* owner);

    virtual EObjectType GetType() const override
    {
        return EObjectType::TabletCellBundle;
    }

    virtual TNullable<TTypeCreationOptions> GetCreationOptions() const override
    {
        return TTypeCreationOptions(
            EObjectTransactionMode::Forbidden,
            EObjectAccountMode::Forbidden);
    }

    virtual TObjectBase* CreateObject(
        const TObjectId& hintId,
        TTransaction* transaction,
        TAccount* account,
        IAttributeDictionary* attributes,
        const TObjectCreationExtensions& /*extensions*/) override;

private:
    TImpl* const Owner_;

    virtual Stroka DoGetName(const TTabletCellBundle* bundle) override
    {
        return Format("tablet cell bundle %Qv", bundle->GetName());
    }

    virtual IObjectProxyPtr DoGetProxy(TTabletCellBundle* bundle, TTransaction* /*transaction*/) override
    {
        return CreateTabletCellBundleProxy(Bootstrap_, bundle);
    }

    virtual void DoDestroyObject(TTabletCellBundle* bundle) override;

};

////////////////////////////////////////////////////////////////////////////////

class TTabletManager::TTabletCellTypeHandler
    : public TObjectTypeHandlerWithMapBase<TTabletCell>
{
public:
    explicit TTabletCellTypeHandler(TImpl* owner);

    virtual EObjectReplicationFlags GetReplicationFlags() const override
    {
        return
            EObjectReplicationFlags::ReplicateCreate |
            EObjectReplicationFlags::ReplicateDestroy |
            EObjectReplicationFlags::ReplicateAttributes;
    }

    virtual EObjectType GetType() const override
    {
        return EObjectType::TabletCell;
    }

    virtual TNullable<TTypeCreationOptions> GetCreationOptions() const override
    {
        return TTypeCreationOptions(
            EObjectTransactionMode::Forbidden,
            EObjectAccountMode::Forbidden);
    }

    virtual TObjectBase* CreateObject(
        const TObjectId& hintId,
        TTransaction* transaction,
        TAccount* account,
        IAttributeDictionary* attributes,
        const TObjectCreationExtensions& extensions) override;

private:
    TImpl* const Owner_;

    virtual TCellTagList DoGetReplicationCellTags(const TTabletCell* /*cell*/) override
    {
        return AllSecondaryCellTags();
    }

    virtual Stroka DoGetName(const TTabletCell* cell) override
    {
        return Format("tablet cell %v", cell->GetId());
    }

    virtual IObjectProxyPtr DoGetProxy(TTabletCell* cell, TTransaction* /*transaction*/) override
    {
        return CreateTabletCellProxy(Bootstrap_, cell);
    }

    virtual void DoZombifyObject(TTabletCell* cell) override;

};

////////////////////////////////////////////////////////////////////////////////

class TTabletManager::TTabletTypeHandler
    : public TObjectTypeHandlerWithMapBase<TTablet>
{
public:
    explicit TTabletTypeHandler(TImpl* owner);

    virtual EObjectType GetType() const override
    {
        return EObjectType::Tablet;
    }

private:
    TImpl* const Owner_;

    virtual Stroka DoGetName(const TTablet* object) override
    {
        return Format("tablet %v", object->GetId());
    }

    virtual IObjectProxyPtr DoGetProxy(TTablet* tablet, TTransaction* /*transaction*/) override
    {
        return CreateTabletProxy(Bootstrap_, tablet);
    }

    virtual void DoDestroyObject(TTablet* tablet) override;

};

////////////////////////////////////////////////////////////////////////////////

class TTabletManager::TImpl
    : public TMasterAutomatonPart
{
public:
    explicit TImpl(
        TTabletManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap)
        , Config_(config)
        , TabletTracker_(New<TTabletTracker>(Config_, Bootstrap_))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(), AutomatonThread);

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

        RegisterMethod(BIND(&TImpl::HydraAssignPeers, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRevokePeers, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraSetLeadingPeer, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletMounted, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraOnTabletUnmounted, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraUpdateTabletStores, Unretained(this)));

        if (Bootstrap_->IsPrimaryMaster()) {
            auto nodeTracker = Bootstrap_->GetNodeTracker();
            nodeTracker->SubscribeNodeRegistered(BIND(&TImpl::OnNodeRegistered, MakeWeak(this)));
            nodeTracker->SubscribeNodeUnregistered(BIND(&TImpl::OnNodeUnregistered, MakeWeak(this)));
            nodeTracker->SubscribeIncrementalHeartbeat(BIND(&TImpl::OnIncrementalHeartbeat, MakeWeak(this)));
        }
    }

    void Initialize()
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        objectManager->RegisterHandler(New<TTabletCellBundleTypeHandler>(this));
        objectManager->RegisterHandler(New<TTabletCellTypeHandler>(this));
        objectManager->RegisterHandler(New<TTabletTypeHandler>(this));

        auto transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->SubscribeTransactionCommitted(BIND(&TImpl::OnTransactionFinished, MakeWeak(this)));
        transactionManager->SubscribeTransactionAborted(BIND(&TImpl::OnTransactionFinished, MakeWeak(this)));
    }

    TTabletCellBundle* CreateCellBundle(const Stroka& name, IAttributeDictionary* attributes, const TObjectId& hintId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (name.empty()) {
            THROW_ERROR_EXCEPTION("Tablet cell bundle name cannot be empty");
        }

        if (FindTabletCellBundleByName(name)) {
            THROW_ERROR_EXCEPTION(
                NYTree::EErrorCode::AlreadyExists,
                "Tablet cell bundle %Qv already exists",
                name);
        }

        auto objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::TabletCellBundle, hintId);
        auto bundleHolder = std::make_unique<TTabletCellBundle>(id);

        bundleHolder->SetName(name);
        bundleHolder->SetOptions(ConvertTo<TTabletCellOptionsPtr>(attributes)); // may throw

        auto* bundle = TabletCellBundleMap_.Insert(id, std::move(bundleHolder));
        YCHECK(NameToTabletCellBundleMap_.insert(std::make_pair(bundle->GetName(), bundle)).second);

        objectManager->RefObject(bundle);

        return bundle;
    }

    void DestroyCellBundle(TTabletCellBundle* bundle)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // Remove tablet cell bundle from maps.
        YCHECK(NameToTabletCellBundleMap_.erase(bundle->GetName()) == 1);
    }

    TTabletCell* CreateCell(int peerCount, IAttributeDictionary* attributes, const TObjectId& hintId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (peerCount < 1 || peerCount > MaxPeerCount) {
            THROW_ERROR_EXCEPTION("Peer count must be in range [%v, %v]",
                1,
                MaxPeerCount);
        }

        auto objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::TabletCell, hintId);
        auto cellHolder = std::make_unique<TTabletCell>(id);

        cellHolder->SetPeerCount(peerCount);
        cellHolder->SetOptions(ConvertTo<TTabletCellOptionsPtr>(attributes)); // may throw
        cellHolder->Peers().resize(peerCount);

        ReconfigureCell(cellHolder.get());

        auto* cell = TabletCellMap_.Insert(id, std::move(cellHolder));

        // Make the fake reference.
        YCHECK(cell->RefObject() == 1);

        auto hiveManager = Bootstrap_->GetHiveManager();
        hiveManager->CreateMailbox(id);

        auto cellMapNodeProxy = GetCellMapNode();
        auto cellNodePath = "/" + ToString(id);

        try {
            // NB: Users typically are not allowed to create these types.
            auto securityManager = Bootstrap_->GetSecurityManager();
            auto* rootUser = securityManager->GetRootUser();
            TAuthenticatedUserGuard userGuard(securityManager, rootUser);
            
            // Create Cypress node.
            {
                auto req = TCypressYPathProxy::Create(cellNodePath);
                req->set_type(static_cast<int>(EObjectType::TabletCellNode));

                auto attributes = CreateEphemeralAttributes();
                attributes->Set("opaque", true);
                ToProto(req->mutable_node_attributes(), *attributes);

                SyncExecuteVerb(cellMapNodeProxy, req);
            }

            // Create "snapshots" child.
            {
                auto req = TCypressYPathProxy::Create(cellNodePath + "/snapshots");
                req->set_type(static_cast<int>(EObjectType::MapNode));

                SyncExecuteVerb(cellMapNodeProxy, req);
            }

            // Create "changelogs" child.
            {
                auto req = TCypressYPathProxy::Create(cellNodePath + "/changelogs");
                req->set_type(static_cast<int>(EObjectType::MapNode));

                SyncExecuteVerb(cellMapNodeProxy, req);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR_UNLESS(IsRecovery(), ex, "Error registering tablet cell in Cypress");
        }

        return cell;
    }

    void DestroyCell(TTabletCell* cell)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto hiveManager = Bootstrap_->GetHiveManager();
        hiveManager->RemoveMailbox(cell->GetId());

        for (const auto& peer : cell->Peers()) {
            if (peer.Node) {
                peer.Node->DetachTabletCell(cell);
            }
            if (!peer.Descriptor.IsNull()) {
                RemoveFromAddressToCellMap(peer.Descriptor, cell);
            }
        }

        AbortPrerequisiteTransaction(cell);

        auto cellMapNodeProxy = GetCellMapNode();
        auto cellNodeProxy = cellMapNodeProxy->FindChild(ToString(cell->GetId()));
        if (cellNodeProxy) {
            auto cypressManager = Bootstrap_->GetCypressManager();
            cypressManager->AbortSubtreeTransactions(cellNodeProxy);
            cellMapNodeProxy->RemoveChild(cellNodeProxy);
        }
    }


    TTablet* CreateTablet(TTableNode* table)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto objectManager = Bootstrap_->GetObjectManager();
        auto id = objectManager->GenerateId(EObjectType::Tablet, NullObjectId);
        auto tabletHolder = std::make_unique<TTablet>(id);
        tabletHolder->SetTable(table);

        auto* tablet = TabletMap_.Insert(id, std::move(tabletHolder));
        objectManager->RefObject(tablet);

        LOG_INFO_UNLESS(IsRecovery(), "Tablet created (TableId: %v, TabletId: %v)",
            table->GetId(),
            tablet->GetId());

        return tablet;
    }

    void DestroyTablet(TTablet* tablet)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        YCHECK(!tablet->GetCell());
    }


    int GetAssignedTabletCellCount(const Stroka& address) const
    {
        auto range = AddressToCell_.equal_range(address);
        return std::distance(range.first, range.second);
    }

    TTableSchema GetTableSchema(TTableNode* table)
    {
        return table->TableSchema();
    }

    TTabletStatistics GetTabletStatistics(const TTablet* tablet)
    {
        const auto* table = tablet->GetTable();
        const auto* rootChunkList = table->GetChunkList();
        const auto* tabletChunkList = rootChunkList->Children()[tablet->GetIndex()]->AsChunkList();
        const auto& treeStatistics = tabletChunkList->Statistics();
        const auto& nodeStatistics = tablet->NodeStatistics();

        TTabletStatistics tabletStatistics;
        tabletStatistics.PartitionCount = nodeStatistics.partition_count();
        tabletStatistics.StoreCount = nodeStatistics.store_count();
        tabletStatistics.PreloadPendingStoreCount = nodeStatistics.preload_pending_store_count();
        tabletStatistics.PreloadCompletedStoreCount = nodeStatistics.preload_completed_store_count();
        tabletStatistics.PreloadFailedStoreCount = nodeStatistics.preload_failed_store_count();
        tabletStatistics.UnmergedRowCount = treeStatistics.RowCount;
        tabletStatistics.UncompressedDataSize = treeStatistics.UncompressedDataSize;
        tabletStatistics.CompressedDataSize = treeStatistics.CompressedDataSize;
        switch (tablet->GetInMemoryMode()) {
            case EInMemoryMode::Compressed:
                tabletStatistics.MemorySize = tabletStatistics.CompressedDataSize;
                break;
            case EInMemoryMode::Uncompressed:
                tabletStatistics.MemorySize = tabletStatistics.UncompressedDataSize;
                break;
            case EInMemoryMode::None:
                tabletStatistics.MemorySize = 0;
                break;
            default:
                YUNREACHABLE();
        }
        tabletStatistics.DiskSpace =
            treeStatistics.RegularDiskSpace * table->GetReplicationFactor() +
            treeStatistics.ErasureDiskSpace;
        tabletStatistics.ChunkCount = treeStatistics.ChunkCount;
        return tabletStatistics;
    }


    void MountTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        const TTabletCellId& cellId)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot mount a static table");
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw
        auto schema = GetTableSchema(table); // may throw

        TTabletCell* hintedCell;
        if (!cellId) {
            ValidateHasHealthyCells(); // may throw
            hintedCell = nullptr;
        } else {
            hintedCell = GetTabletCellOrThrow(cellId); // may throw
        }

        auto objectManager = Bootstrap_->GetObjectManager();
        auto chunkManager = Bootstrap_->GetChunkManager();

        const auto& allTablets = table->Tablets();

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            const auto* tablet = allTablets[index];
            if (tablet->GetState() == ETabletState::Unmounting) {
                THROW_ERROR_EXCEPTION("Tablet %v is in %Qlv state",
                    tablet->GetId(),
                    tablet->GetState());
            }
        }

        TTableMountConfigPtr mountConfig;
        NTabletNode::TTabletWriterOptionsPtr writerOptions;
        GetTableSettings(table, &mountConfig, &writerOptions);

        auto serializedMountConfig = ConvertToYsonString(mountConfig);
        auto serializedWriterOptions = ConvertToYsonString(writerOptions);

        std::vector<TTablet*> tabletsToMount;
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = allTablets[index];
            if (!tablet->GetCell()) {
                tabletsToMount.push_back(tablet);
            }
        }

        const auto& chunkLists = table->GetChunkList()->Children();
        YCHECK(allTablets.size() == chunkLists.size());

        auto assignment = ComputeTabletAssignment(
            table,
            mountConfig,
            hintedCell,
            std::move(tabletsToMount));

        for (const auto& pair : assignment) {
            auto* tablet = pair.first;
            int tabletIndex = tablet->GetIndex();
            auto pivotKey = tablet->GetPivotKey();
            auto nextPivotKey = tablet->GetIndex() + 1 == allTablets.size()
                ? MaxKey()
                : allTablets[tabletIndex + 1]->GetPivotKey();

            auto* cell = pair.second;
            tablet->SetCell(cell);
            YCHECK(cell->Tablets().insert(tablet).second);
            objectManager->RefObject(cell);

            YCHECK(tablet->GetState() == ETabletState::Unmounted);
            tablet->SetState(ETabletState::Mounting);
            tablet->SetInMemoryMode(mountConfig->InMemoryMode);

            const auto* context = GetCurrentMutationContext();
            tablet->SetMountRevision(context->GetVersion().ToRevision());

            TReqMountTablet req;
            ToProto(req.mutable_tablet_id(), tablet->GetId());
            req.set_mount_revision(tablet->GetMountRevision());
            ToProto(req.mutable_table_id(), table->GetId());
            ToProto(req.mutable_schema(), schema);
            ToProto(req.mutable_key_columns()->mutable_names(), table->TableSchema().GetKeyColumns()); // max42: What do we do here?
            ToProto(req.mutable_pivot_key(), pivotKey);
            ToProto(req.mutable_next_pivot_key(), nextPivotKey);
            req.set_mount_config(serializedMountConfig.Data());
            req.set_writer_options(serializedWriterOptions.Data());
            req.set_atomicity(static_cast<int>(table->GetAtomicity()));

            auto* chunkList = chunkLists[tabletIndex]->AsChunkList();
            auto chunks = EnumerateChunksInChunkTree(chunkList);
            for (const auto* chunk : chunks) {
                auto* descriptor = req.add_stores();
                // XXX(babenko): generalize
                descriptor->set_store_type(static_cast<int>(NTabletNode::EStoreType::SortedChunk));
                ToProto(descriptor->mutable_store_id(), chunk->GetId());
                descriptor->mutable_chunk_meta()->CopyFrom(chunk->ChunkMeta());
            }

            auto hiveManager = Bootstrap_->GetHiveManager();
            auto* mailbox = hiveManager->GetMailbox(cell->GetId());
            hiveManager->PostMessage(mailbox, req);

            LOG_INFO_UNLESS(IsRecovery(), "Mounting tablet (TableId: %v, TabletId: %v, CellId: %v, ChunkCount: %v, "
                "Atomicity: %v)",
                table->GetId(),
                tablet->GetId(),
                cell->GetId(),
                chunks.size(),
                table->GetAtomicity());
        }
    }

    void UnmountTable(
        TTableNode* table,
        bool force,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot unmount a static table");
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw

        if (!force) {
            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* tablet = table->Tablets()[index];
                if (tablet->GetState() == ETabletState::Mounting) {
                    THROW_ERROR_EXCEPTION("Tablet %v is in %Qlv state",
                        tablet->GetId(),
                        tablet->GetState());
                }
            }
        }

        DoUnmountTable(table, force, firstTabletIndex, lastTabletIndex);
    }

    void RemountTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot remount a static table");
        }

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw

        TTableMountConfigPtr mountConfig;
        NTabletNode::TTabletWriterOptionsPtr writerOptions;
        GetTableSettings(table, &mountConfig, &writerOptions);

        auto serializedMountConfig = ConvertToYsonString(mountConfig);
        auto serializedWriterOptions = ConvertToYsonString(writerOptions);

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            auto* cell = tablet->GetCell();

            if (tablet->GetState() == ETabletState::Mounted ||
                tablet->GetState() == ETabletState::Mounting)
            {
                LOG_INFO_UNLESS(IsRecovery(), "Remounting tablet (TableId: %v, TabletId: %v, CellId: %v)",
                    table->GetId(),
                    tablet->GetId(),
                    cell->GetId());

                cell->TotalStatistics() -= GetTabletStatistics(tablet);
                tablet->SetInMemoryMode(mountConfig->InMemoryMode);
                cell->TotalStatistics() += GetTabletStatistics(tablet);

                auto hiveManager = Bootstrap_->GetHiveManager();

                {
                    TReqRemountTablet request;
                    request.set_mount_config(serializedMountConfig.Data());
                    request.set_writer_options(serializedWriterOptions.Data());
                    ToProto(request.mutable_tablet_id(), tablet->GetId());
                    auto* mailbox = hiveManager->GetMailbox(cell->GetId());
                    hiveManager->PostMessage(mailbox, request);
                }
            }
        }
    }

    void ClearTablets(TTableNode* table)
    {
        if (table->Tablets().empty())
            return;

        DoUnmountTable(
            table,
            true,
            0,
            static_cast<int>(table->Tablets().size()) - 1);

        auto objectManager = Bootstrap_->GetObjectManager();
        for (auto* tablet : table->Tablets()) {
            YCHECK(tablet->GetState() == ETabletState::Unmounted);
            objectManager->UnrefObject(tablet);
        }

        table->Tablets().clear();
    }

    void ReshardTable(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex,
        const std::vector<TOwningKey>& pivotKeys)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);
        YCHECK(table->IsTrunk());

        if (!table->IsDynamic()) {
            THROW_ERROR_EXCEPTION("Cannot reshard a static table");
        }

        auto objectManager = Bootstrap_->GetObjectManager();
        auto chunkManager = Bootstrap_->GetChunkManager();

        ParseTabletRange(table, &firstTabletIndex, &lastTabletIndex); // may throw

        auto& tablets = table->Tablets();
        YCHECK(tablets.size() == table->GetChunkList()->Children().size());

        int oldTabletCount = lastTabletIndex - firstTabletIndex + 1;
        int newTabletCount = static_cast<int>(pivotKeys.size());

        if (tablets.size() - oldTabletCount + newTabletCount > MaxTabletCount) {
            THROW_ERROR_EXCEPTION("Tablet count cannot exceed the limit of %v",
                MaxTabletCount);
        }

        if (!pivotKeys.empty()) {
            if (firstTabletIndex > lastTabletIndex) {
                if (pivotKeys[0] != EmptyKey()) {
                    THROW_ERROR_EXCEPTION("First pivot key must be empty");
                }
            } else {
                if (pivotKeys[0] != tablets[firstTabletIndex]->GetPivotKey()) {
                    THROW_ERROR_EXCEPTION(
                        "First pivot key must match that of the first tablet "
                        "in the resharded range");
                }
            }
        }

        for (int index = 0; index < static_cast<int>(pivotKeys.size()) - 1; ++index) {
            if (pivotKeys[index] >= pivotKeys[index + 1]) {
                THROW_ERROR_EXCEPTION("Pivot keys must be strictly increasing");
            }
        }

        // Validate pivot keys against table schema.
        auto schema = GetTableSchema(table);
        int keyColumnCount = table->TableSchema().GetKeyColumns().size();
        for (const auto& pivotKey : pivotKeys) {
            ValidatePivotKey(pivotKey, schema, keyColumnCount);
        }

        if (lastTabletIndex != tablets.size() - 1) {
            if (pivotKeys.back() >= tablets[lastTabletIndex + 1]->GetPivotKey()) {
                THROW_ERROR_EXCEPTION(
                    "Last pivot key must be strictly less than that of the tablet "
                    "which follows the resharded range");
            }
        }

        // Validate that all tablets are unmounted.
        if (table->HasMountedTablets()) {
            THROW_ERROR_EXCEPTION("Cannot reshard the table since it has mounted tablets");
        }

        // Drop old tablets.
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            objectManager->UnrefObject(tablet);
        }

        // Create new tablets.
        std::vector<TTablet*> newTablets;
        for (int index = 0; index < newTabletCount; ++index) {
            auto* tablet = CreateTablet(table);
            tablet->SetPivotKey(pivotKeys[index]);
            newTablets.push_back(tablet);
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
        CopyChunkListIfShared(table, firstTabletIndex, lastTabletIndex);

        // Update chunk lists.
        auto* newRootChunkList = chunkManager->CreateChunkList();
        auto* oldRootChunkList = table->GetChunkList();
        auto& chunkLists = oldRootChunkList->Children();
        chunkManager->AttachToChunkList(
            newRootChunkList,
            chunkLists.data(),
            chunkLists.data() + firstTabletIndex);
        for (int index = 0; index < newTabletCount; ++index) {
            auto* tabletChunkList = chunkManager->CreateChunkList();
            chunkManager->AttachToChunkList(newRootChunkList, tabletChunkList);
        }
        chunkManager->AttachToChunkList(
            newRootChunkList,
            chunkLists.data() + lastTabletIndex + 1,
            chunkLists.data() + chunkLists.size());

        // Move chunks from the resharded tablets to appropriate chunk lists.
        std::vector<TChunk*> chunks;
        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            EnumerateChunksInChunkTree(chunkLists[index]->AsChunkList(), &chunks);
        }

        std::sort(chunks.begin(), chunks.end(), TObjectRefComparer::Compare);
        chunks.erase(
            std::unique(chunks.begin(), chunks.end()),
            chunks.end());

        for (auto* chunk : chunks) {
            auto boundaryKeysExt = GetProtoExtension<TBoundaryKeysExt>(chunk->ChunkMeta().extensions());
            auto minKey = WidenKey(FromProto<TOwningKey>(boundaryKeysExt.min()), keyColumnCount);
            auto maxKey = WidenKey(FromProto<TOwningKey>(boundaryKeysExt.max()), keyColumnCount);
            auto range = GetIntersectingTablets(newTablets, minKey, maxKey);
            for (auto it = range.first; it != range.second; ++it) {
                auto* tablet = *it;
                chunkManager->AttachToChunkList(
                    newRootChunkList->Children()[tablet->GetIndex()]->AsChunkList(),
                    chunk);
            }
        }

        // Replace root chunk list.
        table->SetChunkList(newRootChunkList);
        newRootChunkList->AddOwningNode(table);
        objectManager->RefObject(newRootChunkList);
        oldRootChunkList->RemoveOwningNode(table);
        objectManager->UnrefObject(oldRootChunkList);

        table->SnapshotStatistics() = table->GetChunkList()->Statistics().ToDataStatistics();
    }

    void MakeDynamic(TTableNode* table)
    {
        if (table->IsDynamic()) {
            return;
        }

        auto* rootChunkList = table->GetChunkList();
        if (!rootChunkList->Children().empty()) {
            THROW_ERROR_EXCEPTION("Table is not empty");
        }

        auto* tablet = CreateTablet(table);
        tablet->SetIndex(0);
        tablet->SetPivotKey(EmptyKey());
        table->Tablets().push_back(tablet);

        auto chunkManager = Bootstrap_->GetChunkManager();
        auto* tabletChunkList = chunkManager->CreateChunkList();
        chunkManager->AttachToChunkList(rootChunkList, tabletChunkList);

        LOG_DEBUG_UNLESS(IsRecovery(), "Table is switched to dynamic mode (TableId: %v)",
            table->GetId());
    }


    TTabletCell* GetTabletCellOrThrow(const TTabletCellId& id)
    {
        auto* cell = FindTabletCell(id);
        if (!IsObjectAlive(cell)) {
            THROW_ERROR_EXCEPTION("No such tablet cell %v", id);
        }
        return cell;
    }

    TTabletCellBundle* FindTabletCellBundleByName(const Stroka& name)
    {
        auto it = NameToTabletCellBundleMap_.find(name);
        return it == NameToTabletCellBundleMap_.end() ? nullptr : it->second;
    }

    DECLARE_ENTITY_MAP_ACCESSORS(TabletCellBundle, TTabletCellBundle, TTabletCellBundleId);
    DECLARE_ENTITY_MAP_ACCESSORS(TabletCell, TTabletCell, TTabletCellId);
    DECLARE_ENTITY_MAP_ACCESSORS(Tablet, TTablet, TTabletId);

private:
    friend class TTabletCellBundleTypeHandler;
    friend class TTabletCellTypeHandler;
    friend class TTabletTypeHandler;

    const TTabletManagerConfigPtr Config_;

    const TTabletTrackerPtr TabletTracker_;

    TEntityMap<TTabletCellBundleId, TTabletCellBundle> TabletCellBundleMap_;
    TEntityMap<TTabletCellId, TTabletCell> TabletCellMap_;
    TEntityMap<TTabletId, TTablet> TabletMap_;

    yhash_map<Stroka, TTabletCellBundle*> NameToTabletCellBundleMap_;

    yhash_multimap<Stroka, TTabletCell*> AddressToCell_;
    yhash_map<TTransaction*, TTabletCell*> TransactionToCellMap_;

    TPeriodicExecutorPtr CleanupExecutor_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void SaveKeys(NCellMaster::TSaveContext& context) const
    {
        TabletCellBundleMap_.SaveKeys(context);
        TabletCellMap_.SaveKeys(context);
        TabletMap_.SaveKeys(context);
    }

    void SaveValues(NCellMaster::TSaveContext& context) const
    {
        TabletCellBundleMap_.SaveValues(context);
        TabletCellMap_.SaveValues(context);
        TabletMap_.SaveValues(context);
    }


    void LoadKeys(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (context.GetVersion() >= 202) {
            TabletCellBundleMap_.LoadKeys(context);
        }
        TabletCellMap_.LoadKeys(context);
        TabletMap_.LoadKeys(context);
    }

    void LoadValues(NCellMaster::TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (context.GetVersion() >= 202) {
            TabletCellBundleMap_.LoadValues(context);
        }
        TabletCellMap_.LoadValues(context);
        TabletMap_.LoadValues(context);
    }


    virtual void OnAfterSnapshotLoaded() override
    {
        TMasterAutomatonPart::OnAfterSnapshotLoaded();

        NameToTabletCellBundleMap_.clear();
        for (const auto& pair : TabletCellBundleMap_) {
            auto* bundle = pair.second;
            YCHECK(NameToTabletCellBundleMap_.insert(std::make_pair(bundle->GetName(), bundle)).second);
        }

        AddressToCell_.clear();

        for (const auto& pair : TabletCellMap_) {
            auto* cell = pair.second;
            for (const auto& peer : cell->Peers()) {
                if (!peer.Descriptor.IsNull()) {
                    AddToAddressToCellMap(peer.Descriptor, cell);
                }
            }
            auto* transaction = cell->GetPrerequisiteTransaction();
            if (transaction) {
                YCHECK(TransactionToCellMap_.insert(std::make_pair(transaction, cell)).second);
            }
        }
    }

    virtual void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::Clear();

        TabletCellBundleMap_.Clear();
        TabletCellMap_.Clear();
        TabletMap_.Clear();
        NameToTabletCellBundleMap_.clear();
        AddressToCell_.clear();
        TransactionToCellMap_.clear();
    }


    void OnNodeRegistered(TNode* node)
    {
        node->InitTabletSlots();
    }

    void OnNodeUnregistered(TNode* node)
    {
        for (const auto& slot : node->TabletSlots()) {
            auto* cell = slot.Cell;
            if (cell) {
                LOG_INFO_UNLESS(IsRecovery(), "Tablet cell peer offline: node unregistered (Address: %v, CellId: %v, PeerId: %v)",
                    node->GetDefaultAddress(),
                    cell->GetId(),
                    slot.PeerId);
                cell->DetachPeer(node);
            }
        }
        node->ClearTabletSlots();
    }

    void OnIncrementalHeartbeat(
        TNode* node,
        const TReqIncrementalHeartbeat& request,
        TRspIncrementalHeartbeat* response)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        // Various request helpers.
        auto requestCreateSlot = [&] (const TTabletCell* cell) {
            if (!response)
                return;

            if (!cell->GetPrerequisiteTransaction())
                return;

            auto* protoInfo = response->add_tablet_slots_to_create();

            const auto& cellId = cell->GetId();
            auto peerId = cell->GetPeerId(node->GetDefaultAddress());

            ToProto(protoInfo->mutable_cell_id(), cell->GetId());
            protoInfo->set_peer_id(peerId);
            protoInfo->set_options(ConvertToYsonString(cell->GetOptions()).Data());

            LOG_INFO_UNLESS(IsRecovery(), "Tablet slot creation requested (Address: %v, CellId: %v, PeerId: %v)",
                node->GetDefaultAddress(),
                cellId,
                peerId);
        };

        auto requestConfigureSlot = [&] (const TNode::TTabletSlot* slot) {
            if (!response)
                return;

            const auto* cell = slot->Cell;
            if (!cell->GetPrerequisiteTransaction())
                return;

            auto* protoInfo = response->add_tablet_slots_configure();

            const auto& cellId = cell->GetId();
            auto cellDescriptor = cell->GetDescriptor();

            const auto& prerequisiteTransactionId = cell->GetPrerequisiteTransaction()->GetId();

            ToProto(protoInfo->mutable_cell_descriptor(), cellDescriptor);
            ToProto(protoInfo->mutable_prerequisite_transaction_id(), prerequisiteTransactionId);

            LOG_INFO_UNLESS(IsRecovery(), "Tablet slot configuration update requested "
                "(Address: %v, CellId: %v, Version: %v, PrerequisiteTransactionId: %v)",
                node->GetDefaultAddress(),
                cellId,
                cellDescriptor.ConfigVersion,
                prerequisiteTransactionId);
        };

        auto requestRemoveSlot = [&] (const TTabletCellId& cellId) {
            if (!response)
                return;

            auto* protoInfo = response->add_tablet_slots_to_remove();
            ToProto(protoInfo->mutable_cell_id(), cellId);

            LOG_INFO_UNLESS(IsRecovery(), "Tablet slot removal requested (Address: %v, CellId: %v)",
                node->GetDefaultAddress(),
                cellId);
        };

        const auto* mutationContext = GetCurrentMutationContext();
        auto mutationTimestamp = mutationContext->GetTimestamp();

        const auto& address = node->GetDefaultAddress();

        // Our expectations.
        yhash_set<TTabletCell*> expectedCells;
        for (const auto& slot : node->TabletSlots()) {
            auto* cell = slot.Cell;
            if (IsObjectAlive(cell)) {
                YCHECK(expectedCells.insert(cell).second);
            }
        }

        // Figure out and analyze the reality.
        yhash_set<TTabletCell*> actualCells;
        for (int slotIndex = 0; slotIndex < request.tablet_slots_size(); ++slotIndex) {
            // Pre-erase slot.
            auto& slot = node->TabletSlots()[slotIndex];
            slot = TNode::TTabletSlot();

            const auto& slotInfo = request.tablet_slots(slotIndex);

            auto state = EPeerState(slotInfo.peer_state());
            if (state == EPeerState::None)
                continue;

            auto cellInfo = FromProto<TCellInfo>(slotInfo.cell_info());
            const auto& cellId = cellInfo.CellId;
            auto* cell = FindTabletCell(cellId);
            if (!IsObjectAlive(cell)) {
                LOG_INFO_UNLESS(IsRecovery(), "Unknown tablet slot is running (Address: %v, CellId: %v)",
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            auto peerId = cell->FindPeerId(address);
            if (peerId == InvalidPeerId) {
                LOG_INFO_UNLESS(IsRecovery(), "Unexpected tablet cell is running (Address: %v, CellId: %v)",
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            if (slotInfo.peer_id() != InvalidPeerId && slotInfo.peer_id() != peerId)  {
                LOG_INFO_UNLESS(IsRecovery(), "Invalid peer id for tablet cell: %v instead of %v (Address: %v, CellId: %v)",
                    slotInfo.peer_id(),
                    peerId,
                    address,
                    cellId);
                requestRemoveSlot(cellId);
                continue;
            }

            auto expectedIt = expectedCells.find(cell);
            if (expectedIt == expectedCells.end()) {
                cell->AttachPeer(node, peerId);
                LOG_INFO_UNLESS(IsRecovery(), "Tablet cell peer online (Address: %v, CellId: %v, PeerId: %v)",
                    address,
                    cellId,
                    peerId);
            }

            cell->UpdatePeerSeenTime(peerId, mutationTimestamp);
            YCHECK(actualCells.insert(cell).second);

            // Populate slot.
            slot.Cell = cell;
            slot.PeerState = state;
            slot.PeerId = slot.Cell->GetPeerId(node); // don't trust peerInfo, it may still be InvalidPeerId

            LOG_DEBUG_UNLESS(IsRecovery(), "Tablet cell is running (Address: %v, CellId: %v, PeerId: %v, State: %v, ConfigVersion: %v)",
                address,
                slot.Cell->GetId(),
                slot.PeerId,
                slot.PeerState,
                cellInfo.ConfigVersion);

            if (cellInfo.ConfigVersion != slot.Cell->GetConfigVersion()) {
                requestConfigureSlot(&slot);
            }
        }

        // Check for expected slots that are missing.
        for (auto* cell : expectedCells) {
            if (actualCells.find(cell) == actualCells.end()) {
                LOG_INFO_UNLESS(IsRecovery(), "Tablet cell peer offline: slot is missing (CellId: %v, Address: %v)",
                    cell->GetId(),
                    address);
                cell->DetachPeer(node);
            }
        }

        // Request slot starts.
        {
            int availableSlots = node->Statistics().available_tablet_slots();
            auto range = AddressToCell_.equal_range(address);
            for (auto it = range.first; it != range.second; ++it) {
                auto* cell = it->second;
                if (IsObjectAlive(cell) && actualCells.find(cell) == actualCells.end()) {
                    requestCreateSlot(cell);
                    --availableSlots;
                }
            }
        }

        // Copy tablet statistics, update performance counters.
        auto now = TInstant::Now();
        for (auto& tabletInfo : request.tablets()) {
            auto tabletId = FromProto<TTabletId>(tabletInfo.tablet_id());
            auto* tablet = FindTablet(tabletId);
            if (!tablet || tablet->GetState() != ETabletState::Mounted)
                continue;

            auto* cell = tablet->GetCell();
            cell->TotalStatistics() -= GetTabletStatistics(tablet);
            tablet->NodeStatistics() = tabletInfo.statistics();
            cell->TotalStatistics() += GetTabletStatistics(tablet);

            auto updatePerformanceCounter = [&] (TTabletPerformanceCounter* counter, i64 curValue) {
                i64 prevValue = counter->Count;
                auto timeDelta = std::max(1.0, (now - tablet->PerformanceCounters().Timestamp).SecondsFloat());
                counter->Rate = (std::max(curValue, prevValue) - prevValue) / timeDelta;
                counter->Count = curValue;
            };

            #define XX(name, Name) updatePerformanceCounter( \
                &tablet->PerformanceCounters().Name, \
                tabletInfo.performance_counters().name ## _count());
            ITERATE_TABLET_PERFORMANCE_COUNTERS(XX)
            #undef XX
            tablet->PerformanceCounters().Timestamp = now;
        }
    }


    void AddToAddressToCellMap(const TNodeDescriptor& descriptor, TTabletCell* cell)
    {
        AddressToCell_.insert(std::make_pair(descriptor.GetDefaultAddress(), cell));
    }

    void RemoveFromAddressToCellMap(const TNodeDescriptor& descriptor, TTabletCell* cell)
    {
        auto range = AddressToCell_.equal_range(descriptor.GetDefaultAddress());
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == cell) {
                AddressToCell_.erase(it);
                break;
            }
        }
    }


    void HydraAssignPeers(const TReqAssignPeers& request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TTabletCellId>(request.cell_id());
        auto* cell = FindTabletCell(cellId);
        if (!IsObjectAlive(cell))
            return;

        const auto* mutationContext = GetCurrentMutationContext();
        auto mutationTimestamp = mutationContext->GetTimestamp();

        bool leadingPeerAssigned = false;
        for (const auto& peerInfo : request.peer_infos()) {
            auto peerId = peerInfo.peer_id();
            auto descriptor = FromProto<TNodeDescriptor>(peerInfo.node_descriptor());

            auto& peer = cell->Peers()[peerId];
            if (!peer.Descriptor.IsNull())
                continue;

            if (peerId == cell->GetLeadingPeerId()) {
                leadingPeerAssigned = true;
            }

            AddToAddressToCellMap(descriptor, cell);
            cell->AssignPeer(descriptor, peerId);
            cell->UpdatePeerSeenTime(peerId, mutationTimestamp);

            LOG_INFO_UNLESS(IsRecovery(), "Tablet cell peer assigned (CellId: %v, Address: %v, PeerId: %v)",
                cellId,
                descriptor.GetDefaultAddress(),
                peerId);
        }

        // Once a peer is assigned, we must ensure that the cell has a valid prerequisite transaction.
        if (leadingPeerAssigned || !cell->GetPrerequisiteTransaction()) {
            RestartPrerequisiteTransaction(cell);
        }

        ReconfigureCell(cell);
    }

    void HydraRevokePeers(const TReqRevokePeers& request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TTabletCellId>(request.cell_id());
        auto* cell = FindTabletCell(cellId);
        if (!IsObjectAlive(cell))
            return;

        bool leadingPeerRevoked = false;
        for (auto peerId : request.peer_ids()) {
            if (peerId == cell->GetLeadingPeerId()) {
                leadingPeerRevoked = true;
            }
            DoRevokePeer(cell, peerId);
        }

        if (leadingPeerRevoked) {
            AbortPrerequisiteTransaction(cell);
        }
        ReconfigureCell(cell);
    }

    void HydraSetLeadingPeer(const TReqSetLeadingPeer& request)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto cellId = FromProto<TTabletCellId>(request.cell_id());
        auto* cell = FindTabletCell(cellId);
        if (!IsObjectAlive(cell))
            return;

        auto peerId = request.peer_id();
        cell->SetLeadingPeerId(peerId);

        const auto& descriptor = cell->Peers()[peerId].Descriptor;
        LOG_INFO_UNLESS(IsRecovery(), "Tablet cell leading peer updated (CellId: %v, Address: %v, PeerId: %v)",
            cellId,
            descriptor.GetDefaultAddress(),
            peerId);

        RestartPrerequisiteTransaction(cell);
        ReconfigureCell(cell);
    }

    void HydraOnTabletMounted(const TRspMountTablet& response)
    {
        auto tabletId = FromProto<TTabletId>(response.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet))
            return;

        if (tablet->GetState() != ETabletState::Mounting) {
            LOG_INFO_UNLESS(IsRecovery(), "Mounted notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                tablet->GetState(),
                tabletId);
            return;
        }

        auto* table = tablet->GetTable();
        auto* cell = tablet->GetCell();

        LOG_INFO_UNLESS(IsRecovery(), "Tablet mounted (TableId: %v, TabletId: %v, MountRevision: %v, CellId: %v)",
            table->GetId(),
            tablet->GetId(),
            tablet->GetMountRevision(),
            cell->GetId());

        cell->TotalStatistics() += GetTabletStatistics(tablet);

        tablet->SetState(ETabletState::Mounted);
    }

    void HydraOnTabletUnmounted(const TRspUnmountTablet& response)
    {
        auto tabletId = FromProto<TTabletId>(response.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet))
            return;

        if (tablet->GetState() != ETabletState::Unmounting) {
            LOG_INFO_UNLESS(IsRecovery(), "Unmounted notification received for a tablet in %Qlv state, ignored (TabletId: %v)",
                tablet->GetState(),
                tabletId);
            return;
        }

        DoTabletUnmounted(tablet);
    }

    void DoTabletUnmounted(TTablet* tablet)
    {
        auto* table = tablet->GetTable();
        auto* cell = tablet->GetCell();

        LOG_INFO_UNLESS(IsRecovery(), "Tablet unmounted (TableId: %v, TabletId: %v, CellId: %v)",
            table->GetId(),
            tablet->GetId(),
            cell->GetId());

        cell->TotalStatistics() -= GetTabletStatistics(tablet);

        tablet->NodeStatistics().Clear();
        tablet->PerformanceCounters() = TTabletPerformanceCounters();
        tablet->SetInMemoryMode(EInMemoryMode::None);
        tablet->SetState(ETabletState::Unmounted);
        tablet->SetCell(nullptr);

        auto objectManager = Bootstrap_->GetObjectManager();
        YCHECK(cell->Tablets().erase(tablet) == 1);
        objectManager->UnrefObject(cell);
    }

    void CopyChunkListIfShared(
        TTableNode* table,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        auto* oldRootChunkList = table->GetChunkList();
        auto& chunkLists = oldRootChunkList->Children();
        auto chunkManager = Bootstrap_->GetChunkManager();
        auto objectManager = Bootstrap_->GetObjectManager();

        if (table->GetChunkList()->GetObjectRefCounter() > 1) {
            auto statistics = oldRootChunkList->Statistics();
            auto* newRootChunkList = chunkManager->CreateChunkList();

            chunkManager->AttachToChunkList(
                newRootChunkList,
                chunkLists.data(),
                chunkLists.data() + firstTabletIndex);

            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* tabletChunkList = chunkLists[index]->AsChunkList();
                auto* newTabletChunkList = chunkManager->CreateChunkList();
                chunkManager->AttachToChunkList(newTabletChunkList, tabletChunkList->Children());
                chunkManager->AttachToChunkList(newRootChunkList, newTabletChunkList);
            }

            chunkManager->AttachToChunkList(
                newRootChunkList,
                chunkLists.data() + lastTabletIndex + 1,
                chunkLists.data() + chunkLists.size());

            // Replace root chunk list.
            table->SetChunkList(newRootChunkList);
            newRootChunkList->AddOwningNode(table);
            objectManager->RefObject(newRootChunkList);
            oldRootChunkList->RemoveOwningNode(table);
            objectManager->UnrefObject(oldRootChunkList);
            YCHECK(newRootChunkList->Statistics() == statistics);
        } else {
            auto statistics = oldRootChunkList->Statistics();

            for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
                auto* tabletChunkList = chunkLists[index]->AsChunkList();
                if (tabletChunkList->GetObjectRefCounter() > 1) {
                    auto* newTabletChunkList = chunkManager->CreateChunkList();
                    chunkManager->AttachToChunkList(newTabletChunkList, tabletChunkList->Children());
                    chunkLists[index] = newTabletChunkList;

                    // TODO(savrus): make a helper to replace a tablet chunk list.
                    newTabletChunkList->AddParent(oldRootChunkList);
                    objectManager->RefObject(newTabletChunkList);
                    tabletChunkList->RemoveParent(oldRootChunkList);
                    objectManager->UnrefObject(tabletChunkList);
                }
            }

            YCHECK(oldRootChunkList->Statistics() == statistics);
        }
    }

    void HydraUpdateTabletStores(const TReqUpdateTabletStores& request)
    {
        auto tabletId = FromProto<TTabletId>(request.tablet_id());
        auto* tablet = FindTablet(tabletId);
        if (!IsObjectAlive(tablet)) {
            return;
        }

        auto mountRevision = request.mount_revision();

        // NB: Stores may be updated while unmounting to facilitate flush.
        if (tablet->GetState() != ETabletState::Mounted &&
            tablet->GetState() != ETabletState::Unmounting)
        {
            LOG_INFO_UNLESS(IsRecovery(), "Requested to update stores for a tablet in %Qlv state, ignored (TabletId: %v)",
                tablet->GetState(),
                tabletId);
            return;
        }

        auto* cell = tablet->GetCell();
        auto* table = tablet->GetTable();
        if (!IsObjectAlive(table)) {
            return;
        }

        auto cypressManager = Bootstrap_->GetCypressManager();
        cypressManager->SetModified(table, nullptr);

        TRspUpdateTabletStores response;
        response.mutable_tablet_id()->MergeFrom(request.tablet_id());
        // NB: Take mount revision from the request, not from the tablet.
        response.set_mount_revision(mountRevision);
        response.mutable_stores_to_add()->MergeFrom(request.stores_to_add());
        response.mutable_stores_to_remove()->MergeFrom(request.stores_to_remove());

        try {
            tablet->ValidateMountRevision(mountRevision);

            auto chunkManager = Bootstrap_->GetChunkManager();
            auto securityManager = Bootstrap_->GetSecurityManager();

            // Collect all changes first.
            std::vector<TChunkTree*> chunksToAttach;
            i64 attachedRowCount = 0;
            for (const auto& descriptor : request.stores_to_add()) {
                auto storeId = FromProto<TStoreId>(descriptor.store_id());
                if (TypeFromId(storeId) == EObjectType::Chunk ||
                    TypeFromId(storeId) == EObjectType::ErasureChunk)
                {
                    auto* chunk = chunkManager->GetChunkOrThrow(storeId);
                    const auto& miscExt = chunk->MiscExt();
                    attachedRowCount += miscExt.row_count();
                    chunksToAttach.push_back(chunk);
                }
            }

            std::vector<TChunkTree*> chunksToDetach;
            i64 detachedRowCount = 0;
            for (const auto& descriptor : request.stores_to_remove()) {
                auto storeId = FromProto<TStoreId>(descriptor.store_id());
                if (TypeFromId(storeId) == EObjectType::Chunk ||
                    TypeFromId(storeId) == EObjectType::ErasureChunk)
                {
                    auto* chunk = chunkManager->GetChunkOrThrow(storeId);
                    const auto& miscExt = chunk->MiscExt();
                    detachedRowCount += miscExt.row_count();
                    chunksToDetach.push_back(chunk);
                }
            }

            // Copy chunk tree if somebody holds a reference.
            CopyChunkListIfShared(table, tablet->GetIndex(), tablet->GetIndex());

            // Apply all requested changes.
            cell->TotalStatistics() -= GetTabletStatistics(tablet);
            auto* chunkList = table->GetChunkList()->Children()[tablet->GetIndex()]->AsChunkList();
            chunkManager->AttachToChunkList(chunkList, chunksToAttach);
            chunkManager->DetachFromChunkList(chunkList, chunksToDetach);
            cell->TotalStatistics() += GetTabletStatistics(tablet);
            table->SnapshotStatistics() = table->GetChunkList()->Statistics().ToDataStatistics();

            // Unstage just attached chunks.
            // Update table resource usage.
            for (auto* chunk : chunksToAttach) {
                chunkManager->UnstageChunk(chunk->AsChunk());
            }
            securityManager->UpdateAccountNodeUsage(table);

            LOG_DEBUG_UNLESS(IsRecovery(), "Tablet stores updated (TableId: %v, TabletId: %v, "
                "AttachedChunkIds: %v, DetachedChunkIds: %v, "
                "AttachedRowCount: %v, DetachedRowCount: %v)",
                table->GetId(),
                tabletId,
                ToObjectIds(chunksToAttach),
                ToObjectIds(chunksToDetach),
                attachedRowCount,
                detachedRowCount);
        } catch (const std::exception& ex) {
            auto error = TError(ex);
            LOG_WARNING_UNLESS(IsRecovery(), error, "Error updating tablet stores (TabletId: %v)",
                tabletId);
            ToProto(response.mutable_error(), error.Sanitize());
        }

        auto hiveManager = Bootstrap_->GetHiveManager();
        auto* mailbox = hiveManager->GetMailbox(cell->GetId());
        hiveManager->PostMessage(mailbox, response);
    }


    virtual void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnLeaderActive();

        TabletTracker_->Start();

        for (const auto& pair : TabletCellMap_) {
            auto* cell = pair.second;
            UpdateCellDirectory(cell);
        }

        CleanupExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetEpochAutomatonInvoker(),
            BIND(&TImpl::OnCleanup, MakeWeak(this)),
            CleanupPeriod);
        CleanupExecutor_->Start();
    }

    virtual void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TMasterAutomatonPart::OnStopLeading();

        TabletTracker_->Stop();

        if (CleanupExecutor_) {
            CleanupExecutor_->Stop();
            CleanupExecutor_.Reset();
        }
    }


    void ReconfigureCell(TTabletCell* cell)
    {
        cell->SetConfigVersion(cell->GetConfigVersion() + 1);

        auto config = cell->GetConfig();
        config->Addresses.clear();
        for (const auto& peer : cell->Peers()) {
            auto nodeTracker = Bootstrap_->GetNodeTracker();
            if (peer.Descriptor.IsNull()) {
                config->Addresses.push_back(Null);
            } else {
                config->Addresses.push_back(peer.Descriptor.GetInterconnectAddress());
            }
        }

        UpdateCellDirectory(cell);

        LOG_INFO_UNLESS(IsRecovery(), "Tablet cell reconfigured (CellId: %v, Version: %v)",
            cell->GetId(),
            cell->GetConfigVersion());
    }

    void UpdateCellDirectory(TTabletCell* cell)
    {
        auto cellDirectory = Bootstrap_->GetCellDirectory();
        cellDirectory->ReconfigureCell(cell->GetDescriptor());
    }


    void ValidateHasHealthyCells()
    {
        for (const auto& pair : TabletCellMap_) {
            auto* cell = pair.second;
            if (cell->GetHealth() == ETabletCellHealth::Good)
                return;
        }
        THROW_ERROR_EXCEPTION("No healthy tablet cells");
    }

    std::vector<std::pair<TTablet*, TTabletCell*>> ComputeTabletAssignment(
        TTableNode* table,
        TTableMountConfigPtr mountConfig,
        TTabletCell* hintedCell,
        std::vector<TTablet*> tabletsToMount)
    {
        if (hintedCell) {
            std::vector<std::pair<TTablet*, TTabletCell*>> assignment;
            for (auto* tablet : tabletsToMount) {
                assignment.emplace_back(tablet, hintedCell);
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

        auto getCellSize = [&] (const TTabletCell* cell) -> i64 {
            i64 result = 0;
            switch (mountConfig->InMemoryMode) {
                case EInMemoryMode::None:
                    result += cell->TotalStatistics().UncompressedDataSize;
                    break;
                case EInMemoryMode::Uncompressed:
                case EInMemoryMode::Compressed:
                    result += cell->TotalStatistics().MemorySize;
                    break;
                default:
                    YUNREACHABLE();
            }
            result += cell->Tablets().size() * Config_->TabletDataSizeFootprint;
            return result;
        };

        std::set<TCellKey> cellKeys;
        for (const auto& pair : TabletCellMap_) {
            auto* cell = pair.second;
            if (cell->GetHealth() == ETabletCellHealth::Good) {
                YCHECK(cellKeys.insert(TCellKey{getCellSize(cell), cell}).second);
            }
        }
        YCHECK(!cellKeys.empty());

        auto getTabletSize = [&] (const TTablet* tablet) -> i64 {
            i64 result = 0;
            auto statistics = GetTabletStatistics(tablet);
            switch (mountConfig->InMemoryMode) {
                case EInMemoryMode::None:
                case EInMemoryMode::Uncompressed:
                    result += statistics.UncompressedDataSize;
                    break;
                case EInMemoryMode::Compressed:
                    result += statistics.CompressedDataSize;
                    break;
                default:
                    YUNREACHABLE();
            }
            result += Config_->TabletDataSizeFootprint;
            return result;
        };

        // Sort tablets by decreasing size to improve greedy heuristic performance.
        std::sort(
            tabletsToMount.begin(),
            tabletsToMount.end(),
            [&] (const TTablet* lhs, const TTablet* rhs) {
                return
                    std::make_tuple(getTabletSize(lhs), lhs->GetId()) >
                    std::make_tuple(getTabletSize(rhs), rhs->GetId());
            });

        auto chargeCell = [&] (std::set<TCellKey>::iterator it, TTablet* tablet) {
            const auto& existingKey = *it;
            auto newKey = TCellKey{existingKey.Size + getTabletSize(tablet), existingKey.Cell};
            cellKeys.erase(it);
            YCHECK(cellKeys.insert(newKey).second);
        };

        // Iteratively assign tablets to least-loaded cells.
        std::vector<std::pair<TTablet*, TTabletCell*>> assignment;
        for (auto* tablet : tabletsToMount) {
            auto it = cellKeys.begin();
            assignment.emplace_back(tablet, it->Cell);
            chargeCell(it, tablet);
        }

        return assignment;
    }


    void RestartPrerequisiteTransaction(TTabletCell* cell)
    {
        AbortPrerequisiteTransaction(cell);
        StartPrerequisiteTransaction(cell);
    }

    void StartPrerequisiteTransaction(TTabletCell* cell)
    {
        auto multicellManager = Bootstrap_->GetMulticellManager();
        const auto& secondaryCellTags = multicellManager->GetRegisteredMasterCellTags();

        auto transactionManager = Bootstrap_->GetTransactionManager();
        auto* transaction = transactionManager->StartTransaction(
            nullptr,
            secondaryCellTags,
            Null,
            Format("Prerequisite for cell %v", cell->GetId()));

        YCHECK(!cell->GetPrerequisiteTransaction());
        cell->SetPrerequisiteTransaction(transaction);
        YCHECK(TransactionToCellMap_.insert(std::make_pair(transaction, cell)).second);

        LOG_INFO_UNLESS(IsRecovery(), "Tablet cell prerequisite transaction started (CellId: %v, TransactionId: %v)",
            cell->GetId(),
            transaction->GetId());
    }

    void AbortPrerequisiteTransaction(TTabletCell* cell)
    {
        auto* transaction = cell->GetPrerequisiteTransaction();
        if (!transaction)
            return;

        // Suppress calling OnTransactionFinished.
        YCHECK(TransactionToCellMap_.erase(transaction) == 1);
        cell->SetPrerequisiteTransaction(nullptr);

        // NB: Make a copy, transaction will die soon.
        auto transactionId = transaction->GetId();

        auto transactionManager = Bootstrap_->GetTransactionManager();
        transactionManager->AbortTransaction(transaction, true);

        auto cypressManager = Bootstrap_->GetCypressManager();
        auto cellNodeProxy = GetCellNode(cell->GetId());
        cypressManager->AbortSubtreeTransactions(cellNodeProxy);

        LOG_INFO_UNLESS(IsRecovery(), "Tablet cell prerequisite aborted (CellId: %v, TransactionId: %v)",
            cell->GetId(),
            transactionId);
    }

    void OnTransactionFinished(TTransaction* transaction)
    {
        auto it = TransactionToCellMap_.find(transaction);
        if (it == TransactionToCellMap_.end())
            return;

        auto* cell = it->second;
        cell->SetPrerequisiteTransaction(nullptr);
        TransactionToCellMap_.erase(it);

        LOG_INFO_UNLESS(IsRecovery(), "Tablet cell prerequisite transaction aborted (CellId: %v, TransactionId: %v)",
            cell->GetId(),
            transaction->GetId());

        for (auto peerId = 0; peerId < cell->Peers().size(); ++peerId) {
            DoRevokePeer(cell, peerId);
        }
    }


    void DoRevokePeer(TTabletCell* cell, TPeerId peerId)
    {
        const auto& peer = cell->Peers()[peerId];
        const auto& descriptor = peer.Descriptor;
        if (descriptor.IsNull())
            return;

        LOG_INFO_UNLESS(IsRecovery(), "Tablet cell peer revoked (CellId: %v, Address: %v, PeerId: %v)",
            cell->GetId(),
            descriptor.GetDefaultAddress(),
            peerId);

        if (peer.Node) {
            peer.Node->DetachTabletCell(cell);
        }
        RemoveFromAddressToCellMap(descriptor, cell);
        cell->RevokePeer(peerId);
    }

    void DoUnmountTable(
        TTableNode* table,
        bool force,
        int firstTabletIndex,
        int lastTabletIndex)
    {
        auto hiveManager = Bootstrap_->GetHiveManager();

        for (int index = firstTabletIndex; index <= lastTabletIndex; ++index) {
            auto* tablet = table->Tablets()[index];
            auto* cell = tablet->GetCell();

            if (tablet->GetState() == ETabletState::Mounted) {
                LOG_INFO_UNLESS(IsRecovery(), "Unmounting tablet (TableId: %v, TabletId: %v, CellId: %v, Force: %v)",
                    table->GetId(),
                    tablet->GetId(),
                    cell->GetId(),
                    force);

                tablet->SetState(ETabletState::Unmounting);
            }

            if (cell) {
                TReqUnmountTablet request;
                ToProto(request.mutable_tablet_id(), tablet->GetId());
                request.set_force(force);
                auto* mailbox = hiveManager->GetMailbox(cell->GetId());
                hiveManager->PostMessage(mailbox, request);
            }

            if (force && tablet->GetState() != ETabletState::Unmounted) {
                DoTabletUnmounted(tablet);
            }
        }
    }


    void GetTableSettings(
        TTableNode* table,
        TTableMountConfigPtr* mountConfig,
        TTableWriterOptionsPtr* writerOptions)
    {
        auto objectManager = Bootstrap_->GetObjectManager();
        auto tableProxy = objectManager->GetProxy(table);
        const auto& tableAttributes = tableProxy->Attributes();

        // Parse and prepare mount config.
        try {
            *mountConfig = ConvertTo<TTableMountConfigPtr>(tableAttributes);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing table mount configuration")
                << ex;
        }

        // Prepare tablet writer options.
        *writerOptions = New<TTableWriterOptions>();
        (*writerOptions)->ReplicationFactor = table->GetReplicationFactor();
        (*writerOptions)->Account = table->GetAccount()->GetName();
        (*writerOptions)->CompressionCodec = tableAttributes.Get<NCompression::ECodec>("compression_codec");
        (*writerOptions)->ErasureCodec = tableAttributes.Get<NErasure::ECodec>("erasure_codec", NErasure::ECodec::None);
        (*writerOptions)->ChunksVital = table->GetVital();
    }

    static void ParseTabletRange(
        TTableNode* table,
        int* first,
        int* last)
    {
        auto& tablets = table->Tablets();
        if (*first == -1 && *last == -1) {
            *first = 0;
            *last = static_cast<int>(tablets.size() - 1);
        } else {
            if (tablets.empty()) {
                THROW_ERROR_EXCEPTION("Table has no tablets");
            }
            if (*first < 0 || *first >= tablets.size()) {
                THROW_ERROR_EXCEPTION("First tablet index %v is out of range [%v, %v]",
                    *first,
                    0,
                    tablets.size() - 1);
            }
            if (*last < 0 || *last >= tablets.size()) {
                THROW_ERROR_EXCEPTION("Last tablet index %v is out of range [%v, %v]",
                    *last,
                    0,
                    tablets.size() - 1);
            }
            if (*first > *last) {
                THROW_ERROR_EXCEPTION("First tablet index is greater than last tablet index");
            }
        }
    }


    IMapNodePtr GetCellMapNode()
    {
        auto cypressManager = Bootstrap_->GetCypressManager();
        auto resolver = cypressManager->CreateResolver();
        return resolver->ResolvePath("//sys/tablet_cells")->AsMap();
    }

    INodePtr GetCellNode(const TCellId& cellId)
    {
        auto cypressManager = Bootstrap_->GetCypressManager();
        auto resolver = cypressManager->CreateResolver();
        return resolver->ResolvePath(Format("//sys/tablet_cells/%v", cellId));
    }


    void OnCleanup()
    {
        try {
            auto cypressManager = Bootstrap_->GetCypressManager();
            auto resolver = cypressManager->CreateResolver();
            for (const auto& pair : TabletCellMap_) {
                const auto& cellId = pair.first;
                const auto* cell = pair.second;
                if (!IsObjectAlive(cell))
                    continue;

                auto snapshotsPath = Format("//sys/tablet_cells/%v/snapshots", cellId);
                IMapNodePtr snapshotsMap;
                try {
                    snapshotsMap = resolver->ResolvePath(snapshotsPath)->AsMap();
                } catch (const std::exception&) {
                    continue;
                }

                std::vector<int> snapshotIds;
                auto snapshotKeys = SyncYPathList(snapshotsMap, "");
                for (const auto& key : snapshotKeys) {
                    int snapshotId;
                    try {
                        snapshotId = FromString<int>(key);
                    } catch (const std::exception& ex) {
                        LOG_WARNING("Unrecognized item %Qv in tablet snapshot store (CellId: %v)",
                            key,
                            cellId);
                        continue;
                    }
                    snapshotIds.push_back(snapshotId);
                }

                if (snapshotIds.size() <= Config_->MaxSnapshotsToKeep)
                    continue;

                std::sort(snapshotIds.begin(), snapshotIds.end());
                int thresholdId = snapshotIds[snapshotIds.size() - Config_->MaxSnapshotsToKeep];

                auto objectManager = Bootstrap_->GetObjectManager();
                auto rootService = objectManager->GetRootService();

                for (const auto& key : snapshotKeys) {
                    try {
                        int snapshotId = FromString<int>(key);
                        if (snapshotId < thresholdId) {
                            LOG_INFO("Removing tablet cell snapshot %v (CellId: %v)",
                                snapshotId,
                                cellId);
                            auto req = TYPathProxy::Remove(snapshotsPath + "/" + key);
                            ExecuteVerb(rootService, req).Subscribe(BIND([=] (const TYPathProxy::TErrorOrRspRemovePtr& rspOrError) {
                                if (rspOrError.IsOK()) {
                                    LOG_INFO("Tablet cell snapshot %v removed successfully (CellId: %v)",
                                        snapshotId,
                                        cellId);
                                } else {
                                    LOG_INFO(rspOrError, "Error removing tablet cell snapshot %v (CellId: %v)",
                                        snapshotId,
                                        cellId);
                                }
                            }));
                        }
                    } catch (const std::exception& ex) {
                        // Ignore, cf. logging above.
                    }
                }

                auto changelogsPath = Format("//sys/tablet_cells/%v/changelogs", cellId);
                IMapNodePtr changelogsMap;
                try {
                    changelogsMap = resolver->ResolvePath(changelogsPath)->AsMap();
                } catch (const std::exception&) {
                    continue;
                }

                auto changelogKeys = SyncYPathList(changelogsMap, "");
                for (const auto& key : changelogKeys) {
                    int changelogId;
                    try {
                        changelogId = FromString<int>(key);
                    } catch (const std::exception& ex) {
                        LOG_WARNING("Unrecognized item %Qv in tablet changelog store (CellId: %v)",
                            key,
                            cellId);
                        continue;
                    }
                    if (changelogId < thresholdId) {
                        LOG_INFO("Removing tablet cell changelog %v (CellId: %v)",
                            changelogId,
                            cellId);
                        auto req = TYPathProxy::Remove(changelogsPath + "/" + key);
                        ExecuteVerb(rootService, req).Subscribe(BIND([=] (const TYPathProxy::TErrorOrRspRemovePtr& rspOrError) {
                            if (rspOrError.IsOK()) {
                                LOG_INFO("Tablet cell changelog %v removed successfully (CellId: %v)",
                                    changelogId,
                                    cellId);
                            } else {
                                LOG_INFO(rspOrError, "Error removing tablet cell changelog %v (CellId: %v)",
                                    changelogId,
                                    cellId);
                            }
                        }));;
                    }
                }
            }
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error performing tablets cleanup");
        }
    }

    std::pair<std::vector<TTablet*>::iterator, std::vector<TTablet*>::iterator> GetIntersectingTablets(
        std::vector<TTablet*>& tablets,
        const TOwningKey& minKey,
        const TOwningKey& maxKey)
    {
        auto beginIt = std::upper_bound(
            tablets.begin(),
            tablets.end(),
            minKey,
            [] (const TOwningKey& key, const TTablet* tablet) {
                return key < tablet->GetPivotKey();
            });

        if (beginIt != tablets.begin()) {
            --beginIt;
        }

        auto endIt = beginIt;
        while (endIt != tablets.end() && maxKey >= (*endIt)->GetPivotKey()) {
            ++endIt;
        }

        return std::make_pair(beginIt, endIt);
    }
};

DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, TabletCellBundle, TTabletCellBundle, TTabletCellBundleId, TabletCellBundleMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, TabletCell, TTabletCell, TTabletCellId, TabletCellMap_)
DEFINE_ENTITY_MAP_ACCESSORS(TTabletManager::TImpl, Tablet, TTablet, TTabletId, TabletMap_)

///////////////////////////////////////////////////////////////////////////////

TTabletManager::TTabletCellBundleTypeHandler::TTabletCellBundleTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->TabletCellBundleMap_)
    , Owner_(owner)
{ }

TObjectBase* TTabletManager::TTabletCellBundleTypeHandler::CreateObject(
    const TObjectId& hintId,
    TTransaction* transaction,
    TAccount* account,
    IAttributeDictionary* attributes,
    const TObjectCreationExtensions& /*extensions*/)
{
    auto name = attributes->Get<Stroka>("name");
    attributes->Remove("name");

    return Owner_->CreateCellBundle(name, attributes, hintId);
}

void TTabletManager::TTabletCellBundleTypeHandler::DoDestroyObject(TTabletCellBundle* bundle)
{
    TObjectTypeHandlerWithMapBase::DoDestroyObject(bundle);
    Owner_->DestroyCellBundle(bundle);
}

///////////////////////////////////////////////////////////////////////////////

TTabletManager::TTabletCellTypeHandler::TTabletCellTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->TabletCellMap_)
    , Owner_(owner)
{ }

TObjectBase* TTabletManager::TTabletCellTypeHandler::CreateObject(
    const TObjectId& hintId,
    TTransaction* transaction,
    TAccount* account,
    IAttributeDictionary* attributes,
    const TObjectCreationExtensions& /*extensions*/)
{
    int peerCount = attributes->Get<int>("peer_count", 1);
    attributes->Remove("peer_count");

    return Owner_->CreateCell(peerCount, attributes, hintId);
}

void TTabletManager::TTabletCellTypeHandler::DoZombifyObject(TTabletCell* cell)
{
    TObjectTypeHandlerWithMapBase::DoZombifyObject(cell);
    // NB: Destroy the cell right away and do not wait for GC to prevent
    // dangling links from occurring in //sys/tablet_cells.
    Owner_->DestroyCell(cell);
}

///////////////////////////////////////////////////////////////////////////////

TTabletManager::TTabletTypeHandler::TTabletTypeHandler(TImpl* owner)
    : TObjectTypeHandlerWithMapBase(owner->Bootstrap_, &owner->TabletMap_)
    , Owner_(owner)
{ }

void TTabletManager::TTabletTypeHandler::DoDestroyObject(TTablet* tablet)
{
    TObjectTypeHandlerWithMapBase::DoDestroyObject(tablet);
    Owner_->DestroyTablet(tablet);
}

///////////////////////////////////////////////////////////////////////////////

TTabletManager::TTabletManager(
    TTabletManagerConfigPtr config,
    NCellMaster::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TTabletManager::~TTabletManager()
{ }

void TTabletManager::Initialize()
{
    return Impl_->Initialize();
}

int TTabletManager::GetAssignedTabletCellCount(const Stroka& address) const
{
    return Impl_->GetAssignedTabletCellCount(address);
}

TTableSchema TTabletManager::GetTableSchema(TTableNode* table)
{
    return Impl_->GetTableSchema(table);
}

TTabletStatistics TTabletManager::GetTabletStatistics(const TTablet* tablet)
{
    return Impl_->GetTabletStatistics(tablet);
}

void TTabletManager::MountTable(
    TTableNode* table,
    int firstTabletIndex,
    int lastTabletIndex,
    const TTabletCellId& cellId)
{
    Impl_->MountTable(
        table,
        firstTabletIndex,
        lastTabletIndex,
        cellId);
}

void TTabletManager::UnmountTable(
    TTableNode* table,
    bool force,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->UnmountTable(
        table,
        force,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::RemountTable(
    TTableNode* table,
    int firstTabletIndex,
    int lastTabletIndex)
{
    Impl_->RemountTable(
        table,
        firstTabletIndex,
        lastTabletIndex);
}

void TTabletManager::ClearTablets(TTableNode* table)
{
    Impl_->ClearTablets(table);
}

void TTabletManager::ReshardTable(
    TTableNode* table,
    int firstTabletIndex,
    int lastTabletIndex,
    const std::vector<TOwningKey>& pivotKeys)
{
    Impl_->ReshardTable(
        table,
        firstTabletIndex,
        lastTabletIndex,
        pivotKeys);
}

void TTabletManager::MakeDynamic(TTableNode* table)
{
    Impl_->MakeDynamic(table);
}

TTabletCell* TTabletManager::GetTabletCellOrThrow(const TTabletCellId& id)
{
    return Impl_->GetTabletCellOrThrow(id);
}

TTabletCellBundle* TTabletManager::FindTabletCellBundleByName(const Stroka& name)
{
    return Impl_->FindTabletCellBundleByName(name);
}

DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, TabletCellBundle, TTabletCellBundle, TTabletCellBundleId, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, TabletCell, TTabletCell, TTabletCellId, *Impl_)
DELEGATE_ENTITY_MAP_ACCESSORS(TTabletManager, Tablet, TTablet, TTabletId, *Impl_)

///////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT
