#pragma once

#include "operation_controller.h"

#include <yt/yt/server/lib/scheduler/message_queue.h>

#include <yt/yt/ytlib/scheduler/job_resources.h>

#include <yt/yt/core/ytree/public.h>

namespace NYT::NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

struct TAgentToSchedulerOperationEvent
{
    TAgentToSchedulerOperationEvent(
        NScheduler::EAgentToSchedulerOperationEventType eventType,
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TError error = TError());

    static TAgentToSchedulerOperationEvent CreateCompletedEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch);
    static TAgentToSchedulerOperationEvent CreateSuspendedEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TError error);
    static TAgentToSchedulerOperationEvent CreateFailedEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TError error);
    static TAgentToSchedulerOperationEvent CreateAbortedEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TError error);
    static TAgentToSchedulerOperationEvent CreateBannedInTentativeTreeEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TString treeId,
        std::vector<TJobId> jobIds);
    static TAgentToSchedulerOperationEvent CreateHeavyControllerActionFinishedEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TError error,
        std::optional<TOperationControllerInitializeResult> maybeResult);
    static TAgentToSchedulerOperationEvent CreateHeavyControllerActionFinishedEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TError error,
        std::optional<TOperationControllerPrepareResult> maybeResult);
    static TAgentToSchedulerOperationEvent CreateHeavyControllerActionFinishedEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TError error,
        std::optional<TOperationControllerMaterializeResult> maybeResult);
    static TAgentToSchedulerOperationEvent CreateHeavyControllerActionFinishedEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TError error,
        std::optional<TOperationControllerReviveResult> maybeResult);
    static TAgentToSchedulerOperationEvent CreateHeavyControllerActionFinishedEvent(
        TOperationId operationId,
        NScheduler::TControllerEpoch controllerEpoch,
        TError error,
        std::optional<TOperationControllerCommitResult> maybeResult);

    NScheduler::EAgentToSchedulerOperationEventType EventType;
    TOperationId OperationId;
    NScheduler::TControllerEpoch ControllerEpoch;
    TError Error;
    TString TentativeTreeId;
    std::vector<TJobId> TentativeTreeJobIds;
    std::optional<TOperationControllerInitializeResult> InitializeResult;
    std::optional<TOperationControllerPrepareResult> PrepareResult;
    std::optional<TOperationControllerMaterializeResult> MaterializeResult;
    std::optional<TOperationControllerReviveResult> ReviveResult;
    std::optional<TOperationControllerCommitResult> CommitResult;
};

// TODO(eshcherbin): Add static CreateXXXEvent methods as in TAgentToSchedulerOperationEvent.
struct TAgentToSchedulerJobEvent
{
    NScheduler::EAgentToSchedulerJobEventType EventType;
    TJobId JobId;
    NScheduler::TControllerEpoch ControllerEpoch;
    TError Error;
    std::optional<EInterruptReason> InterruptReason;
    std::optional<NJobTrackerClient::TReleaseJobFlags> ReleaseFlags;
};

////////////////////////////////////////////////////////////////////////////////

class TOperationControllerHost
    : public IOperationControllerHost
{
public:
    TOperationControllerHost(
        TOperation* operation,
        IInvokerPtr cancelableControlInvoker,
        TIntrusivePtr<NScheduler::TMessageQueueOutbox<TAgentToSchedulerOperationEvent>> operationEventsOutbox,
        TIntrusivePtr<NScheduler::TMessageQueueOutbox<TAgentToSchedulerJobEvent>> jobEventsOutbox,
        TBootstrap* bootstrap);

    virtual void InterruptJob(TJobId jobId, EInterruptReason reason) override;
    virtual void AbortJob(TJobId jobId, const TError& error) override;
    virtual void FailJob(TJobId jobId) override;
    virtual void ReleaseJobs(const std::vector<NJobTrackerClient::TJobToRelease>& TJobToRelease) override;

    virtual std::optional<TString> RegisterJobForMonitoring(TOperationId operationId, TJobId jobId) override;
    virtual bool UnregisterJobForMonitoring(TOperationId operationId, TJobId jobId) override;

    virtual TFuture<TOperationSnapshot> DownloadSnapshot() override;
    virtual TFuture<void> RemoveSnapshot() override;

    virtual TFuture<void> FlushOperationNode() override;
    virtual TFuture<void> UpdateInitializedOperationNode() override;
    virtual void CreateJobNode(const TCreateJobNodeRequest& request) override;

    virtual TFuture<void> AttachChunkTreesToLivePreview(
        NTransactionClient::TTransactionId transactionId,
        NCypressClient::TNodeId tableId,
        const std::vector<NChunkClient::TChunkTreeId>& childIds) override;
    virtual void AddChunkTreesToUnstageList(
        const std::vector<NChunkClient::TChunkId>& chunkTreeIds,
        bool recursive) override;

    virtual const NApi::NNative::IClientPtr& GetClient() override;
    virtual const NNodeTrackerClient::TNodeDirectoryPtr& GetNodeDirectory() override;
    virtual const NChunkClient::TThrottlerManagerPtr& GetChunkLocationThrottlerManager() override;
    virtual const IInvokerPtr& GetControllerThreadPoolInvoker() override;
    virtual const IInvokerPtr& GetJobSpecBuildPoolInvoker() override;
    virtual const IInvokerPtr& GetConnectionInvoker() override;
    virtual const NEventLog::IEventLogWriterPtr& GetEventLogWriter() override;
    virtual const ICoreDumperPtr& GetCoreDumper() override;
    virtual const NConcurrency::TAsyncSemaphorePtr& GetCoreSemaphore() override;
    virtual const NConcurrency::IThroughputThrottlerPtr& GetJobSpecSliceThrottler() override;
    virtual const NJobAgent::TJobReporterPtr& GetJobReporter() override;
    virtual const NChunkClient::TMediumDirectoryPtr& GetMediumDirectory() override;
    virtual TMemoryTagQueue* GetMemoryTagQueue() override;

    virtual int GetOnlineExecNodeCount() override;
    virtual TRefCountedExecNodeDescriptorMapPtr GetExecNodeDescriptors(const NScheduler::TSchedulingTagFilter& filter, bool onlineOnly = false) override;

    virtual TInstant GetConnectionTime() override;
    virtual TIncarnationId GetIncarnationId() override;

    virtual void OnOperationCompleted() override;
    virtual void OnOperationAborted(const TError& error) override;
    virtual void OnOperationFailed(const TError& error) override;
    virtual void OnOperationSuspended(const TError& error) override;
    virtual void OnOperationBannedInTentativeTree(const TString& treeId, const std::vector<TJobId>& jobIds) override;

    virtual void ValidateOperationAccess(
        const TString& user,
        NYTree::EPermission permission) override;
    
    virtual TFuture<void> UpdateAccountResourceUsageLease(
        NSecurityClient::TAccountResourceUsageLeaseId leaseId,
        const NScheduler::TDiskQuota& diskQuota) override;

private:
    const TOperationId OperationId_;
    const IInvokerPtr CancelableControlInvoker_;
    const TIntrusivePtr<NScheduler::TMessageQueueOutbox<TAgentToSchedulerOperationEvent>> OperationEventsOutbox_;
    const TIntrusivePtr<NScheduler::TMessageQueueOutbox<TAgentToSchedulerJobEvent>> JobEventsOutbox_;
    TBootstrap* const Bootstrap_;
    const TIncarnationId IncarnationId_;
    const NScheduler::TControllerEpoch ControllerEpoch_;
};

DEFINE_REFCOUNTED_TYPE(TOperationControllerHost)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent

