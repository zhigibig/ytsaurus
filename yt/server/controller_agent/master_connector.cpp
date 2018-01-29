#include "master_connector.h"
#include "operation_controller.h"
#include "snapshot_downloader.h"
#include "snapshot_builder.h"
#include "controller_agent.h"
#include "operation.h"
#include "serialize.h"

#include <yt/server/scheduler/config.h>
#include <yt/server/scheduler/scheduler.h>
#include <yt/server/scheduler/helpers.h>

#include <yt/server/cell_scheduler/bootstrap.h>
#include <yt/server/cell_scheduler/config.h>

#include <yt/server/misc/update_executor.h>

#include <yt/ytlib/api/native_connection.h>
#include <yt/ytlib/api/native_client.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/hive/cluster_directory.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/table_client/table_ypath_proxy.h>

#include <yt/ytlib/file_client/file_ypath_proxy.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/actions/cancelable_context.h>

namespace NYT {
namespace NControllerAgent {

using namespace NApi;
using namespace NRpc;
using namespace NYTree;
using namespace NYson;
using namespace NConcurrency;
using namespace NHiveClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NTableClient;
using namespace NFileClient;
using namespace NSecurityClient;
using namespace NTransactionClient;
using namespace NScheduler;
using namespace NCellScheduler;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = MasterConnectorLogger;

////////////////////////////////////////////////////////////////////////////////

class TMasterConnector::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TControllerAgentConfigPtr config,
        NCellScheduler::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
    { }

    void OnMasterConnecting(const TIncarnationId& incarnationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // NB: We cannot be sure the previous incarnation did a proper cleanup due to possible
        // fiber cancelation.
        DoCleanup();

        IncarnationId_ = incarnationId;

        YCHECK(!CancelableContext_);
        CancelableContext_ = New<TCancelableContext>();

        YCHECK(!CancelableControlInvoker_);
        CancelableControlInvoker_ = CancelableContext_->CreateInvoker(Bootstrap_->GetControlInvoker(EControlQueue::MasterConnector));

        MasterConnecting_.Fire();
    }

    void OnMasterConnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YCHECK(!Connected_);
        Connected_.store(true);
        ConnectionTime_ = TInstant::Now();

        YCHECK(!OperationNodesUpdateExecutor_);
        OperationNodesUpdateExecutor_ = New<TUpdateExecutor<TOperationId, TOperationNodeUpdate>>(
            CancelableControlInvoker_,
            BIND(&TImpl::UpdateOperationNode, Unretained(this)),
            BIND(&TImpl::IsOperationInFinishedState, Unretained(this)),
            BIND(&TImpl::OnOperationUpdateFailed, Unretained(this)),
            Config_->OperationsUpdatePeriod,
            Logger);
        OperationNodesUpdateExecutor_->Start();

        YCHECK(!TransactionRefreshExecutor_);
        TransactionRefreshExecutor_ = New<TPeriodicExecutor>(
            CancelableControlInvoker_,
            BIND(&TImpl::RefreshTransactions, MakeStrong(this)),
            Config_->TransactionsRefreshPeriod,
            EPeriodicExecutorMode::Automatic);
        TransactionRefreshExecutor_->Start();

        YCHECK(!SnapshotExecutor_);
        SnapshotExecutor_= New<TPeriodicExecutor>(
            CancelableControlInvoker_,
            BIND(&TImpl::BuildSnapshot, MakeStrong(this)),
            Config_->SnapshotPeriod,
            EPeriodicExecutorMode::Automatic);
        SnapshotExecutor_->Start();

        YCHECK(!UnstageExecutor_);
        UnstageExecutor_ = New<TPeriodicExecutor>(
            CancelableControlInvoker_,
            BIND(&TImpl::UnstageChunkTrees, MakeWeak(this)),
            Config_->ChunkUnstagePeriod,
            EPeriodicExecutorMode::Automatic);
        UnstageExecutor_->Start();

        MasterConnected_.Fire();
    }

    void OnMasterDisconnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        DoCleanup();

        MasterDisconnected_.Fire();
    }

    bool IsConnected() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Connected_.load();
    }

    TInstant GetConnectionTime() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return ConnectionTime_.load();
    }

    const TIncarnationId& GetIncarnationId() const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        // XXX(babenko)
        // YCHECK(Connected_);

        return IncarnationId_;
    }

    void StartOperationNodeUpdates(
        const TOperationId& operationId,
        NScheduler::EOperationCypressStorageMode storageMode)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected_);

        OperationNodesUpdateExecutor_->AddUpdate(
            operationId,
            TOperationNodeUpdate(operationId, storageMode));
    }

    void CreateJobNode(const TOperationId& operationId, const TCreateJobNodeRequest& request)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected_);

        CancelableControlInvoker_->Invoke(BIND(
            &TImpl::DoCreateJobNode,
            MakeStrong(this),
            operationId,
            request));
    }

    TFuture<void> FlushOperationNode(const TOperationId& operationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected_);

        return BIND(&TImpl::DoFlushOperationNode, MakeStrong(this))
            .AsyncVia(CancelableControlInvoker_)
            .Run(operationId);
    }

    TFuture<void> AttachToLivePreview(
        const TOperationId& operationId,
        const TTransactionId& transactionId,
        const std::vector<TNodeId>& tableIds,
        const std::vector<TChunkTreeId>& childIds)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected_);

        return BIND(&TImpl::DoAttachToLivePreview, MakeStrong(this))
            .AsyncVia(CancelableControlInvoker_)
            .Run(operationId, transactionId, tableIds, childIds);
    }

    TFuture<TOperationSnapshot> DownloadSnapshot(const TOperationId& operationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected_);

        if (!Config_->EnableSnapshotLoading) {
            return MakeFuture<TOperationSnapshot>(TError("Snapshot loading is disabled in configuration"));
        }

        return BIND(&TImpl::DoDownloadSnapshot, MakeStrong(this))
            .AsyncVia(CancelableControlInvoker_)
            .Run(operationId);
    }

    TFuture<void> RemoveSnapshot(const TOperationId& operationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected_);

        return BIND(&TImpl::DoRemoveSnapshot, MakeStrong(this))
            .AsyncVia(CancelableControlInvoker_)
            .Run(operationId);
    }

    void AddChunkTreesToUnstageList(std::vector<TChunkTreeId> chunkTreeIds, bool recursive)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        YCHECK(Connected_);

        CancelableControlInvoker_->Invoke(BIND(&TImpl::DoAddChunkTreesToUnstageList,
            MakeWeak(this),
            Passed(std::move(chunkTreeIds)),
            recursive));
    }

    void UpdateConfig(const TControllerAgentConfigPtr& config)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        Config_ = config;

        if (OperationNodesUpdateExecutor_) {
            OperationNodesUpdateExecutor_->SetPeriod(Config_->OperationsUpdatePeriod);
        }
        if (TransactionRefreshExecutor_) {
            TransactionRefreshExecutor_->SetPeriod(Config_->TransactionsRefreshPeriod);
        }
        if (SnapshotExecutor_) {
            SnapshotExecutor_->SetPeriod(Config_->SnapshotPeriod);
        }
        if (UnstageExecutor_) {
            UnstageExecutor_->SetPeriod(Config_->ChunkUnstagePeriod);
        }
    }

    DEFINE_SIGNAL(void(), MasterConnecting);
    DEFINE_SIGNAL(void(), MasterConnected);
    DEFINE_SIGNAL(void(), MasterDisconnected);

private:
    TControllerAgentConfigPtr Config_;
    NCellScheduler::TBootstrap* const Bootstrap_;

    std::atomic<bool> Connected_ = {false};
    std::atomic<TInstant> ConnectionTime_;

    TIncarnationId IncarnationId_;

    TCancelableContextPtr CancelableContext_;
    IInvokerPtr CancelableControlInvoker_;

    struct TLivePreviewRequest
    {
        TChunkListId TableId;
        TChunkTreeId ChildId;
    };

    struct TOperationNodeUpdate
    {
        TOperationNodeUpdate(const TOperationId& operationId, EOperationCypressStorageMode storageMode)
            : OperationId(operationId)
            , StorageMode(storageMode)
        { }

        TOperationId OperationId;
        EOperationCypressStorageMode StorageMode;

        std::vector<TCreateJobNodeRequest> JobRequests;

        TTransactionId LivePreviewTransactionId;
        std::vector<TLivePreviewRequest> LivePreviewRequests;
    };

    TIntrusivePtr<TUpdateExecutor<TOperationId, TOperationNodeUpdate>> OperationNodesUpdateExecutor_;

    TPeriodicExecutorPtr TransactionRefreshExecutor_;
    TPeriodicExecutorPtr SnapshotExecutor_;
    TPeriodicExecutorPtr UnstageExecutor_;

    struct TUnstageRequest
    {
        TChunkTreeId ChunkTreeId;
        bool Recursive;
    };

    THashMap<TCellTag, std::vector<TUnstageRequest>> CellTagToUnstageList_;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);


    void DoCleanup()
    {
        Connected_.store(false);
        IncarnationId_ = {};

        if (CancelableContext_) {
            CancelableContext_->Cancel();
            CancelableContext_.Reset();
        }

        CancelableControlInvoker_.Reset();

        if (OperationNodesUpdateExecutor_) {
            OperationNodesUpdateExecutor_->Stop();
            OperationNodesUpdateExecutor_.Reset();
        }

        if (TransactionRefreshExecutor_) {
            TransactionRefreshExecutor_->Stop();
            TransactionRefreshExecutor_.Reset();
        }

        if (SnapshotExecutor_) {
            SnapshotExecutor_->Stop();
            SnapshotExecutor_.Reset();
        }

        if (UnstageExecutor_) {
            UnstageExecutor_->Stop();
            UnstageExecutor_.Reset();
        }
    }

    TObjectServiceProxy::TReqExecuteBatchPtr StartObjectBatchRequest(
        EMasterChannelKind channelKind = EMasterChannelKind::Leader,
        TCellTag cellTag = PrimaryMasterCellTag)
    {
        TObjectServiceProxy proxy(Bootstrap_
            ->GetMasterClient()
            ->GetMasterChannelOrThrow(channelKind, cellTag));
        return proxy.ExecuteBatch();
    }

    TChunkServiceProxy::TReqExecuteBatchPtr StartChunkBatchRequest(TCellTag cellTag = PrimaryMasterCellTag)
    {
        TChunkServiceProxy proxy(Bootstrap_
            ->GetMasterClient()
            ->GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellTag));
        return proxy.ExecuteBatch();
    }

    void RefreshTransactions()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        // Take a snapshot of all known operations.
        const auto& controllerAgent = Bootstrap_->GetControllerAgent();

        // Collect all transactions that are used by currently running operations.
        THashSet<TTransactionId> watchSet;
        for (const auto& pair : controllerAgent->GetOperations()) {
            const auto& operation = pair.second;
            for (const auto& transaction : operation->GetTransactions()) {
                watchSet.insert(transaction->GetId());
            }
        }

        THashMap<TCellTag, TObjectServiceProxy::TReqExecuteBatchPtr> batchReqs;

        for (const auto& id : watchSet) {
            auto cellTag = CellTagFromId(id);
            if (batchReqs.find(cellTag) == batchReqs.end()) {
                auto connection = Bootstrap_->FindRemoteConnection(cellTag);
                if (!connection) {
                    continue;
                }
                auto channel = connection->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
                auto authenticatedChannel = CreateAuthenticatedChannel(channel, SchedulerUserName);
                TObjectServiceProxy proxy(authenticatedChannel);
                batchReqs[cellTag] = proxy.ExecuteBatch();
            }

            auto checkReq = TObjectYPathProxy::GetBasicAttributes(FromObjectId(id));
            batchReqs[cellTag]->AddRequest(checkReq, "check_tx_" + ToString(id));
        }

        LOG_INFO("Refreshing transactions");

        THashMap<TCellTag, NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr> batchRsps;

        for (const auto& pair : batchReqs) {
            auto cellTag = pair.first;
            const auto& batchReq = pair.second;
            auto batchRspOrError = WaitFor(batchReq->Invoke());
            if (batchRspOrError.IsOK()) {
                batchRsps[cellTag] = batchRspOrError.Value();
            } else {
                LOG_ERROR(batchRspOrError, "Error refreshing transactions (CellTag: %v)",
                    cellTag);
            }
        }

        THashSet<TTransactionId> deadTransactionIds;

        for (const auto& id : watchSet) {
            auto cellTag = CellTagFromId(id);
            auto it = batchRsps.find(cellTag);
            if (it != batchRsps.end()) {
                const auto& batchRsp = it->second;
                auto rspOrError = batchRsp->GetResponse("check_tx_" + ToString(id));
                if (!rspOrError.IsOK()) {
                    LOG_DEBUG(rspOrError, "Found dead transaction (TransactionId: %v)", id);
                    deadTransactionIds.insert(id);
                }
            }
        }

        LOG_INFO("Transactions refreshed");

        // Check every transaction of every operation and raise appropriate notifications.
        for (const auto& pair : controllerAgent->GetOperations()) {
            const auto& operation = pair.second;
            auto controller = operation->GetController();
            for (const auto& transaction : operation->GetTransactions()) {
                if (deadTransactionIds.find(transaction->GetId()) != deadTransactionIds.end()) {
                    controller->GetCancelableInvoker()->Invoke(BIND(
                        &IOperationController::OnTransactionAborted,
                        controller,
                        transaction->GetId()));
                    break;
                }
            }
        }
    }

    void DoUpdateOperationNode(
        const TOperationId& operationId,
        EOperationCypressStorageMode storageMode,
        const TTransactionId& transactionId,
        const std::vector<TCreateJobNodeRequest>& jobRequests,
        const std::vector<TLivePreviewRequest>& livePreviewRequests)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        std::vector<bool> failedRequestSet(jobRequests.size(), false);
        try {
            CreateJobNodes(operationId, storageMode, jobRequests, &failedRequestSet);
        } catch (const std::exception& ex) {
            auto error = TError("Error creating job nodes for operation %v",
                operationId)
                << ex;
            if (error.FindMatching(NSecurityClient::EErrorCode::AccountLimitExceeded)) {
                LOG_DEBUG(error);
                return;
            } else {
                THROW_ERROR error;
            }
        }

        try {
            std::vector<TJobFile> files;

            for (size_t index = 0; index < jobRequests.size(); ++index) {
                if (failedRequestSet[index]) {
                    continue;
                }

                const auto& request = jobRequests[index];
                if (request.StderrChunkId) {
                    auto paths = GetCompatibilityJobPaths(operationId, request.JobId, storageMode, "stderr");
                    for (const auto& path : paths) {
                        files.push_back({
                            request.JobId,
                            path,
                            request.StderrChunkId,
                            "stderr"
                        });
                    }
                }
                if (request.FailContextChunkId) {
                    auto paths = GetCompatibilityJobPaths(operationId, request.JobId, storageMode, "fail_context");
                    for (const auto& path : paths) {
                        files.push_back({
                            request.JobId,
                            path,
                            request.FailContextChunkId,
                            "fail_context"
                        });
                    }
                }
            }
            SaveJobFiles(operationId, files);
        } catch (const std::exception& ex) {
            // NB: Don' treat this as a critical error.
            // Some of these chunks could go missing for a number of reasons.
            LOG_WARNING(ex, "Error saving job files (OperationId: %v)",
                operationId);
        }

        try {
            AttachLivePreviewChunks(operationId, transactionId, livePreviewRequests);
        } catch (const std::exception& ex) {
            // NB: Don' treat this as a critical error.
            // Some of these chunks could go missing for a number of reasons.
            LOG_WARNING(ex, "Error attaching live preview chunks (OperationId: %v)",
                operationId);
        }

        try {
            UpdateOperationNodeAttributes(operationId, storageMode);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error updating operation %v node",
                operationId)
                << ex;
        }

        LOG_DEBUG("Operation node updated (OperationId: %v)", operationId);
    }

    TCallback<TFuture<void>()> UpdateOperationNode(const TOperationId& operationId, TOperationNodeUpdate* update)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        const auto& controllerAgent = Bootstrap_->GetControllerAgent();
        auto operation = controllerAgent->FindOperation(operationId);
        if (!operation) {
            return {};
        }

        auto controller = operation->GetController();

        if (update->JobRequests.empty() &&
            update->LivePreviewRequests.empty() &&
            !controller->ShouldUpdateProgress())
        {
            return {};
        }

        return BIND(&TImpl::DoUpdateOperationNode,
            MakeStrong(this),
            operationId,
            update->StorageMode,
            update->LivePreviewTransactionId,
            Passed(std::move(update->JobRequests)),
            Passed(std::move(update->LivePreviewRequests)))
            .AsyncVia(CancelableControlInvoker_);
    }

    void UpdateOperationNodeAttributes(const TOperationId& operationId, EOperationCypressStorageMode storageMode)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        const auto& controllerAgent = Bootstrap_->GetControllerAgent();
        auto operation = controllerAgent->FindOperation(operationId);
        if (!operation) {
            return;
        }

        auto controller = operation->GetController();
        if (!controller->HasProgress()) {
            return;
        }

        controller->SetProgressUpdated();

        auto paths = GetCompatibilityOperationPaths(operationId, storageMode);

        auto batchReq = StartObjectBatchRequest();
        GenerateMutationId(batchReq);

        auto progress = controller->GetProgress();
        YCHECK(progress);

        auto briefProgress = controller->GetBriefProgress();
        YCHECK(briefProgress);

        for (const auto& operationPath : paths) {
            auto multisetReq = TYPathProxy::Multiset(operationPath + "/@");

            // Set progress.
            {
                auto req = multisetReq->add_subrequests();
                req->set_key("progress");
                req->set_value(progress.GetData());
            }

            // Set brief progress.
            {
                auto req = multisetReq->add_subrequests();
                req->set_key("brief_progress");
                req->set_value(briefProgress.GetData());
            }

            batchReq->AddRequest(multisetReq, "update_op_node");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
    }

    void CreateJobNodes(
        const TOperationId& operationId,
        EOperationCypressStorageMode storageMode,
        const std::vector<TCreateJobNodeRequest>& requests,
        std::vector<bool>* failedRequestSet)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (requests.empty()) {
            return;
        }

        auto batchReq = StartObjectBatchRequest();

        for (const auto& request : requests) {
            const auto& jobId = request.JobId;

            auto paths = GetCompatibilityJobPaths(operationId, jobId, storageMode);
            auto attributes = ConvertToAttributes(request.Attributes);

            for (const auto& path : paths) {
                auto req = TCypressYPathProxy::Create(path);
                GenerateMutationId(req);
                req->set_type(static_cast<int>(EObjectType::MapNode));
                req->set_force(true);
                ToProto(req->mutable_node_attributes(), *attributes);
                batchReq->AddRequest(req, "create_" + ToString(jobId));
            }
        }

        auto batchRsp = WaitFor(batchReq->Invoke())
            .ValueOrThrow();

        int failedRequestCount = 0;
        for (size_t index = 0; index < requests.size(); ++index) {
            const auto& request = requests[index];
            const auto& jobId = request.JobId;
            auto rsps = batchRsp->GetResponses<TCypressYPathProxy::TRspCreate>("create_" + ToString(jobId));
            for (const auto& rsp : rsps) {
                if (rsp.IsOK()) {
                    continue;
                }
                TError error = rsp;
                if (error.FindMatching(NSecurityClient::EErrorCode::AccountLimitExceeded)) {
                    LOG_ERROR(error, "Account limit exceeded while creating job node (JobId: %v)",
                        jobId);
                    (*failedRequestSet)[index] = true;
                    ++failedRequestCount;
                } else {
                    THROW_ERROR_EXCEPTION("Failed to create job node")
                        << TErrorAttribute("job_id", jobId)
                        << error;
                }
            }
        }

        LOG_INFO("Job nodes created (TotalCount: %v, FailedCount: %v, OperationId: %v)",
            requests.size(),
            failedRequestCount,
            operationId);
    }

    void AttachLivePreviewChunks(
        const TOperationId& operationId,
        const TTransactionId& transactionId,
        const std::vector<TLivePreviewRequest>& requests)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        struct TTableInfo
        {
            TNodeId TableId;
            TCellTag CellTag;
            std::vector<TChunkId> ChildIds;
            TTransactionId UploadTransactionId;
            TChunkListId UploadChunkListId;
            NChunkClient::NProto::TDataStatistics Statistics;
        };

        THashMap<TNodeId, TTableInfo> tableIdToInfo;
        for (const auto& request : requests) {
            auto& tableInfo = tableIdToInfo[request.TableId];
            tableInfo.TableId = request.TableId;
            tableInfo.ChildIds.push_back(request.ChildId);

            LOG_DEBUG("Appending live preview chunk trees (OperationId: %v, TableId: %v, ChildCount: %v)",
                operationId,
                tableInfo.TableId,
                tableInfo.ChildIds.size());
        }

        if (tableIdToInfo.empty()) {
            return;
        }

        // BeginUpload
        {
            auto batchReq = StartObjectBatchRequest();

            for (const auto& pair : tableIdToInfo) {
                const auto& tableId = pair.first;
                {
                    auto req = TTableYPathProxy::BeginUpload(FromObjectId(tableId));
                    req->set_update_mode(static_cast<int>(EUpdateMode::Append));
                    req->set_lock_mode(static_cast<int>(ELockMode::Shared));
                    req->set_upload_transaction_title(Format("Attaching live preview chunks of operation %v",
                        operationId));
                    SetTransactionId(req, transactionId);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req, "begin_upload");
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            auto rsps = batchRsp->GetResponses<TChunkOwnerYPathProxy::TRspBeginUpload>("begin_upload");
            int rspIndex = 0;
            for (auto& pair : tableIdToInfo) {
                auto& tableInfo = pair.second;
                const auto& rsp = rsps[rspIndex++].Value();
                tableInfo.CellTag = rsp->cell_tag();
                tableInfo.UploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());
            }
        }

        THashMap<TCellTag, std::vector<TTableInfo*>> cellTagToInfos;
        for (auto& pair : tableIdToInfo) {
            auto& tableInfo  = pair.second;
            cellTagToInfos[tableInfo.CellTag].push_back(&tableInfo);
        }

        // GetUploadParams
        for (auto& pair : cellTagToInfos) {
            auto cellTag = pair.first;
            auto& tableInfos = pair.second;

            auto batchReq = StartObjectBatchRequest(EMasterChannelKind::Follower, cellTag);
            for (const auto* tableInfo : tableInfos) {
                auto req = TTableYPathProxy::GetUploadParams(FromObjectId(tableInfo->TableId));
                SetTransactionId(req, tableInfo->UploadTransactionId);
                batchReq->AddRequest(req, "get_upload_params");
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            auto rsps = batchRsp->GetResponses<TTableYPathProxy::TRspGetUploadParams>("get_upload_params");
            int rspIndex = 0;
            for (auto* tableInfo : tableInfos) {
                const auto& rsp = rsps[rspIndex++].Value();
                tableInfo->UploadChunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());
            }
        }

        // Attach
        for (auto& pair : cellTagToInfos) {
            auto cellTag = pair.first;
            auto& tableInfos = pair.second;

            auto batchReq = StartChunkBatchRequest(cellTag);
            GenerateMutationId(batchReq);
            batchReq->set_suppress_upstream_sync(true);

            std::vector<int> tableIndexToRspIndex;
            for (const auto* tableInfo : tableInfos) {
                size_t beginIndex = 0;
                const auto& childIds = tableInfo->ChildIds;
                while (beginIndex < childIds.size()) {
                    auto lastIndex = std::min(beginIndex + Config_->MaxChildrenPerAttachRequest, childIds.size());
                    bool isFinal = (lastIndex == childIds.size());
                    if (isFinal) {
                        tableIndexToRspIndex.push_back(batchReq->attach_chunk_trees_subrequests_size());
                    }
                    auto* req = batchReq->add_attach_chunk_trees_subrequests();
                    ToProto(req->mutable_parent_id(), tableInfo->UploadChunkListId);
                    for (auto index = beginIndex; index < lastIndex; ++index) {
                        ToProto(req->add_child_ids(), childIds[index]);
                    }
                    req->set_request_statistics(isFinal);
                    beginIndex = lastIndex;
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
            const auto& batchRsp = batchRspOrError.Value();

            const auto& rsps = batchRsp->attach_chunk_trees_subresponses();
            for (int tableIndex = 0; tableIndex < tableInfos.size(); ++tableIndex) {
                auto* tableInfo = tableInfos[tableIndex];
                const auto& rsp = rsps.Get(tableIndexToRspIndex[tableIndex]);
                tableInfo->Statistics = rsp.statistics();
            }
        }

        // EndUpload
        {
            auto batchReq = StartObjectBatchRequest();

            for (const auto& pair : tableIdToInfo) {
                const auto& tableId = pair.first;
                const auto& tableInfo = pair.second;
                {
                    auto req = TTableYPathProxy::EndUpload(FromObjectId(tableId));
                    *req->mutable_statistics() = tableInfo.Statistics;
                    SetTransactionId(req, tableInfo.UploadTransactionId);
                    GenerateMutationId(req);
                    batchReq->AddRequest(req, "end_upload");
                }
            }

            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError));
        }
    }

    void DoAttachToLivePreview(
        const TOperationId& operationId,
        const TTransactionId& transactionId,
        const std::vector<TNodeId>& tableIds,
        const std::vector<TChunkTreeId>& childIds)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto* update = OperationNodesUpdateExecutor_->FindUpdate(operationId);
        if (!update) {
            LOG_DEBUG("Trying to attach live preview to an unknown operation (OperationId: %v)",
                operationId);
            return;
        }

        // NB: Controller must attach all live preview chunks under the same transaction.
        YCHECK(!update->LivePreviewTransactionId || update->LivePreviewTransactionId == transactionId);
        update->LivePreviewTransactionId = transactionId;

        LOG_TRACE("Attaching live preview chunk trees (OperationId: %v, TableIds: %v, ChildCount: %v)",
            operationId,
            tableIds,
            childIds.size());

        for (const auto& tableId : tableIds) {
            for (const auto& childId : childIds) {
                update->LivePreviewRequests.push_back(TLivePreviewRequest{tableId, childId});
            }
        }
    }

    TOperationSnapshot DoDownloadSnapshot(const TOperationId& operationId)
    {
        std::vector<NYTree::TYPath> paths = {
            NScheduler::GetNewSnapshotPath(operationId),
            NScheduler::GetSnapshotPath(operationId)
        };

        auto batchReq = StartObjectBatchRequest();
        for (const auto& path : paths) {
            auto req = TYPathProxy::Get(path + "/@version");
            batchReq->AddRequest(req, "get_version");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        const auto& batchRsp = batchRspOrError.ValueOrThrow();

        auto rsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_version");
        YCHECK(rsps.size() == paths.size());

        TNullable<int> version;
        NYTree::TYPath snapshotPath;

        for (int index = 0; index < paths.size(); ++index) {
            const auto& rsp = rsps[index];

            if (rsp.IsOK()) {
                const auto& versionRsp = rsp.Value();
                version = ConvertTo<int>(TYsonString(versionRsp->value()));
                snapshotPath = paths[index];
                break;
            } else {
                if (!rsp.FindMatching(NYTree::EErrorCode::ResolveError)) {
                    THROW_ERROR_EXCEPTION("Error getting snapshot version")
                        << rsp;
                }
            }
        }

        if (!version) {
            THROW_ERROR_EXCEPTION("Snapshot does not exist");
        }

        LOG_INFO("Snapshot found (OperationId: %v, Version: %v, Path: %v)",
            operationId,
            *version,
            snapshotPath);

        if (!ValidateSnapshotVersion(*version)) {
            THROW_ERROR_EXCEPTION("Snapshot version validation failed");
        }

        TOperationSnapshot snapshot;
        snapshot.Version = *version;
        try {
            auto downloader = New<TSnapshotDownloader>(
                Config_,
                Bootstrap_,
                operationId);
            snapshot.Data = downloader->Run(snapshotPath);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error downloading snapshot") << ex;
        }
        return snapshot;
    }

    void DoCreateJobNode(const TOperationId& operationId, const TCreateJobNodeRequest& request)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_DEBUG("Creating job node (OperationId: %v, JobId: %v, StderrChunkId: %v, FailContextChunkId: %v)",
            operationId,
            request.JobId,
            request.StderrChunkId,
            request.FailContextChunkId);

        auto* update = OperationNodesUpdateExecutor_->FindUpdate(operationId);
        if (!update) {
            LOG_DEBUG("Create a job node for an unknown operation is impossible (OperationId: %v, JobId: %v)",
                operationId,
                request.JobId);
            return;
        }

        update->JobRequests.emplace_back(request);
    }

    void DoFlushOperationNode(const TOperationId& operationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Flushing operation node (OperationId: %v)",
            operationId);

        WaitFor(OperationNodesUpdateExecutor_->ExecuteUpdate(operationId))
            .ThrowOnError();
    }

    void DoRemoveSnapshot(const TOperationId& operationId)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto batchReq = StartObjectBatchRequest();
        for (const auto& path : {NScheduler::GetSnapshotPath(operationId), NScheduler::GetNewSnapshotPath(operationId)}) {
            auto req = TYPathProxy::Remove(path);
            req->set_force(true);
            batchReq->AddRequest(req, "remove_snapshot");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        auto error = GetCumulativeError(batchRspOrError);
        if (!error.IsOK()) {
            Bootstrap_->GetScheduler()->Disconnect(TError("Failed to remove snapshot") << error);
        }
    }

    void SaveJobFiles(const TOperationId& operationId, const std::vector<TJobFile>& files)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        NScheduler::SaveJobFiles(Bootstrap_->GetMasterClient(), operationId, files);
    }

    void BuildSnapshot()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!Config_->EnableSnapshotBuilding) {
            return;
        }

        const auto& controllerAgent = Bootstrap_->GetControllerAgent();
        auto controllerMap = controllerAgent->GetOperations();

        auto builder = New<TSnapshotBuilder>(
            Config_,
            std::move(controllerMap),
            Bootstrap_->GetMasterClient(),
            Bootstrap_->GetControllerAgent()->GetSnapshotIOInvoker());

        // NB: Result is logged in the builder.
        auto error = WaitFor(builder->Run());
        if (error.IsOK()) {
            LOG_INFO("Snapshot builder finished");
        } else {
            LOG_ERROR(error, "Error building snapshots");
        }
    }

    bool IsOperationInFinishedState(const TOperationNodeUpdate* update) const
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return !Bootstrap_->GetControllerAgent()->FindOperation(update->OperationId);
    }

    void OnOperationUpdateFailed(const TError& error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_ERROR(error, "Failed to update operation node");
    }

    void DoAddChunkTreesToUnstageList(std::vector<TChunkTreeId> chunkTreeIds, bool recursive)
    {
        for (const auto& chunkTreeId : chunkTreeIds) {
            auto cellTag = CellTagFromId(chunkTreeId);
            CellTagToUnstageList_[cellTag].emplace_back(TUnstageRequest{chunkTreeId, recursive});
        }
    }

    void UnstageChunkTrees()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        for (auto& pair : CellTagToUnstageList_) {
            const auto& cellTag = pair.first;
            auto& unstageRequests = pair.second;

            if (unstageRequests.empty()) {
                continue;
            }

            TChunkServiceProxy proxy(Bootstrap_
                ->GetMasterClient()
                ->GetMasterChannelOrThrow(NApi::EMasterChannelKind::Leader, cellTag));

            auto batchReq = proxy.ExecuteBatch();
            while (!unstageRequests.empty() &&
                batchReq->unstage_chunk_tree_subrequests_size() < Config_->DesiredChunkListsPerRelease)
            {
                auto unstageRequest = unstageRequests.back();
                unstageRequests.pop_back();
                auto req = batchReq->add_unstage_chunk_tree_subrequests();
                ToProto(req->mutable_chunk_tree_id(), unstageRequest.ChunkTreeId);
                req->set_recursive(unstageRequest.Recursive);
            }

            LOG_DEBUG("Unstaging chunk trees (ChunkTreeCount: %v, CellTag: %v)",
                batchReq->unstage_chunk_tree_subrequests_size(),
                cellTag);

            batchReq->Invoke().Apply(
                BIND([=] (const TChunkServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError) {
                    if (!batchRspOrError.IsOK()) {
                        LOG_DEBUG(batchRspOrError, "Error unstaging chunk trees (CellTag: %v)", cellTag);
                    }
                }));
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TMasterConnector::TMasterConnector(
    TControllerAgentConfigPtr config,
    NCellScheduler::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(std::move(config), bootstrap))
{ }

TMasterConnector::~TMasterConnector() = default;

void TMasterConnector::OnMasterConnecting(const TIncarnationId& incarnationId)
{
    Impl_->OnMasterConnecting(incarnationId);
}

void TMasterConnector::OnMasterConnected()
{
    Impl_->OnMasterConnected();
}

void TMasterConnector::OnMasterDisconnected()
{
    Impl_->OnMasterDisconnected();
}

bool TMasterConnector::IsConnected() const
{
    return Impl_->IsConnected();
}

TInstant TMasterConnector::GetConnectionTime() const
{
    return Impl_->GetConnectionTime();
}

const TIncarnationId& TMasterConnector::GetIncarnationId() const
{
    return Impl_->GetIncarnationId();
}

void TMasterConnector::StartOperationNodeUpdates(
    const TOperationId& operationId,
    NScheduler::EOperationCypressStorageMode storageMode)
{
    Impl_->StartOperationNodeUpdates(operationId, storageMode);
}

void TMasterConnector::CreateJobNode(const TOperationId& operationId, const TCreateJobNodeRequest& request)
{
    Impl_->CreateJobNode(operationId, request);
}

TFuture<void> TMasterConnector::FlushOperationNode(const TOperationId& operationId)
{
    return Impl_->FlushOperationNode(operationId);
}

TFuture<void> TMasterConnector::AttachToLivePreview(
    const TOperationId& operationId,
    const TTransactionId& transactionId,
    const std::vector<TNodeId>& tableIds,
    const std::vector<TChunkTreeId>& childIds)
{
    return Impl_->AttachToLivePreview(operationId, transactionId, tableIds, childIds);
}

TFuture<TOperationSnapshot> TMasterConnector::DownloadSnapshot(const TOperationId& operationId)
{
    return Impl_->DownloadSnapshot(operationId);
}

TFuture<void> TMasterConnector::RemoveSnapshot(const TOperationId& operationId)
{
    return Impl_->RemoveSnapshot(operationId);
}

void TMasterConnector::AddChunkTreesToUnstageList(std::vector<TChunkId> chunkTreeIds, bool recursive)
{
    Impl_->AddChunkTreesToUnstageList(std::move(chunkTreeIds), recursive);
}

void TMasterConnector::UpdateConfig(const TControllerAgentConfigPtr& config)
{
    Impl_->UpdateConfig(config);
}

DELEGATE_SIGNAL(TMasterConnector, void(), MasterConnecting, *Impl_);
DELEGATE_SIGNAL(TMasterConnector, void(), MasterConnected, *Impl_);
DELEGATE_SIGNAL(TMasterConnector, void(), MasterDisconnected, *Impl_);

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT


