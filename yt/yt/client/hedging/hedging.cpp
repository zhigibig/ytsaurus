#include "hedging.h"

#include "cache.h"
#include "counter.h"
#include "logger.h"

#include <yt/yt/client/api/client.h>

#include <yt/yt/client/hedging/rpc.h>
#include <yt/yt_proto/yt/client/hedging/proto/config.pb.h>

#include <yt/yt/client/table_client/unversioned_row.h>

#include <yt/yt/client/ypath/rich.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/profiling/timing.h>

#include <yt/yt/core/rpc/dispatcher.h>

#include <library/cpp/iterator/enumerate.h>
#include <library/cpp/iterator/zip.h>

#include <util/datetime/base.h>

#include <util/generic/va_args.h>

#include <util/system/compiler.h>

namespace NYT::NClient::NHedging::NRpc {

////////////////////////////////////////////////////////////////////////////////

namespace {

using namespace NYT;
using namespace NApi;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

using TClientBuilder = std::function<NApi::IClientPtr(const TConfig&)>;

[[noreturn]] void unsupported(TStringBuf n)
{
    ythrow yexception() << "Not supported operation: " << n;
}

#define Y_COMMA() ,

#define Y_FUNCTION_ARG_UNUSED(x) x
#define Y_FUNCTION_ARG(i, x) x Y_CAT(a, i)
#define Y_PASS_ARG(i, x) Y_CAT(a, i)

#define Y_FUNCTION_ARG_UNUSED_COMMA(x) x Y_COMMA()
#define Y_FUNCTION_ARG_COMMA(i, x) x Y_CAT(a, i) Y_COMMA()
#define Y_PASS_ARG_COMMA(i, x) Y_CAT(a, i) Y_COMMA()

#define Y_VA_ARGS(...) __VA_ARGS__

// Map input arguments types.
// Example: Y_METHOD_UNUSED_ARGS_DECLARATION((int, TString)) ---> int, TString.
#define Y_METHOD_UNUSED_ARGS_DECLARATION(Args) Y_MAP_ARGS_WITH_LAST(Y_FUNCTION_ARG_UNUSED_COMMA, Y_FUNCTION_ARG_UNUSED, Y_PASS_VA_ARGS(Y_VA_ARGS Args))

// Map input arguments types with parameter names introduction.
// Example: Y_METHOD_USED_ARGS_DECLARATION((int, TString)) ---> int a2, TString a1.
#define Y_METHOD_USED_ARGS_DECLARATION(Args) Y_MAP_ARGS_WITH_LAST_N(Y_FUNCTION_ARG_COMMA, Y_FUNCTION_ARG, Y_PASS_VA_ARGS(Y_VA_ARGS Args))

// Map input arguments types into corresponding parameter names.
// Example: Y_PASS_METHOD_USED_ARGS((int, TString)) ---> a2, a1.
#define Y_PASS_METHOD_USED_ARGS(Args) Y_MAP_ARGS_WITH_LAST_N(Y_PASS_ARG_COMMA, Y_PASS_ARG, Y_PASS_VA_ARGS(Y_VA_ARGS Args))

// Use at the end of macros declaration. It allows macros usage only with semicolon at the end.
// It prevents from warnings for extra semicolons when building with -pedantic mode.
#define Y_SEMICOLON_GUARD static_assert(true, "missing semicolon")

#define UNSUPPORTED_METHOD(ReturnType, MethodName, Args)                     \
    ReturnType MethodName(Y_METHOD_UNUSED_ARGS_DECLARATION(Args)) override { \
        unsupported(Y_STRINGIZE(MethodName));                                \
        Y_UNREACHABLE();                                                     \
    } Y_SEMICOLON_GUARD

#define RETRYABLE_METHOD(ReturnType, MethodName, Args)                                      \
    ReturnType MethodName(Y_METHOD_USED_ARGS_DECLARATION(Args)) override {                  \
        return Executor_->DoWithHedging<typename ReturnType::TValueType>(BIND([=] (IClientPtr client) { \
            return client->MethodName(Y_PASS_METHOD_USED_ARGS(Args));                       \
        }));                                                                                \
    } Y_SEMICOLON_GUARD

////////////////////////////////////////////////////////////////////////////////

class THedgingClient
    : public IClient
{
public:
    THedgingClient(const THedgingClientOptions& options, const IPenaltyProviderPtr& penaltyProvider)
        : Executor_(New<THedgingExecutor>(options, penaltyProvider))
    {
    }

    // IClientBase methods.
    // Supported methods.
    IConnectionPtr GetConnection() override {
        return Executor_->GetConnection();
    }

    RETRYABLE_METHOD(TFuture<IUnversionedRowsetPtr>, LookupRows, (const NYPath::TYPath&, NTableClient::TNameTablePtr, const TSharedRange<NTableClient::TUnversionedRow>&, const TLookupRowsOptions&));
    RETRYABLE_METHOD(TFuture<IVersionedRowsetPtr>, VersionedLookupRows, (const NYPath::TYPath&, NTableClient::TNameTablePtr, const TSharedRange<NTableClient::TUnversionedRow>&, const TVersionedLookupRowsOptions&));
    RETRYABLE_METHOD(TFuture<TSelectRowsResult>, SelectRows, (const TString&, const TSelectRowsOptions&));
    RETRYABLE_METHOD(TFuture<NQueueClient::TQueueRowsetPtr>, PullQueue, (const NYPath::TRichYPath&, i64, int, const NQueueClient::TQueueRowBatchReadOptions&, const TPullQueueOptions&));
    RETRYABLE_METHOD(TFuture<NYson::TYsonString>, ExplainQuery, (const TString&, const TExplainQueryOptions&));
    RETRYABLE_METHOD(TFuture<ITableReaderPtr>, CreateTableReader, (const NYPath::TRichYPath&, const TTableReaderOptions&));
    RETRYABLE_METHOD(TFuture<NYson::TYsonString>, GetNode, (const NYPath::TYPath&, const TGetNodeOptions&));
    RETRYABLE_METHOD(TFuture<NYson::TYsonString>, ListNode, (const NYPath::TYPath&, const TListNodeOptions&));
    RETRYABLE_METHOD(TFuture<bool>, NodeExists, (const NYPath::TYPath&, const TNodeExistsOptions&));
    RETRYABLE_METHOD(TFuture<IFileReaderPtr>, CreateFileReader, (const NYPath::TYPath&, const TFileReaderOptions&));
    RETRYABLE_METHOD(TFuture<std::vector<IUnversionedRowsetPtr>>, MultiLookup, (const std::vector<TMultiLookupSubrequest>&, const TMultiLookupOptions&));

    // Unsupported methods.
    UNSUPPORTED_METHOD(TFuture<ITransactionPtr>, StartTransaction, (NTransactionClient::ETransactionType, const TTransactionStartOptions&));
    UNSUPPORTED_METHOD(TFuture<ITableWriterPtr>, CreateTableWriter, (const NYPath::TRichYPath&, const TTableWriterOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, SetNode, (const NYPath::TYPath&, const NYson::TYsonString&, const TSetNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, MultisetAttributesNode, (const NYPath::TYPath&, const NYTree::IMapNodePtr&, const TMultisetAttributesNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, RemoveNode, (const NYPath::TYPath&, const TRemoveNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<NCypressClient::TNodeId>, CreateNode, (const NYPath::TYPath&, NObjectClient::EObjectType, const TCreateNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<TLockNodeResult>, LockNode, (const NYPath::TYPath&, NCypressClient::ELockMode, const TLockNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, UnlockNode, (const NYPath::TYPath&, const TUnlockNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<NCypressClient::TNodeId>, CopyNode, (const NYPath::TYPath&, const NYPath::TYPath&, const TCopyNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<NCypressClient::TNodeId>, MoveNode, (const NYPath::TYPath&, const NYPath::TYPath&, const TMoveNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<NCypressClient::TNodeId>, LinkNode, (const NYPath::TYPath&, const NYPath::TYPath&, const TLinkNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, ConcatenateNodes, (const std::vector<NYPath::TRichYPath>&, const NYPath::TRichYPath&, const TConcatenateNodesOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, ExternalizeNode, (const NYPath::TYPath&, NObjectClient::TCellTag, const TExternalizeNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, InternalizeNode, (const NYPath::TYPath&, const TInternalizeNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<NObjectClient::TObjectId>, CreateObject, (NObjectClient::EObjectType, const TCreateObjectOptions&));
    UNSUPPORTED_METHOD(IFileWriterPtr, CreateFileWriter, (const NYPath::TRichYPath&, const TFileWriterOptions&));
    UNSUPPORTED_METHOD(IJournalReaderPtr, CreateJournalReader, (const NYPath::TYPath&, const TJournalReaderOptions&));
    UNSUPPORTED_METHOD(IJournalWriterPtr, CreateJournalWriter, (const NYPath::TYPath&, const TJournalWriterOptions&));

    // IClient methods.
    // Unsupported methods.
    UNSUPPORTED_METHOD(void, Terminate, ());
    UNSUPPORTED_METHOD(const NTabletClient::ITableMountCachePtr&, GetTableMountCache, ());
    UNSUPPORTED_METHOD(const NChaosClient::IReplicationCardCachePtr&, GetReplicationCardCache, ());
    UNSUPPORTED_METHOD(const NTransactionClient::ITimestampProviderPtr&, GetTimestampProvider, ());
    UNSUPPORTED_METHOD(ITransactionPtr, AttachTransaction, (NTransactionClient::TTransactionId, const TTransactionAttachOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, MountTable, (const NYPath::TYPath&, const TMountTableOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, UnmountTable, (const NYPath::TYPath&, const TUnmountTableOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, RemountTable, (const NYPath::TYPath&, const TRemountTableOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, FreezeTable, (const NYPath::TYPath&, const TFreezeTableOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, UnfreezeTable, (const NYPath::TYPath&, const TUnfreezeTableOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, ReshardTable, (const NYPath::TYPath&, const std::vector<NTableClient::TUnversionedOwningRow>&, const TReshardTableOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, ReshardTable, (const NYPath::TYPath&, int, const TReshardTableOptions&));
    UNSUPPORTED_METHOD(TFuture<std::vector<NTabletClient::TTabletActionId>>, ReshardTableAutomatic, (const NYPath::TYPath&, const TReshardTableAutomaticOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, TrimTable, (const NYPath::TYPath&, int, i64, const TTrimTableOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, AlterTable, (const NYPath::TYPath&, const TAlterTableOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, AlterTableReplica, (NTabletClient::TTableReplicaId, const TAlterTableReplicaOptions&));
    UNSUPPORTED_METHOD(TFuture<std::vector<NTabletClient::TTableReplicaId>>, GetInSyncReplicas, (const NYPath::TYPath&, const NTableClient::TNameTablePtr&, const TSharedRange<NTableClient::TUnversionedRow>&, const TGetInSyncReplicasOptions&));
    UNSUPPORTED_METHOD(TFuture<std::vector<NTabletClient::TTableReplicaId>>, GetInSyncReplicas, (const NYPath::TYPath&, const TGetInSyncReplicasOptions&));
    UNSUPPORTED_METHOD(TFuture<std::vector<TTabletInfo>>, GetTabletInfos, (const NYPath::TYPath&, const std::vector<int>&, const TGetTabletInfosOptions&));
    UNSUPPORTED_METHOD(TFuture<TGetTabletErrorsResult>, GetTabletErrors, (const NYPath::TYPath&, const TGetTabletErrorsOptions&));
    UNSUPPORTED_METHOD(TFuture<std::vector<NTabletClient::TTabletActionId>>, BalanceTabletCells, (const TString&, const std::vector<NYPath::TYPath>&, const TBalanceTabletCellsOptions&));
    UNSUPPORTED_METHOD(TFuture<TSkynetSharePartsLocationsPtr>, LocateSkynetShare, (const NYPath::TRichYPath&, const TLocateSkynetShareOptions&));
    UNSUPPORTED_METHOD(TFuture<std::vector<NTableClient::TColumnarStatistics>>, GetColumnarStatistics, (const std::vector<NYPath::TRichYPath>&, const TGetColumnarStatisticsOptions&));
    UNSUPPORTED_METHOD(TFuture<TMultiTablePartitions>, PartitionTables, (const std::vector<NYPath::TRichYPath>&, const TPartitionTablesOptions&));
    UNSUPPORTED_METHOD(TFuture<NYson::TYsonString>, GetTablePivotKeys, (const NYPath::TYPath&, const TGetTablePivotKeysOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, CreateTableBackup, (const TBackupManifestPtr&, const TCreateTableBackupOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, RestoreTableBackup, (const TBackupManifestPtr&, const TRestoreTableBackupOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, TruncateJournal, (const NYPath::TYPath&, i64, const TTruncateJournalOptions&));
    UNSUPPORTED_METHOD(TFuture<TGetFileFromCacheResult>, GetFileFromCache, (const TString&, const TGetFileFromCacheOptions&));
    UNSUPPORTED_METHOD(TFuture<TPutFileToCacheResult>, PutFileToCache, (const NYPath::TYPath&, const TString&, const TPutFileToCacheOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, AddMember, (const TString&, const TString&, const TAddMemberOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, RemoveMember, (const TString&, const TString&, const TRemoveMemberOptions&));
    UNSUPPORTED_METHOD(TFuture<TCheckPermissionResponse>, CheckPermission, (const TString&, const NYPath::TYPath&, NYTree::EPermission, const TCheckPermissionOptions&));
    UNSUPPORTED_METHOD(TFuture<TCheckPermissionByAclResult>, CheckPermissionByAcl, (const std::optional<TString>&, NYTree::EPermission, NYTree::INodePtr, const TCheckPermissionByAclOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, TransferAccountResources, (const TString&, const TString&, NYTree::INodePtr, const TTransferAccountResourcesOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, TransferPoolResources, (const TString&, const TString&, const TString&, NYTree::INodePtr, const TTransferPoolResourcesOptions&));
    UNSUPPORTED_METHOD(TFuture<NScheduler::TOperationId>, StartOperation, (NScheduler::EOperationType, const NYson::TYsonString&, const TStartOperationOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, AbortOperation, (const NScheduler::TOperationIdOrAlias&, const TAbortOperationOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, SuspendOperation, (const NScheduler::TOperationIdOrAlias&, const TSuspendOperationOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, ResumeOperation, (const NScheduler::TOperationIdOrAlias&, const TResumeOperationOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, CompleteOperation, (const NScheduler::TOperationIdOrAlias&, const TCompleteOperationOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, UpdateOperationParameters, (const NScheduler::TOperationIdOrAlias&, const NYson::TYsonString&, const TUpdateOperationParametersOptions&));
    UNSUPPORTED_METHOD(TFuture<TOperation>, GetOperation, (const NScheduler::TOperationIdOrAlias&, const TGetOperationOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, DumpJobContext, (NJobTrackerClient::TJobId, const NYPath::TYPath&, const TDumpJobContextOptions&));
    UNSUPPORTED_METHOD(TFuture<NConcurrency::IAsyncZeroCopyInputStreamPtr>, GetJobInput, (NJobTrackerClient::TJobId, const TGetJobInputOptions&));
    UNSUPPORTED_METHOD(TFuture<NYson::TYsonString>, GetJobInputPaths, (NJobTrackerClient::TJobId, const TGetJobInputPathsOptions&));
    UNSUPPORTED_METHOD(TFuture<NYson::TYsonString>, GetJobSpec, (NJobTrackerClient::TJobId, const TGetJobSpecOptions&));
    UNSUPPORTED_METHOD(TFuture<TSharedRef>, GetJobStderr, (const NScheduler::TOperationIdOrAlias&, NJobTrackerClient::TJobId, const TGetJobStderrOptions&));
    UNSUPPORTED_METHOD(TFuture<TSharedRef>, GetJobFailContext, (const NScheduler::TOperationIdOrAlias&, NJobTrackerClient::TJobId, const TGetJobFailContextOptions&));
    UNSUPPORTED_METHOD(TFuture<TListOperationsResult>, ListOperations, (const TListOperationsOptions&));
    UNSUPPORTED_METHOD(TFuture<TListJobsResult>, ListJobs, (const NScheduler::TOperationIdOrAlias&, const TListJobsOptions&));
    UNSUPPORTED_METHOD(TFuture<NYson::TYsonString>, GetJob, (const NScheduler::TOperationIdOrAlias&, NJobTrackerClient::TJobId, const TGetJobOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, AbandonJob, (NJobTrackerClient::TJobId, const TAbandonJobOptions&));
    UNSUPPORTED_METHOD(TFuture<TPollJobShellResponse>, PollJobShell, (NJobTrackerClient::TJobId, const std::optional<TString>&, const NYson::TYsonString&, const TPollJobShellOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, AbortJob, (NJobTrackerClient::TJobId, const TAbortJobOptions&));
    UNSUPPORTED_METHOD(TFuture<TClusterMeta>, GetClusterMeta, (const TGetClusterMetaOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, CheckClusterLiveness, (const TCheckClusterLivenessOptions&));
    UNSUPPORTED_METHOD(TFuture<int>, BuildSnapshot, (const TBuildSnapshotOptions&));
    UNSUPPORTED_METHOD(TFuture<TCellIdToSnapshotIdMap>, BuildMasterSnapshots, (const TBuildMasterSnapshotsOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, SwitchLeader, (NObjectClient::TCellId, const TString&, const TSwitchLeaderOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, GCCollect, (const TGCCollectOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, KillProcess, (const TString&, const TKillProcessOptions&));
    UNSUPPORTED_METHOD(TFuture<TString>, WriteCoreDump, (const TString&, const TWriteCoreDumpOptions&));
    UNSUPPORTED_METHOD(TFuture<TGuid>, WriteLogBarrier, (const TString&, const TWriteLogBarrierOptions&));
    UNSUPPORTED_METHOD(TFuture<TString>, WriteOperationControllerCoreDump, (NJobTrackerClient::TOperationId, const TWriteOperationControllerCoreDumpOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, HealExecNode, (const TString&, const THealExecNodeOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, SuspendCoordinator, (NObjectClient::TCellId, const TSuspendCoordinatorOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, ResumeCoordinator, (NObjectClient::TCellId, const TResumeCoordinatorOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, SuspendTabletCells, (const std::vector<NObjectClient::TCellId>&, const TSuspendTabletCellsOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, ResumeTabletCells, (const std::vector<NObjectClient::TCellId>&, const TResumeTabletCellsOptions&));
    UNSUPPORTED_METHOD(TFuture<NChaosClient::TReplicationCardPtr>, GetReplicationCard, (NChaosClient::TReplicationCardId, const TGetReplicationCardOptions&));
    UNSUPPORTED_METHOD(TFuture<void>, UpdateChaosTableReplicaProgress, (NChaosClient::TReplicaId, const TUpdateChaosTableReplicaProgressOptions&));
    UNSUPPORTED_METHOD(TFuture<TPullRowsResult>, PullRows, (const NYPath::TYPath&, const TPullRowsOptions&));

private:
    THedgingExecutorPtr Executor_;
};

} // namespace

////////////////////////////////////////////////////////////////////////////////

NApi::IClientPtr CreateHedgingClient(const THedgingClientOptions& options)
{
    return New<THedgingClient>(options, CreateDummyPenaltyProvider());
}

NApi::IClientPtr CreateHedgingClient(const THedgingClientOptions& options,
                                          const IPenaltyProviderPtr& penaltyProvider)
{
    return New<THedgingClient>(options, penaltyProvider);
}

NApi::IClientPtr CreateHedgingClient(const THedgingClientConfig& config)
{
    return CreateHedgingClient(GetHedgingClientOptions(config));
}

NApi::IClientPtr CreateHedgingClient(const THedgingClientConfig& config, const IClientsCachePtr& clientsCache)
{
    return CreateHedgingClient(GetHedgingClientOptions(config, clientsCache));
}

NApi::IClientPtr CreateHedgingClient(const THedgingClientConfig& config,
                                          const IClientsCachePtr& clientsCache,
                                          const IPenaltyProviderPtr& penaltyProvider)
{
    return CreateHedgingClient(GetHedgingClientOptions(config, clientsCache), penaltyProvider);
}

THedgingClientOptions GetHedgingClientOptions(const THedgingClientConfig& config, TClientBuilder clientBuilder)
{
    THedgingClientOptions options;
    options.BanPenalty = TDuration::MilliSeconds(config.GetBanPenalty());
    options.BanDuration = TDuration::MilliSeconds(config.GetBanDuration());

    NProfiling::TTagSet counterTagSet;

    for (const auto& [tagName, tagValue] : config.GetTags()) {
        counterTagSet.AddTag(NProfiling::TTag(tagName, tagValue));
    }

    options.Clients.reserve(config.GetClients().size());
    for (const auto& client : config.GetClients()) {
        options.Clients.emplace_back(
            clientBuilder(client.GetClientConfig()),
            client.GetClientConfig().GetClusterName(),
            TDuration::MilliSeconds(client.GetInitialPenalty()),
            New<TCounter>(counterTagSet.WithTag(NProfiling::TTag("yt_cluster", client.GetClientConfig().GetClusterName()))));
    }
    return options;
}

THedgingClientOptions GetHedgingClientOptions(const THedgingClientConfig& config)
{
    return GetHedgingClientOptions(config, [] (const auto& clientConfig) {
        return CreateClient(clientConfig);
    });
}

THedgingClientOptions GetHedgingClientOptions(const THedgingClientConfig& config, const IClientsCachePtr& clientsCache) 
{
    return GetHedgingClientOptions(config, [clientsCache] (const auto& clientConfig) {
        return clientsCache->GetClient(clientConfig.GetClusterName());
    });
}

} // namespace NYT::NClient::NHedging::NRpc
