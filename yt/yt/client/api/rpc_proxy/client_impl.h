#pragma once

#include "client_base.h"

#include <yt/yt/client/api/client.h>

#include <yt/yt/core/misc/lazy_ptr.h>

#include <yt/yt/core/rpc/public.h>

namespace NYT::NApi::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

class TClient
    : public NApi::IClient
    , public TClientBase
{
public:
    TClient(
        TConnectionPtr connection,
        const TClientOptions& options);

    void Terminate() override;
    const NTabletClient::ITableMountCachePtr& GetTableMountCache() override;
    const NChaosClient::IReplicationCardCachePtr& GetReplicationCardCache() override;
    const NTransactionClient::ITimestampProviderPtr& GetTimestampProvider() override;

    // Transactions.
    NApi::ITransactionPtr AttachTransaction(
        NTransactionClient::TTransactionId transactionId,
        const NApi::TTransactionAttachOptions& options) override;

    // Tables.
    TFuture<void> MountTable(
        const NYPath::TYPath& path,
        const NApi::TMountTableOptions& options) override;

    TFuture<void> UnmountTable(
        const NYPath::TYPath& path,
        const NApi::TUnmountTableOptions& options) override;

    TFuture<void> RemountTable(
        const NYPath::TYPath& path,
        const NApi::TRemountTableOptions& options) override;

    TFuture<void> FreezeTable(
        const NYPath::TYPath& path,
        const NApi::TFreezeTableOptions& options) override;

    TFuture<void> UnfreezeTable(
        const NYPath::TYPath& path,
        const NApi::TUnfreezeTableOptions& options) override;

    TFuture<void> ReshardTable(
        const NYPath::TYPath& path,
        const std::vector<NTableClient::TLegacyOwningKey>& pivotKeys,
        const NApi::TReshardTableOptions& options) override;

    TFuture<void> ReshardTable(
        const NYPath::TYPath& path,
        int tabletCount,
        const NApi::TReshardTableOptions& options) override;

    TFuture<std::vector<NTabletClient::TTabletActionId>> ReshardTableAutomatic(
        const NYPath::TYPath& path,
        const NApi::TReshardTableAutomaticOptions& options) override;

    TFuture<void> TrimTable(
        const NYPath::TYPath& path,
        int tabletIndex,
        i64 trimmedRowCount,
        const NApi::TTrimTableOptions& options) override;

    TFuture<void> AlterTable(
        const NYPath::TYPath& path,
        const NApi::TAlterTableOptions& options) override;

    TFuture<void> AlterTableReplica(
        NTabletClient::TTableReplicaId replicaId,
        const NApi::TAlterTableReplicaOptions& options) override;

    TFuture<NYson::TYsonString> GetTablePivotKeys(
        const NYPath::TYPath& path,
        const TGetTablePivotKeysOptions& options) override;

    TFuture<void> CreateTableBackup(
        const TBackupManifestPtr& manifest,
        const TCreateTableBackupOptions& options) override;

    TFuture<void> RestoreTableBackup(
        const TBackupManifestPtr& manifest,
        const TRestoreTableBackupOptions& options) override;

    TFuture<std::vector<NTabletClient::TTableReplicaId>> GetInSyncReplicas(
        const NYPath::TYPath& path,
        const NTableClient::TNameTablePtr& nameTable,
        const TSharedRange<NTableClient::TLegacyKey>& keys,
        const NApi::TGetInSyncReplicasOptions& options) override;

    TFuture<std::vector<NTabletClient::TTableReplicaId>> GetInSyncReplicas(
        const NYPath::TYPath& path,
        const NApi::TGetInSyncReplicasOptions& options) override;

    TFuture<std::vector<NApi::TTabletInfo>> GetTabletInfos(
        const NYPath::TYPath& path,
        const std::vector<int>& tabletIndexes,
        const NApi::TGetTabletInfosOptions& options) override;

    TFuture<TGetTabletErrorsResult> GetTabletErrors(
        const NYPath::TYPath& path,
        const NApi::TGetTabletErrorsOptions& options) override;

    TFuture<std::vector<NTabletClient::TTabletActionId>> BalanceTabletCells(
        const TString& tabletCellBundle,
        const std::vector<NYPath::TYPath>& movableTables,
        const NApi::TBalanceTabletCellsOptions& options) override;

    TFuture<NChaosClient::TReplicationCardPtr> GetReplicationCard(
        NChaosClient::TReplicationCardId replicationCardId,
        const TGetReplicationCardOptions& options = {}) override;

    TFuture<void> UpdateChaosTableReplicaProgress(
        NChaosClient::TReplicaId replicaId,
        const TUpdateChaosTableReplicaProgressOptions& options = {}) override;

    TFuture<void> AlterReplicationCard(
        NChaosClient::TReplicationCardId replicationCardId,
        const TAlterReplicationCardOptions& options = {}) override;

    // Queues.
    TFuture<NQueueClient::IQueueRowsetPtr> PullQueue(
        const NYPath::TRichYPath& queuePath,
        i64 offset,
        int partitionIndex,
        const NQueueClient::TQueueRowBatchReadOptions& rowBatchReadOptions,
        const TPullQueueOptions& options = {}) override;

    // Files.
    TFuture<NApi::TGetFileFromCacheResult> GetFileFromCache(
        const TString& md5,
        const NApi::TGetFileFromCacheOptions& options) override;

    TFuture<NApi::TPutFileToCacheResult> PutFileToCache(
        const NYPath::TYPath& path,
        const TString& expectedMD5,
        const NApi::TPutFileToCacheOptions& options) override;

    // Security.
    TFuture<void> AddMember(
        const TString& group,
        const TString& member,
        const NApi::TAddMemberOptions& options) override;

    TFuture<void> RemoveMember(
        const TString& group,
        const TString& member,
        const NApi::TRemoveMemberOptions& options) override;

    TFuture<TCheckPermissionResponse> CheckPermission(
        const TString& user,
        const NYPath::TYPath& path,
        NYTree::EPermission permission,
        const NApi::TCheckPermissionOptions& options) override;

    TFuture<TCheckPermissionByAclResult> CheckPermissionByAcl(
        const std::optional<TString>& user,
        NYTree::EPermission permission,
        NYTree::INodePtr acl,
        const NApi::TCheckPermissionByAclOptions& options) override;

    TFuture<void> TransferAccountResources(
        const TString& srcAccount,
        const TString& dstAccount,
        NYTree::INodePtr resourceDelta,
        const TTransferAccountResourcesOptions& options) override;

    // Scheduler pools.
    virtual TFuture<void> TransferPoolResources(
        const TString& srcPool,
        const TString& dstPool,
        const TString& poolTree,
        NYTree::INodePtr resourceDelta,
        const TTransferPoolResourcesOptions& options) override;

    // Scheduler.
    TFuture<NScheduler::TOperationId> StartOperation(
        NScheduler::EOperationType type,
        const NYson::TYsonString& spec,
        const NApi::TStartOperationOptions& options) override;

    TFuture<void> AbortOperation(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        const NApi::TAbortOperationOptions& options) override;

    TFuture<void> SuspendOperation(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        const NApi::TSuspendOperationOptions& options) override;

    TFuture<void> ResumeOperation(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        const NApi::TResumeOperationOptions& options) override;

    TFuture<void> CompleteOperation(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        const NApi::TCompleteOperationOptions& options) override;

    TFuture<void> UpdateOperationParameters(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        const NYson::TYsonString& parameters,
        const NApi::TUpdateOperationParametersOptions& options) override;

    TFuture<TOperation> GetOperation(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        const NApi::TGetOperationOptions& options) override;

    TFuture<void> DumpJobContext(
        NJobTrackerClient::TJobId jobId,
        const NYPath::TYPath& path,
        const NApi::TDumpJobContextOptions& options) override;

    TFuture<NConcurrency::IAsyncZeroCopyInputStreamPtr> GetJobInput(
        NJobTrackerClient::TJobId jobId,
        const NApi::TGetJobInputOptions& options) override;

    TFuture<NYson::TYsonString> GetJobInputPaths(
        NJobTrackerClient::TJobId jobId,
        const NApi::TGetJobInputPathsOptions& options) override;

    TFuture<NYson::TYsonString> GetJobSpec(
        NJobTrackerClient::TJobId jobId,
        const NApi::TGetJobSpecOptions& options) override;

    TFuture<TSharedRef> GetJobStderr(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        NJobTrackerClient::TJobId jobId,
        const NApi::TGetJobStderrOptions& options) override;

    TFuture<TSharedRef> GetJobFailContext(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        NJobTrackerClient::TJobId jobId,
        const NApi::TGetJobFailContextOptions& options) override;

    TFuture<NApi::TListOperationsResult> ListOperations(
        const NApi::TListOperationsOptions& options) override;

    TFuture<NApi::TListJobsResult> ListJobs(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        const NApi::TListJobsOptions&) override;

    TFuture<NYson::TYsonString> GetJob(
        const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
        NJobTrackerClient::TJobId jobId,
        const NApi::TGetJobOptions& options) override;

    TFuture<void> AbandonJob(
        NJobTrackerClient::TJobId job_id,
        const NApi::TAbandonJobOptions& options) override;

    TFuture<TPollJobShellResponse> PollJobShell(
        NJobTrackerClient::TJobId jobId,
        const std::optional<TString>& shellName,
        const NYson::TYsonString& parameters,
        const NApi::TPollJobShellOptions& options) override;

    TFuture<void> AbortJob(
        NJobTrackerClient::TJobId jobId,
        const NApi::TAbortJobOptions& options) override;

    // Metadata.
    TFuture<NApi::TClusterMeta> GetClusterMeta(
        const NApi::TGetClusterMetaOptions&) override;

    TFuture<void> CheckClusterLiveness(
        const TCheckClusterLivenessOptions&) override;

    TFuture<NApi::TSkynetSharePartsLocationsPtr> LocateSkynetShare(
        const NYPath::TRichYPath&,
        const NApi::TLocateSkynetShareOptions&) override;

    TFuture<std::vector<NTableClient::TColumnarStatistics>> GetColumnarStatistics(
        const std::vector<NYPath::TRichYPath>& path,
        const NApi::TGetColumnarStatisticsOptions& options) override;

    TFuture<TMultiTablePartitions> PartitionTables(
        const std::vector<NYPath::TRichYPath>& paths,
        const NApi::TPartitionTablesOptions& options) override;

    TFuture<void> TruncateJournal(
        const NYPath::TYPath& path,
        i64 rowCount,
        const NApi::TTruncateJournalOptions& options) override;

    // Administration.
    TFuture<int> BuildSnapshot(
        const NApi::TBuildSnapshotOptions& options) override;

    TFuture<TCellIdToSnapshotIdMap> BuildMasterSnapshots(
        const TBuildMasterSnapshotsOptions& options) override;

    TFuture<void> SwitchLeader(
        NHydra::TCellId cellId,
        const TString& newLeaderAddress,
        const TSwitchLeaderOptions& options) override;

    TFuture<void> ResetStateHash(
        NHydra::TCellId cellId,
        const TResetStateHashOptions& options) override;

    TFuture<void> GCCollect(
        const NApi::TGCCollectOptions& options) override;

    TFuture<void> KillProcess(
        const TString& address,
        const NApi::TKillProcessOptions& options) override;

    TFuture<TString> WriteCoreDump(
        const TString& address,
        const NApi::TWriteCoreDumpOptions& options) override;

    TFuture<TGuid> WriteLogBarrier(
        const TString& address,
        const TWriteLogBarrierOptions& options) override;

    TFuture<TString> WriteOperationControllerCoreDump(
        NJobTrackerClient::TOperationId operationId,
        const NApi::TWriteOperationControllerCoreDumpOptions& options) override;

    TFuture<void> HealExecNode(
        const TString& address,
        const THealExecNodeOptions& options) override;

    TFuture<void> SuspendCoordinator(
        NObjectClient::TCellId coordinatorCellId,
        const TSuspendCoordinatorOptions& options) override;

    TFuture<void> ResumeCoordinator(
        NObjectClient::TCellId coordinatorCellId,
        const TResumeCoordinatorOptions& options) override;

    TFuture<void> MigrateReplicationCards(
        NObjectClient::TCellId chaosCellId,
        const TMigrateReplicationCardsOptions& options) override;

    TFuture<void> SuspendTabletCells(
        const std::vector<NObjectClient::TCellId>& cellIds,
        const TSuspendTabletCellsOptions& options) override;

    TFuture<void> ResumeTabletCells(
        const std::vector<NObjectClient::TCellId>& cellIds,
        const TResumeTabletCellsOptions& options) override;

    // YQL

    TFuture<TStartYqlQueryResult> StartYqlQuery(
        const TString& query,
        const TStartYqlQueryOptions& options) override;

    // Authentication

    virtual TFuture<void> SetUserPassword(
        const TString& user,
        const TString& currentPasswordSha256,
        const TString& newPasswordSha256,
        const TSetUserPasswordOptions& options) override;

private:
    const TConnectionPtr Connection_;
    const NRpc::TDynamicChannelPoolPtr ChannelPool_;
    const NRpc::IChannelPtr RetryingChannel_;
    const TClientOptions ClientOptions_;

    TLazyIntrusivePtr<NTabletClient::ITableMountCache> TableMountCache_;

    TLazyIntrusivePtr<NTransactionClient::ITimestampProvider> TimestampProvider_;

    NTransactionClient::ITimestampProviderPtr CreateTimestampProvider() const;

    NRpc::IChannelPtr MaybeCreateRetryingChannel(NRpc::IChannelPtr channel, bool retryProxyBanned) const;
    // Returns an RPC channel to use for API calls to the particular address (e.g.: AttachTransaction).
    // The channel is non-retrying, so should be wrapped into retrying channel on demand.
    NRpc::IChannelPtr CreateNonRetryingChannelByAddress(const TString& address) const;

    TConnectionPtr GetRpcProxyConnection() override;
    TClientPtr GetRpcProxyClient() override;

    NRpc::IChannelPtr GetRetryingChannel() const override;
    NRpc::IChannelPtr CreateNonRetryingStickyChannel() const override;
    NRpc::IChannelPtr WrapStickyChannelIntoRetrying(NRpc::IChannelPtr underlying) const override;
};

DEFINE_REFCOUNTED_TYPE(TClient)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy
