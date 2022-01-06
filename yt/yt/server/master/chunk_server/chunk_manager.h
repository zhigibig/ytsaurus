#pragma once

#include "public.h"
#include "chunk_placement.h"
#include "chunk_replica.h"
#include "chunk_view.h"
#include "chunk_requisition.h"

#include <yt/yt/server/master/cell_master/public.h>

#include <yt/yt/server/master/chunk_server/proto/chunk_manager.pb.h>

#include <yt/yt/server/lib/hydra_common/entity_map.h>

#include <yt/yt/server/master/object_server/public.h>

#include <yt/yt/ytlib/job_tracker_client/proto/job_tracker_service.pb.h>

#include <yt/yt/ytlib/journal_client/helpers.h>

#include <yt/yt/client/chunk_client/chunk_replica.h>
#include <yt/yt/client/chunk_client/read_limit.h>

#include <yt/yt/library/erasure/impl/public.h>

#include <yt/yt/core/rpc/service_detail.h>

namespace NYT::NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunkManager
    : public TRefCounted
{
public:
    TChunkManager(
        TChunkManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);

    ~TChunkManager();

    void Initialize();

    NYTree::IYPathServicePtr GetOrchidService();

    const TJobRegistryPtr& GetJobRegistry() const;

    std::unique_ptr<NHydra::TMutation> CreateUpdateChunkRequisitionMutation(
        const NProto::TReqUpdateChunkRequisition& request);
    std::unique_ptr<NHydra::TMutation> CreateConfirmChunkListsRequisitionTraverseFinishedMutation(
        const NProto::TReqConfirmChunkListsRequisitionTraverseFinished& request);
    std::unique_ptr<NHydra::TMutation> CreateRegisterChunkEndorsementsMutation(
        const NProto::TReqRegisterChunkEndorsements& request);

    using TCtxExportChunks = NRpc::TTypedServiceContext<
        NChunkClient::NProto::TReqExportChunks,
        NChunkClient::NProto::TRspExportChunks>;
    using TCtxExportChunksPtr = TIntrusivePtr<TCtxExportChunks>;
    std::unique_ptr<NHydra::TMutation> CreateExportChunksMutation(
        TCtxExportChunksPtr context);

    using TCtxImportChunks = NRpc::TTypedServiceContext<
        NChunkClient::NProto::TReqImportChunks,
        NChunkClient::NProto::TRspImportChunks>;
    using TCtxImportChunksPtr = TIntrusivePtr<TCtxImportChunks>;
    std::unique_ptr<NHydra::TMutation> CreateImportChunksMutation(
        TCtxImportChunksPtr context);

    using TCtxExecuteBatch = NRpc::TTypedServiceContext<
        NChunkClient::NProto::TReqExecuteBatch,
        NChunkClient::NProto::TRspExecuteBatch>;
    using TCtxExecuteBatchPtr = TIntrusivePtr<TCtxExecuteBatch>;
    std::unique_ptr<NHydra::TMutation> CreateExecuteBatchMutation(
        TCtxExecuteBatchPtr context);

    using TCtxJobHeartbeat = NRpc::TTypedServiceContext<
        NJobTrackerClient::NProto::TReqHeartbeat,
        NJobTrackerClient::NProto::TRspHeartbeat>;
    using TCtxJobHeartbeatPtr = TIntrusivePtr<TCtxJobHeartbeat>;

    DECLARE_ENTITY_MAP_ACCESSORS(Chunk, TChunk);
    TChunk* GetChunkOrThrow(TChunkId id);

    DECLARE_ENTITY_MAP_ACCESSORS(ChunkView, TChunkView);
    TChunkView* GetChunkViewOrThrow(TChunkViewId id);

    DECLARE_ENTITY_MAP_ACCESSORS(DynamicStore, TDynamicStore);
    TDynamicStore* GetDynamicStoreOrThrow(TDynamicStoreId id);

    DECLARE_ENTITY_MAP_ACCESSORS(ChunkList, TChunkList);
    TChunkList* GetChunkListOrThrow(TChunkListId id);

    DECLARE_ENTITY_WITH_IRREGULAR_PLURAL_MAP_ACCESSORS(Medium, Media, TMedium)

    TChunkTree* FindChunkTree(TChunkTreeId id);
    TChunkTree* GetChunkTree(TChunkTreeId id);
    TChunkTree* GetChunkTreeOrThrow(TChunkTreeId id);

    //! This function returns a list of nodes where the replicas can be allocated
    //! or an empty list if the search has not succeeded.
    TNodeList AllocateWriteTargets(
        TMedium* medium,
        TChunk* chunk,
        int desiredCount,
        int minCount,
        std::optional<int> replicationFactorOverride,
        const TNodeList* forbiddenNodes,
        const std::optional<TString>& preferredHostName);

    TNodeList AllocateWriteTargets(
        TMedium* medium,
        TChunk* chunk,
        int replicaIndex,
        int desiredCount,
        int minCount,
        std::optional<int> replicationFactorOverride);

    TChunkList* CreateChunkList(EChunkListKind kind);

    //! For ordered tablets, copies all chunks taking trimmed chunks into account
    //! and updates cumulative statistics accordingly. If all chunks were trimmed
    //! then a nullptr chunk is appended to a cloned chunk list.
    //!
    //! For sorted tablets, cloned chunk list is flattened.
    TChunkList* CloneTabletChunkList(TChunkList* chunkList);

    void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree* const* childrenBegin,
        TChunkTree* const* childrenEnd);
    void AttachToChunkList(
        TChunkList* chunkList,
        const std::vector<TChunkTree*>& children);
    void AttachToChunkList(
        TChunkList* chunkList,
        TChunkTree* child);

    void DetachFromChunkList(
        TChunkList* chunkList,
        TChunkTree* const* childrenBegin,
        TChunkTree* const* childrenEnd);
    void DetachFromChunkList(
        TChunkList* chunkList,
        const std::vector<TChunkTree*>& children);
    void DetachFromChunkList(
        TChunkList* chunkList,
        TChunkTree* child);
    void ReplaceChunkListChild(
        TChunkList* chunkList,
        int childIndex,
        TChunkTree* newChild);

    //! Creates #EChunkListKind::HunkRoot child of #tabletChunkList (if missing).
    TChunkList* GetOrCreateHunkChunkList(TChunkList* tabletChunkList);
    //! Similar to #AttachToChunkList but also handles hunk chunks in #children
    //! by attaching them to a separate hunk root child of #chunkList (creating it on demand).
    void AttachToTabletChunkList(
        TChunkList* tabletChunkList,
        const std::vector<TChunkTree*>& children);

    TChunkView* CreateChunkView(TChunkTree* underlyingTree, TChunkViewModifier modifier);
    TChunkView* CloneChunkView(TChunkView* chunkView, NChunkClient::TLegacyReadRange readRange);

    TChunk* CreateChunk(
        NTransactionServer::TTransaction* transaction,
        TChunkList* chunkList,
        NObjectClient::EObjectType chunkType,
        NSecurityServer::TAccount* account,
        int replicationFactor,
        NErasure::ECodec erasureCodecId,
        TMedium* medium,
        int readQuorum,
        int writeQuorum,
        bool movable,
        bool vital,
        bool overlayed = false,
        NChunkClient::TConsistentReplicaPlacementHash consistentReplicaPlacementHash = NChunkClient::NullConsistentReplicaPlacementHash,
        i64 replicaLagLimit = 0);

    TDynamicStore* CreateDynamicStore(TDynamicStoreId storeId, NTabletServer::TTablet* tablet);

    void RebalanceChunkTree(TChunkList* chunkList);

    void UnstageChunk(TChunk* chunk);
    void UnstageChunkList(TChunkList* chunkList, bool recursive);

    TNodePtrWithIndexesList LocateChunk(TChunkPtrWithIndexes chunkWithIndexes);
    void TouchChunk(TChunk* chunk);

    void ClearChunkList(TChunkList* chunkList);

    void ProcessJobHeartbeat(TNode* node, const TCtxJobHeartbeatPtr& context);

    TJobId GenerateJobId() const;

    void SealChunk(TChunk* chunk, const NChunkClient::NProto::TChunkSealInfo& info);

    const IChunkAutotomizerPtr& GetChunkAutotomizer() const;

    bool IsChunkReplicatorEnabled();
    bool IsChunkRefreshEnabled();
    bool IsChunkRequisitionUpdateEnabled();
    bool IsChunkSealerEnabled();

    void ScheduleChunkRefresh(TChunk* chunk);
    void ScheduleChunkRequisitionUpdate(TChunkTree* chunkTree);
    void ScheduleChunkSeal(TChunk* chunk);
    void ScheduleChunkMerge(TChunkOwnerBase* node);
    bool IsNodeBeingMerged(NCypressClient::TObjectId nodeId) const;
    TChunkRequisitionRegistry* GetChunkRequisitionRegistry();

    const THashSet<TChunk*>& LostVitalChunks() const;
    const THashSet<TChunk*>& LostChunks() const;
    const THashSet<TChunk*>& OverreplicatedChunks() const;
    const THashSet<TChunk*>& UnderreplicatedChunks() const;
    const THashSet<TChunk*>& DataMissingChunks() const;
    const THashSet<TChunk*>& ParityMissingChunks() const;
    const TOldestPartMissingChunkSet& OldestPartMissingChunks() const;
    const THashSet<TChunk*>& PrecariousChunks() const;
    const THashSet<TChunk*>& PrecariousVitalChunks() const;
    const THashSet<TChunk*>& QuorumMissingChunks() const;
    const THashSet<TChunk*>& UnsafelyPlacedChunks() const;
    const THashSet<TChunk*>& InconsistentlyPlacedChunks() const;
    const THashSet<TChunk*>& ForeignChunks() const;

    //! Returns the total number of all chunk replicas.
    int GetTotalReplicaCount();

    TMediumMap<EChunkStatus> ComputeChunkStatuses(TChunk* chunk);

    //! Computes quorum info for a given journal chunk
    //! by querying a quorum of replicas.
    TFuture<NJournalClient::TChunkQuorumInfo> GetChunkQuorumInfo(
        NChunkServer::TChunk* chunk);
    TFuture<NJournalClient::TChunkQuorumInfo> GetChunkQuorumInfo(
        TChunkId chunkId,
        bool overlayed,
        NErasure::ECodec codecId,
        int readQuorum,
        i64 replicaLagLimit,
        const std::vector<NJournalClient::TChunkReplicaDescriptor>& replicaDescriptors);

    //! Returns the medium with a given id (throws if none).
    TMedium* GetMediumOrThrow(TMediumId id) const;

    //! Returns the medium with a given index (|nullptr| if none).
    TMedium* FindMediumByIndex(int index) const;

    //! Returns the medium with a given index (fails if none).
    TMedium* GetMediumByIndex(int index) const;

    //! Returns the medium with a given index (throws if none).
    TMedium* GetMediumByIndexOrThrow(int index) const;

    //! Renames an existing medium. Throws on name conflict.
    void RenameMedium(TMedium* medium, const TString& newName);

    //! Validates and changes medium priority.
    void SetMediumPriority(TMedium* medium, int priority);

    //! Changes medium config. Triggers global chunk refresh if necessary.
    void SetMediumConfig(TMedium* medium, TMediumConfigPtr newConfig);

    //! Returns the medium with a given name (|nullptr| if none).
    TMedium* FindMediumByName(const TString& name) const;

    //! Returns the medium with a given name (throws if none).
    TMedium* GetMediumByNameOrThrow(const TString& name) const;

    //! Returns chunk replicas "ideal" from CRP point of view.
    //! This reflects the target chunk placement, not the actual one.
    TNodePtrWithIndexesList GetConsistentChunkReplicas(TChunk* chunk) const;

private:
    friend class TChunkTypeHandler;
    friend class TChunkListTypeHandler;
    friend class TChunkViewTypeHandler;
    friend class TDynamicStoreTypeHandler;
    friend class TMediumTypeHandler;

    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

    NHydra::TEntityMap<TChunk>& MutableChunks();
    void DestroyChunk(TChunk* chunk);
    void ExportChunk(TChunk* chunk, NObjectClient::TCellTag destinationCellTag);
    void UnexportChunk(TChunk* chunk, NObjectClient::TCellTag destinationCellTag, int importRefCounter);

    NHydra::TEntityMap<TChunkList>& MutableChunkLists();
    void DestroyChunkList(TChunkList* chunkList);

    NHydra::TEntityMap<TChunkView>& MutableChunkViews();
    void DestroyChunkView(TChunkView* chunkView);

    NHydra::TEntityMap<TDynamicStore>& MutableDynamicStores();
    void DestroyDynamicStore(TDynamicStore* dynamicStore);

    NHydra::TEntityMap<TMedium>& MutableMedia();
    TMedium* CreateMedium(
        const TString& name,
        std::optional<bool> transient,
        std::optional<bool> cache,
        std::optional<int> priority,
        NObjectClient::TObjectId hintId);
    void DestroyMedium(TMedium* medium);
};

DEFINE_REFCOUNTED_TYPE(TChunkManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
