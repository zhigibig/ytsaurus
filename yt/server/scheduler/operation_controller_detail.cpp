#include "stdafx.h"
#include "operation_controller_detail.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "chunk_pool.h"
#include "job_resources.h"

#include <ytlib/transaction_client/transaction.h>

#include <ytlib/chunk_client//chunk_list_ypath_proxy.h>

#include <ytlib/object_client/object_ypath_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/convert.h>

#include <ytlib/formats/format.h>

#include <ytlib/chunk_client/chunk_meta_extensions.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>
#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/scheduler/config.h>

#include <ytlib/table_client/key.h>

#include <ytlib/meta_state/rpc_helpers.h>

#include <cmath>

namespace NYT {
namespace NScheduler {

using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYPath;
using namespace NFormats;
using namespace NJobProxy;
using namespace NScheduler::NProto;

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TTask::TTask(TOperationControllerBase* controller)
    : Controller(controller)
    , CachedPendingJobCount(0)
    , CachedTotalNeededResources(ZeroNodeResources())
    , Logger(Controller->Logger)
{ }

int TOperationControllerBase::TTask::GetPendingJobCountDelta()
{
    int oldValue = CachedPendingJobCount;
    int newValue = GetPendingJobCount();
    CachedPendingJobCount = newValue;
    return newValue - oldValue;
}

TNodeResources TOperationControllerBase::TTask::GetTotalNeededResourcesDelta()
{
    auto oldValue = CachedTotalNeededResources;
    auto newValue = GetTotalNeededResources();
    CachedTotalNeededResources = newValue;
    newValue -= oldValue;
    return newValue;
}

TNodeResources TOperationControllerBase::TTask::GetTotalNeededResources() const
{
    i64 count = GetPendingJobCount();
    // NB: Don't call GetAvgNeededResources if there are no pending jobs.
    return count == 0 ? ZeroNodeResources() : GetAvgNeededResources() * count;
}

i64 TOperationControllerBase::TTask::GetLocality(const Stroka& address) const
{
    return ChunkPool->GetLocality(address);
}

bool TOperationControllerBase::TTask::IsStrictlyLocal() const
{
    return false;
}

int TOperationControllerBase::TTask::GetPriority() const
{
    return 0;
}

void TOperationControllerBase::TTask::AddStripe(TChunkStripePtr stripe)
{
    ChunkPool->Add(stripe);
    AddInputLocalityHint(stripe);
    AddPendingHint();
}

void TOperationControllerBase::TTask::AddStripes(const std::vector<TChunkStripePtr>& stripes)
{
    FOREACH (auto stripe, stripes) {
        AddStripe(stripe);
    }
}

TJobPtr TOperationControllerBase::TTask::ScheduleJob(ISchedulingContext* context)
{
    if (!Controller->HasEnoughChunkLists(GetChunkListCountPerJob())) {
        return NULL;
    }

    auto jip = New<TJobInProgress>(this, Controller->CurrentJobCount);
    ++Controller->CurrentJobCount;

    auto address = context->GetNode()->GetAddress();
    auto dataSizeThreshold = GetJobDataSizeThreshold();
    jip->PoolResult = ChunkPool->Extract(address, dataSizeThreshold);

    // Compute the actual utilization for this JIP and check it
    // against the the limits. This is the last chance to give up.
    auto neededResources = GetNeededResources(jip);
    auto node = context->GetNode();
    if (!node->HasEnoughResources(neededResources)) {
        ChunkPool->OnFailed(jip->PoolResult);
        return NULL;
    }

    LOG_DEBUG("Job chunks extracted (TotalCount: %d, LocalCount: %d, ExtractedDataSize: %" PRId64 ", DataSizeThreshold: %s)",
        jip->PoolResult->TotalChunkCount,
        jip->PoolResult->LocalChunkCount,
        jip->PoolResult->TotalDataSize,
        ~ToString(dataSizeThreshold));

    auto job = context->BeginStartJob(Controller->Operation);
    jip->Job = job;
    auto* jobSpec = jip->Job->GetSpec();
    BuildJobSpec(jip, jobSpec);
    *jobSpec->mutable_resource_utilization() = neededResources;
    context->EndStartJob(job);

    Controller->RegisterJobInProgress(jip);

    OnJobStarted(jip);

    return jip->Job;
}

bool TOperationControllerBase::TTask::IsPending() const
{
    return ChunkPool->IsPending();
}

bool TOperationControllerBase::TTask::IsCompleted() const
{
    return ChunkPool->IsCompleted();
}

const TProgressCounter& TOperationControllerBase::TTask::DataSizeCounter() const
{
    return ChunkPool->DataSizeCounter();
}

const TProgressCounter& TOperationControllerBase::TTask::ChunkCounter() const
{
    return ChunkPool->ChunkCounter();
}

void TOperationControllerBase::TTask::OnJobStarted(TJobInProgressPtr jip)
{
    UNUSED(jip);
}

void TOperationControllerBase::TTask::OnJobCompleted(TJobInProgressPtr jip)
{
    ChunkPool->OnCompleted(jip->PoolResult);
}

void TOperationControllerBase::TTask::ReleaseFailedJob(TJobInProgressPtr jip)
{
    ChunkPool->OnFailed(jip->PoolResult);

    Controller->ReleaseChunkLists(jip->ChunkListIds);

    FOREACH (const auto& stripe, jip->PoolResult->Stripes) {
        AddInputLocalityHint(stripe);
    }
    AddPendingHint();
}

void TOperationControllerBase::TTask::OnJobFailed(TJobInProgressPtr jip)
{
    ReleaseFailedJob(jip);
}

void TOperationControllerBase::TTask::OnJobAborted(TJobInProgressPtr jip)
{
    ReleaseFailedJob(jip);
}

void TOperationControllerBase::TTask::OnTaskCompleted()
{
    LOG_DEBUG("Task completed (Task: %s)", ~GetId());
}

void TOperationControllerBase::TTask::AddPendingHint()
{
    Controller->AddTaskPendingHint(this);
}

void TOperationControllerBase::TTask::AddInputLocalityHint(TChunkStripePtr stripe)
{
    Controller->AddTaskLocalityHint(this, stripe);
}

i64 TOperationControllerBase::TTask::GetJobDataSizeThresholdGeneric(int pendingJobCount, i64 pendingDataSize)
{
    return static_cast<i64>(std::ceil((double) pendingDataSize / pendingJobCount));
}

void TOperationControllerBase::TTask::AddSequentialInputSpec(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobInProgressPtr jip)
{
    auto* inputSpec = jobSpec->add_input_specs();
    FOREACH (const auto& stripe, jip->PoolResult->Stripes) {
        AddInputChunks(inputSpec, stripe);
    }
}

void TOperationControllerBase::TTask::AddParallelInputSpec(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobInProgressPtr jip)
{
    FOREACH (const auto& stripe, jip->PoolResult->Stripes) {
        auto* inputSpec = jobSpec->add_input_specs();
        AddInputChunks(inputSpec, stripe);
    }
}

void TOperationControllerBase::TTask::AddOutputSpecs(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobInProgressPtr jip)
{
    FOREACH (const auto& table, Controller->OutputTables) {
        auto* outputSpec = jobSpec->add_output_specs();
        outputSpec->set_channels(table.Channels.Data());
        auto chunkListId = Controller->ExtractChunkList();
        jip->ChunkListIds.push_back(chunkListId);
        *outputSpec->mutable_chunk_list_id() = chunkListId.ToProto();
    }
}

void TOperationControllerBase::TTask::AddIntermediateOutputSpec(
    NScheduler::NProto::TJobSpec* jobSpec,
    TJobInProgressPtr jip)
{
    auto* outputSpec = jobSpec->add_output_specs();
    outputSpec->set_channels("[]");
    auto chunkListId = Controller->ExtractChunkList();
    jip->ChunkListIds.push_back(chunkListId);
    *outputSpec->mutable_chunk_list_id() = chunkListId.ToProto();
}

void TOperationControllerBase::TTask::AddInputChunks(
    NScheduler::NProto::TTableInputSpec* inputSpec,
    TChunkStripePtr stripe)
{
    FOREACH (const auto& weightedChunk, stripe->Chunks) {
        auto* inputChunk = inputSpec->add_chunks();
        *inputChunk = *weightedChunk.InputChunk;
        inputChunk->set_uncompressed_data_size(weightedChunk.DataSizeOverride);
        inputChunk->set_row_count(weightedChunk.RowCountOverride);
    }
}

TNodeResources TOperationControllerBase::TTask::GetAvgNeededResources() const
{
    return GetMinNeededResources();
}

TNodeResources TOperationControllerBase::TTask::GetNeededResources(TJobInProgressPtr jip) const
{
    UNUSED(jip);
    return GetMinNeededResources();
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TOperationControllerBase(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
    : Config(config)
    , Host(host)
    , Operation(operation)
    , ObjectProxy(host->GetMasterChannel())
    , Logger(OperationLogger)
    , CancelableContext(New<TCancelableContext>())
    , CancelableControlInvoker(CancelableContext->CreateInvoker(Host->GetControlInvoker()))
    , CancelableBackgroundInvoker(CancelableContext->CreateInvoker(Host->GetBackgroundInvoker()))
    , Active(false)
    , Running(false)
    , RunningJobCount(0)
    , CompletedJobCount(0)
    , FailedJobCount(0)
    , AbortedJobCount(0)
    , CurrentJobCount(0)
    , UsedResources(ZeroNodeResources())
    , PendingTaskInfos(MaxTaskPriority + 1)
    , CachedPendingJobCount(0)
    , CachedNeededResources(ZeroNodeResources())
{
    Logger.AddTag(Sprintf("OperationId: %s", ~operation->GetOperationId().ToString()));
}

void TOperationControllerBase::Initialize()
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    
    LOG_INFO("Initializing operation");

    FOREACH (const auto& path, GetInputTablePaths()) {
        TInputTable table;
        table.Path = path;
        InputTables.push_back(table);
    }

    FOREACH (const auto& path, GetOutputTablePaths()) {
        TOutputTable table;
        table.Path = path;
        if (path.Attributes().Get<bool>("overwrite", false)) {
            table.Clear = true;
            table.Overwrite = true;
            table.LockMode = ELockMode::Exclusive;
        }
        OutputTables.push_back(table);
    }

    FOREACH (const auto& path, GetFilePaths()) {
        TUserFile file;
        file.Path = path;
        Files.push_back(file);
    }

    try {
        DoInitialize();
    } catch (const std::exception& ex) {
        LOG_INFO(ex, "Operation has failed to initialize");
        Active = false;
        throw;
    }

    Active = true;

    LOG_INFO("Operation initialized");
}

void TOperationControllerBase::DoInitialize()
{ }

TFuture<void> TOperationControllerBase::Prepare()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto this_ = MakeStrong(this);
    auto pipeline = StartAsyncPipeline(CancelableBackgroundInvoker)
        ->Add(BIND(&TThis::StartIOTransactions, MakeStrong(this)))
        ->Add(BIND(&TThis::OnIOTransactionsStarted, MakeStrong(this)), CancelableControlInvoker)
        ->Add(BIND(&TThis::GetObjectIds, MakeStrong(this)))
        ->Add(BIND(&TThis::OnObjectIdsReceived, MakeStrong(this)))
        ->Add(BIND(&TThis::RequestInputs, MakeStrong(this)))
        ->Add(BIND(&TThis::OnInputsReceived, MakeStrong(this)))
        ->Add(BIND(&TThis::CompletePreparation, MakeStrong(this)));
     pipeline = CustomizePreparationPipeline(pipeline);
     return pipeline
        ->Add(BIND(&TThis::OnPreparationCompleted, MakeStrong(this)))
        ->Run()
        .Apply(BIND([=] (TValueOrError<void> result) -> TFuture<void> {
            if (result.IsOK()) {
                if (this_->Active) {
                    this_->Running = true;
                }
                return MakeFuture();
            } else {
                LOG_WARNING(result, "Operation has failed to prepare");
                this_->Active = false;
                this_->Host->OnOperationFailed(this_->Operation, result);
                // This promise is never fulfilled.
                return NewPromise<void>();
            }
        }));
}

TFuture<void> TOperationControllerBase::Revive()
{
    try {
        Initialize();
    } catch (const std::exception& ex) {
        OnOperationFailed(TError("Operation has failed to initialize")
            << ex);
        // This promise is never fulfilled.
        return NewPromise<void>();
    }
    return Prepare();
}

TFuture<void> TOperationControllerBase::Commit()
{
    VERIFY_THREAD_AFFINITY_ANY();

    YCHECK(Active);

    LOG_INFO("Committing operation");

    auto this_ = MakeStrong(this);
    return StartAsyncPipeline(CancelableBackgroundInvoker)
        ->Add(BIND(&TThis::CommitOutputs, MakeStrong(this)))
        ->Add(BIND(&TThis::OnOutputsCommitted, MakeStrong(this)))
        ->Run()
        .Apply(BIND([=] (TValueOrError<void> result) -> TFuture<void> {
            Active = false;
            if (result.IsOK()) {
                LOG_INFO("Operation committed");
                return MakeFuture();
            } else {
                LOG_WARNING(result, "Operation has failed to commit");
                this_->Host->OnOperationFailed(this_->Operation, result);
                return NewPromise<void>();
            }
        }));
}

void TOperationControllerBase::OnJobStarted(TJobPtr job)
{
    UsedResources += job->ResourceUtilization();
}

void TOperationControllerBase::OnJobRunning(TJobPtr job, const NProto::TJobStatus& status)
{
    UsedResources -= job->ResourceUtilization();
    job->ResourceUtilization() = status.resource_utilization();
    UsedResources += job->ResourceUtilization();
}

void TOperationControllerBase::OnJobCompleted(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    --RunningJobCount;
    ++CompletedJobCount;

    UsedResources -= job->ResourceUtilization();

    auto jip = GetJobInProgress(job);
    jip->Task->OnJobCompleted(jip);
    
    RemoveJobInProgress(job);

    LogProgress();

    if (jip->Task->IsCompleted()) {
        jip->Task->OnTaskCompleted();
    }

    if (RunningJobCount == 0 && GetPendingJobCount() == 0) {
        OnOperationCompleted();
    }
}

void TOperationControllerBase::OnJobFailed(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    --RunningJobCount;
    ++FailedJobCount;

    UsedResources -= job->ResourceUtilization();

    auto jip = GetJobInProgress(job);
    jip->Task->OnJobFailed(jip);

    RemoveJobInProgress(job);

    LogProgress();

    if (FailedJobCount >= Config->FailedJobsLimit) {
        OnOperationFailed(TError("Failed jobs limit %d has been reached",
            Config->FailedJobsLimit));
    }

    FOREACH (const auto& chunkId, job->Result().failed_chunk_ids()) {
        OnChunkFailed(TChunkId::FromProto(chunkId));
    }
}

void TOperationControllerBase::OnJobAborted(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    --RunningJobCount;
    ++AbortedJobCount;

    UsedResources -= job->ResourceUtilization();

    auto jip = GetJobInProgress(job);
    jip->Task->OnJobAborted(jip);

    RemoveJobInProgress(job);

    LogProgress();
}

void TOperationControllerBase::OnChunkFailed(const TChunkId& chunkId)
{
    if (InputChunkIds.find(chunkId) == InputChunkIds.end()) {
        LOG_WARNING("Intermediate chunk %s has failed", ~chunkId.ToString());
        OnIntermediateChunkFailed(chunkId);
    } else {
        LOG_WARNING("Input chunk %s has failed", ~chunkId.ToString());
        OnInputChunkFailed(chunkId);
    }
}

void TOperationControllerBase::OnInputChunkFailed(const TChunkId& chunkId)
{
    OnOperationFailed(TError("Unable to read input chunk %s", ~chunkId.ToString()));
}

void TOperationControllerBase::OnIntermediateChunkFailed(const TChunkId& chunkId)
{
    OnOperationFailed(TError("Unable to read intermediate chunk %s", ~chunkId.ToString()));
}

void TOperationControllerBase::Abort()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Aborting operation");

    Running = false;
    Active = false;
    CancelableContext->Cancel();

    AbortTransactions();

    LOG_INFO("Operation aborted");
}


void TOperationControllerBase::OnNodeOffline(TExecNodePtr node)
{
    LOG_INFO("Node has gone offline");
}

TJobPtr TOperationControllerBase::ScheduleJob(
    ISchedulingContext* context,
    bool isStarving)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
 
    if (!Running) {
        LOG_TRACE("Operation is not running, scheduling request ignored");
        return NULL;
    }

    if (GetPendingJobCount() == 0) {
        LOG_TRACE("No pending jobs left, scheduling request ignored");
        return NULL;
    }

    // Make a course check to see if the node has enough resources.
    auto node = context->GetNode();
    if (!HasEnoughResources(node)) {
        return NULL;
    }

    auto job = DoScheduleJob(context, isStarving);
    if (!job) {
        return NULL;
    }

    ++RunningJobCount;
    LogProgress();
    return job;
}

void TOperationControllerBase::OnTaskUpdated(TTaskPtr task)
{
    int oldJobCount = CachedPendingJobCount;
    int newJobCount = CachedPendingJobCount + task->GetPendingJobCountDelta();
    CachedPendingJobCount = newJobCount;

    CachedNeededResources += task->GetTotalNeededResourcesDelta();

    LOG_DEBUG_IF(newJobCount != oldJobCount, "Pending job count updated (Task: %s, Count: %d -> %d, NeededResources: {%s})",
        ~task->GetId(),
        oldJobCount,
        newJobCount,
        ~FormatResources(CachedNeededResources));
}

void TOperationControllerBase::AddTaskPendingHint(TTaskPtr task)
{
    if (!task->IsStrictlyLocal() && task->GetPendingJobCount() > 0) {
        auto* info = GetPendingTaskInfo(task);
        if (info->GlobalTasks.insert(task).second) {
            LOG_DEBUG("Task pending hint added (Task: %s)",
                ~task->GetId());
        }
    }
    OnTaskUpdated(task);
}

void TOperationControllerBase::DoAddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    auto* info = GetPendingTaskInfo(task);
    if (info->AddressToLocalTasks[address].insert(task).second) {
        LOG_TRACE("Task locality hint added (Task: %s, Address: %s)",
            ~task->GetId(),
            ~address);
    }
}

TOperationControllerBase::TPendingTaskInfo* TOperationControllerBase::GetPendingTaskInfo(TTaskPtr task)
{
    int priority = task->GetPriority();
    YASSERT(priority >= 0 && priority <= MaxTaskPriority);
    return &PendingTaskInfos[priority];
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    DoAddTaskLocalityHint(task, address);
    OnTaskUpdated(task);
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, TChunkStripePtr stripe)
{
    FOREACH (const auto& chunk, stripe->Chunks) {
        const auto& inputChunk = chunk.InputChunk;
        FOREACH (const auto& address, inputChunk->node_addresses()) {
            DoAddTaskLocalityHint(task, address);
        }
    }
    OnTaskUpdated(task);
}

bool TOperationControllerBase::HasEnoughResources(TExecNodePtr node)
{
    return Dominates(
        node->ResourceLimits() + node->ResourceUtilizationDiscount(),
        node->ResourceUtilization() + GetMinNeededResources());
}

bool TOperationControllerBase::HasEnoughResources(TTaskPtr task, TExecNodePtr node)
{
    return node->HasEnoughResources(task->GetMinNeededResources());
}

TJobPtr TOperationControllerBase::DoScheduleJob(
    ISchedulingContext* context,
    bool isStarving)
{
    // First try to find a local task for this node.
    auto now = TInstant::Now();
    auto node = context->GetNode();
    auto address = node->GetAddress();
    for (int priority = static_cast<int>(PendingTaskInfos.size()) - 1; priority >= 0; --priority) {
        auto& info = PendingTaskInfos[priority];
        auto localTasksIt = info.AddressToLocalTasks.find(address);
        if (localTasksIt == info.AddressToLocalTasks.end()) {
            continue;
        }

        i64 bestLocality = 0;
        TTaskPtr bestTask = NULL;

        auto& localTasks = localTasksIt->second;
        auto it = localTasks.begin();
        while (it != localTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            // Make sure that the task is ready to launch jobs.
            // Remove pending hint if not.
            i64 locality = task->GetLocality(address);
            if (locality <= 0) {
                localTasks.erase(jt);
                LOG_TRACE("Task locality hint removed (Task: %s, Address: %s)",
                    ~task->GetId(),
                    ~address);
                continue;
            }

            if (locality <= bestLocality) {
                continue;
            }

            if (!HasEnoughResources(task, node)) {
                continue;
            }

            if (task->GetPendingJobCount() == 0) {
                OnTaskUpdated(task);
                continue;
            }

            bestLocality = locality;
            bestTask = task;
        }

        if (bestTask) {
            auto job = bestTask->ScheduleJob(context);
            if (job) {
                auto delayedTime = bestTask->GetDelayedTime();
                LOG_DEBUG("Scheduled a local job (Task: %s, Address: %s, Priority: %d, Locality: %" PRId64 ", Delay: %s)",
                    ~bestTask->GetId(),
                    ~address,
                    priority,
                    bestLocality,
                    delayedTime ? ~ToString(now - delayedTime.Get()) : "Null");
                bestTask->SetDelayedTime(Null);
                OnTaskUpdated(bestTask);
                OnJobStarted(job);
                return job;
            }
        }
    }

    // Next look for other (global) tasks.
    for (int priority = static_cast<int>(PendingTaskInfos.size()) - 1; priority >= 0; --priority) {
        auto& info = PendingTaskInfos[priority];
        auto& globalTasks = info.GlobalTasks;
        auto it = globalTasks.begin();
        while (it != globalTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            // Make sure that the task is ready to launch jobs.
            // Remove pending hint if not.
            if (task->GetPendingJobCount() == 0) {
                LOG_DEBUG("Task pending hint removed (Task: %s)", ~task->GetId());
                globalTasks.erase(jt);
                OnTaskUpdated(task);
                continue;
            }

            if (!HasEnoughResources(task, node)) {
                continue;
            }

            // Use delayed execution unless starving.
            bool mustWait = false;
            auto delayedTime = task->GetDelayedTime();
            if (delayedTime) {
                mustWait = delayedTime.Get() + task->GetLocalityTimeout() > now;
            } else {
                task->SetDelayedTime(now);
                mustWait = true;
            }
            if (!isStarving && mustWait) {
                continue;
            }

            auto job = task->ScheduleJob(context);
            if (job) {
                LOG_DEBUG("Scheduled a non-local job (Task: %s, Address: %s, Priority: %d, Delay: %s)",
                    ~task->GetId(),
                    ~address,
                    priority,
                    delayedTime ? ~ToString(now - delayedTime.Get()) : "Null");
                OnTaskUpdated(task);
                OnJobStarted(job);
                return job;
            }
        }
    }

    return NULL;
}

TCancelableContextPtr TOperationControllerBase::GetCancelableContext()
{
    return CancelableContext;
}

IInvokerPtr TOperationControllerBase::GetCancelableControlInvoker()
{
    return CancelableControlInvoker;
}

IInvokerPtr TOperationControllerBase::GetCancelableBackgroundInvoker()
{
    return CancelableBackgroundInvoker;
}

int TOperationControllerBase::GetPendingJobCount()
{
    return CachedPendingJobCount;
}

NProto::TNodeResources TOperationControllerBase::GetUsedResources()
{
    return UsedResources;
}

NProto::TNodeResources TOperationControllerBase::GetNeededResources()
{
    return CachedNeededResources;
}

void TOperationControllerBase::OnOperationCompleted()
{
    VERIFY_THREAD_AFFINITY_ANY();

    YCHECK(Active);
    LOG_INFO("Operation completed");

    Running = false;

    Host->OnOperationCompleted(Operation);
}

void TOperationControllerBase::OnOperationFailed(const TError& error)
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (!Active)
        return;

    LOG_WARNING(error, "Operation failed");

    Running = false;
    Active = false;

    Host->OnOperationFailed(Operation, error);
}

void TOperationControllerBase::AbortTransactions()
{
    LOG_INFO("Aborting transactions");

    Operation->GetSchedulerTransaction()->Abort();

    // No need to abort the others.
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::CommitOutputs()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Committing outputs");

    auto batchReq = ObjectProxy.ExecuteBatch();

    FOREACH (const auto& table, OutputTables) {
        auto path = FromObjectId(table.ObjectId);
        // Split large outputs into separate requests.
        {
            TChunkListYPathProxy::TReqAttachPtr req;
            int reqSize = 0;
            auto flushReq = [&] () {
                if (req) {
                    batchReq->AddRequest(req, "attach_out");
                    reqSize = 0;
                    req.Reset();
                }
            };

            FOREACH (const auto& pair, table.OutputChunkTreeIds) {
                if (!req) {
                    req = TChunkListYPathProxy::Attach(FromObjectId(table.OutputChunkListId));
                    NMetaState::GenerateRpcMutationId(req);
                }
                *req->add_children_ids() = pair.second.ToProto();
                ++reqSize;
                if (reqSize >= Config->MaxChildrenPerAttachRequest) {
                    flushReq();
                }
            }

            flushReq();
        }
        if (table.KeyColumns) {
            LOG_INFO("Table %s will be marked as sorted by %s",
                ~table.Path.GetPath(),
                ~ConvertToYsonString(table.KeyColumns.Get(), EYsonFormat::Text).Data());
            auto req = TTableYPathProxy::SetSorted(path);
            SetTransactionId(req, OutputTransaction);
            ToProto(req->mutable_key_columns(), table.KeyColumns.Get());
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "set_out_sorted");
        }
    }

    {
        auto req = TTransactionYPathProxy::Commit(FromObjectId(InputTransaction->GetId()));
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "commit_in_tx");
    }

    {
        auto req = TTransactionYPathProxy::Commit(FromObjectId(OutputTransaction->GetId()));
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "commit_out_tx");
    }

    {
        auto req = TTransactionYPathProxy::Commit(FromObjectId(Operation->GetSchedulerTransaction()->GetId()));
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "commit_scheduler_tx");
    }

    // We don't need pings any longer, detach the transactions.
    Operation->GetSchedulerTransaction()->Detach();
    InputTransaction->Detach();
    OutputTransaction->Detach();

    return batchReq->Invoke();
}

void TOperationControllerBase::OnOutputsCommitted(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error committing outputs");

    {
        auto rsps = batchRsp->GetResponses("attach_out");
        FOREACH (auto rsp, rsps) {
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error attaching chunk trees");
        }
    }

    {
        auto rsps = batchRsp->GetResponses("set_out_sorted");
        FOREACH (auto rsp, rsps) {
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error marking output table as sorted");
        }
    }

    {
        auto rsp = batchRsp->GetResponse("commit_in_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error committing input transaction");
    }

    {
        auto rsp = batchRsp->GetResponse("commit_out_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error committing output transaction");
    }

    {
        auto rsp = batchRsp->GetResponse("commit_scheduler_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error committing scheduler transaction");
    }

    LOG_INFO("Outputs committed");
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::StartIOTransactions()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Starting IO transactions");

    auto batchReq = ObjectProxy.ExecuteBatch();
    const auto& schedulerTransactionId = Operation->GetSchedulerTransaction()->GetId();

    {
        auto req = TTransactionYPathProxy::CreateObject(FromObjectId(schedulerTransactionId));
        req->set_type(EObjectType::Transaction);
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "start_in_tx");
    }

    {
        auto req = TTransactionYPathProxy::CreateObject(FromObjectId(schedulerTransactionId));
        req->set_type(EObjectType::Transaction);
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req, "start_out_tx");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnIOTransactionsStarted(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error starting IO transactions");

    {
        auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_in_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error starting input transaction");
        auto id = TTransactionId::FromProto(rsp->object_id());
        LOG_INFO("Input transaction is %s", ~id.ToString());
        InputTransaction = Host->GetTransactionManager()->Attach(id, true);
    }

    {
        auto rsp = batchRsp->GetResponse<TTransactionYPathProxy::TRspCreateObject>("start_out_tx");
        THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error starting output transaction");
        auto id = TTransactionId::FromProto(rsp->object_id());
        LOG_INFO("Output transaction is %s", ~id.ToString());
        OutputTransaction = Host->GetTransactionManager()->Attach(id, true);
    }
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::GetObjectIds()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Getting object ids");

    auto batchReq = ObjectProxy.ExecuteBatch();

    FOREACH (const auto& table, InputTables) {
        auto req = TObjectYPathProxy::GetId(table.Path.GetPath());
        SetTransactionId(req, InputTransaction);
        req->set_allow_nonempty_path_suffix(true);
        batchReq->AddRequest(req, "get_in_id");
    }

    FOREACH (const auto& table, OutputTables) {
        auto req = TObjectYPathProxy::GetId(table.Path.GetPath());
        SetTransactionId(req, InputTransaction);
        // TODO(babenko): should we allow this?
        req->set_allow_nonempty_path_suffix(true);
        batchReq->AddRequest(req, "get_out_id");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnObjectIdsReceived(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error getting object ids");

    {
        auto getInIdRsps = batchRsp->GetResponses<TObjectYPathProxy::TRspGetId>("get_in_id");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            {
                auto rsp = getInIdRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting id for input table %s",
                    ~table.Path.GetPath());
                table.ObjectId = TObjectId::FromProto(rsp->object_id());
            }
        }
    }

    {
        auto getOutIdRsps = batchRsp->GetResponses<TObjectYPathProxy::TRspGetId>("get_out_id");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            {
                auto rsp = getOutIdRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting id for output table %s",
                    ~table.Path.GetPath());
                table.ObjectId = TObjectId::FromProto(rsp->object_id());
            }
        }
    }

    LOG_INFO("Object ids received");
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::RequestInputs()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Requesting inputs");

    auto batchReq = ObjectProxy.ExecuteBatch();

    FOREACH (const auto& table, InputTables) {
        auto path = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(path);
            SetTransactionId(req, InputTransaction);
            req->set_mode(ELockMode::Snapshot);
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "lock_in");
        }
        {
            // NB: Use table.Path here, otherwise path suffix is ignored.
            auto req = TTableYPathProxy::Fetch(table.Path.GetPath());
            SetTransactionId(req, InputTransaction);
            req->set_fetch_node_addresses(true);
            req->set_fetch_all_meta_extensions(true);
            req->set_negate(table.NegateFetch);
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "fetch_in");
        }
        {
            auto req = TYPathProxy::Get(path + "/@sorted");
            SetTransactionId(req, InputTransaction);
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "get_in_sorted");
        }
        {
            auto req = TYPathProxy::Get(path + "/@sorted_by");
            SetTransactionId(req, InputTransaction);
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "get_in_sorted_by");
        }
    }

    FOREACH (const auto& table, OutputTables) {
        auto path = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(path);
            SetTransactionId(req, OutputTransaction);
            req->set_mode(table.LockMode);
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "lock_out");
        }
        {
            auto req = TYPathProxy::Get(path + "/@channels");
            SetTransactionId(req, OutputTransaction);
            batchReq->AddRequest(req, "get_out_channels");
        }
        {
            auto req = TYPathProxy::Get(path + "/@row_count");
            SetTransactionId(req, OutputTransaction);
            batchReq->AddRequest(req, "get_out_row_count");
        }
        if (table.Clear) {
            LOG_INFO("Output table %s will be cleared", ~table.Path.GetPath());
            auto req = TTableYPathProxy::Clear(path);
            SetTransactionId(req, OutputTransaction);
            NMetaState::GenerateRpcMutationId(req);
            batchReq->AddRequest(req, "clear_out");
        } else {
            // Even if |Clear| is False we still add a dummy request
            // to keep "clear_out" requests aligned with output tables.
            batchReq->AddRequest(NULL, "clear_out");
        }
        {
            auto req = TTableYPathProxy::GetChunkListForUpdate(path);
            SetTransactionId(req, OutputTransaction);
            batchReq->AddRequest(req, "get_out_chunk_list");
        }
    }

    FOREACH (const auto& file, Files) {
        auto path = file.Path.GetPath();
        {
            auto req = TFileYPathProxy::FetchFile(path);
            SetTransactionId(req, InputTransaction->GetId());
            batchReq->AddRequest(req, "fetch_files");
        }
    }

    RequestCustomInputs(batchReq);

    return batchReq->Invoke();
}

void TOperationControllerBase::OnInputsReceived(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error requesting inputs");

    {
        auto fetchInRsps = batchRsp->GetResponses<TTableYPathProxy::TRspFetch>("fetch_in");
        auto lockInRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_in");
        auto getInSortedRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_in_sorted");
        auto getInKeyColumns = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_in_sorted_by");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            {
                auto rsp = lockInRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error locking input table %s",
                    ~table.Path.GetPath());
                LOG_INFO("Input table %s locked",
                    ~table.Path.GetPath());
            }
            {
                auto rsp = fetchInRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error fetching input input table %s",
                    ~table.Path.GetPath());
                table.FetchResponse = rsp;
                FOREACH (const auto& chunk, rsp->chunks()) {
                    auto chunkId = TChunkId::FromProto(chunk.slice().chunk_id());
                    if (chunk.node_addresses_size() == 0) {
                        THROW_ERROR_EXCEPTION("Chunk %s in input table %s is lost",
                            ~chunkId.ToString(),
                            ~table.Path.GetPath());
                    }
                    InputChunkIds.insert(chunkId);
                }
                LOG_INFO("Input table %s has %d chunks",
                    ~table.Path.GetPath(),
                    rsp->chunks_size());
            }
            bool sorted;
            {
                auto rsp = getInSortedRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting \"sorted\" attribute for input table %s",
                    ~table.Path.GetPath());
                sorted = ConvertTo<bool>(TYsonString(rsp->value()));
            }
            if (sorted) {
                auto rsp = getInKeyColumns[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting \"sorted_by\" attribute for input table %s",
                    ~table.Path.GetPath());
                table.KeyColumns = ConvertTo< std::vector<Stroka> >(TYsonString(rsp->value()));
                LOG_INFO("Input table %s is sorted by %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.KeyColumns.Get(), EYsonFormat::Text).Data());
            } else {
                LOG_INFO("Input table %s is not sorted",
                    ~table.Path.GetPath());
            }
        }
    }

    {
        auto lockOutRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_out");
        auto clearOutRsps = batchRsp->GetResponses<TTableYPathProxy::TRspClear>("clear_out");
        auto getOutChunkListRsps = batchRsp->GetResponses<TTableYPathProxy::TRspGetChunkListForUpdate>("get_out_chunk_list");
        auto getOutChannelsRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_out_channels");
        auto getOutRowCountRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_out_row_count");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            {
                auto rsp = lockOutRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error locking output table %s",
                    ~table.Path.GetPath());
                LOG_INFO("Output table %s locked",
                    ~table.Path.GetPath());
            }
            {
                auto rsp = getOutChannelsRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting channels for output table %s",
                    ~table.Path.GetPath());
                table.Channels = TYsonString(rsp->value());
                LOG_INFO("Output table %s has channels %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.Channels, EYsonFormat::Text).Data());
            }
            {
                auto rsp = getOutRowCountRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting \"row_count\" attribute for output table %s",
                    ~table.Path.GetPath());
                i64 initialRowCount = ConvertTo<i64>(TYsonString(rsp->value()));
                if (initialRowCount > 0 && table.Clear && !table.Overwrite) {
                    THROW_ERROR_EXCEPTION("Output table %s must be empty (use \"overwrite\" attribute to force clearing it)",
                        ~table.Path.GetPath());
                }
            }
            if (table.Clear) {
                auto rsp = clearOutRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error clearing output table %s",
                    ~table.Path.GetPath());
                LOG_INFO("Output table %s cleared",
                    ~table.Path.GetPath());
            }
            {
                auto rsp = getOutChunkListRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting output chunk list for table %s",
                    ~table.Path.GetPath());
                table.OutputChunkListId = TChunkListId::FromProto(rsp->chunk_list_id());
                LOG_INFO("Output table %s has output chunk list %s",
                    ~table.Path.GetPath(),
                    ~table.OutputChunkListId.ToString());
            }
        }
    }

    {
        auto fetchFilesRsps = batchRsp->GetResponses<TFileYPathProxy::TRspFetchFile>("fetch_files");
        for (int index = 0; index < static_cast<int>(Files.size()); ++index) {
            auto& file = Files[index];
            {
                auto rsp = fetchFilesRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error fetching files");
                file.FetchResponse = rsp;
                LOG_INFO("File %s consists of chunk %s",
                    ~file.Path.GetPath(),
                    ~TChunkId::FromProto(rsp->chunk_id()).ToString());
            }
        }
    }

    OnCustomInputsRecieved(batchRsp);

    LOG_INFO("Inputs received");
}

void TOperationControllerBase::RequestCustomInputs(TObjectServiceProxy::TReqExecuteBatchPtr batchReq)
{
    UNUSED(batchReq);
}

void TOperationControllerBase::OnCustomInputsRecieved(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    UNUSED(batchRsp);
}

void TOperationControllerBase::CompletePreparation()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Completing preparation");

    ChunkListPool = New<TChunkListPool>(
        Config,
        Host->GetMasterChannel(),
        CancelableControlInvoker,
        Operation);
}

void TOperationControllerBase::OnPreparationCompleted()
{
    if (!Active)
        return;

    LOG_INFO("Preparation completed");
}

TAsyncPipeline<void>::TPtr TOperationControllerBase::CustomizePreparationPipeline(TAsyncPipeline<void>::TPtr pipeline)
{
    return pipeline;
}

void TOperationControllerBase::ReleaseChunkList(const TChunkListId& id)
{
    std::vector<TChunkListId> ids;
    ids.push_back(id);
    ReleaseChunkLists(ids);
}

void TOperationControllerBase::ReleaseChunkLists(const std::vector<TChunkListId>& ids)
{
    auto batchReq = ObjectProxy.ExecuteBatch();
    FOREACH (const auto& id, ids) {
        auto req = TTransactionYPathProxy::ReleaseObject();
        *req->mutable_object_id() = id.ToProto();
        NMetaState::GenerateRpcMutationId(req);
        batchReq->AddRequest(req);
    }

    // Fire-and-forget.
    // The subscriber is only needed to log the outcome.
    batchReq->Invoke().Subscribe(
        BIND(&TThis::OnChunkListsReleased, MakeStrong(this)));
}

void TOperationControllerBase::OnChunkListsReleased(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    if (!batchRsp->IsOK()) {
        LOG_WARNING(*batchRsp, "Error releasing chunk lists");
    }
}

std::vector<TRefCountedInputChunkPtr> TOperationControllerBase::CollectInputTablesChunks()
{
    // TODO(babenko): set row_attributes
    std::vector<TRefCountedInputChunkPtr> result;
    FOREACH (const auto& table, InputTables) {
        FOREACH (const auto& inputChunk, table.FetchResponse->chunks()) {
            result.push_back(New<TRefCountedInputChunk>(inputChunk));
        }
    }
    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::PrepareChunkStripes(
    const std::vector<TRefCountedInputChunkPtr>& inputChunks,
    TNullable<int> jobCount,
    i64 jobSliceDataSize)
{
    i64 sliceDataSize = jobSliceDataSize;
    if (jobCount) {
        i64 totalDataSize = 0;
        FOREACH (auto inputChunk, inputChunks) {
            totalDataSize += inputChunk->uncompressed_data_size();
        }
        sliceDataSize = std::min(sliceDataSize, totalDataSize / jobCount.Get() + 1);
    }

    YCHECK(sliceDataSize > 0);

    LOG_DEBUG("Preparing chunk stripes (ChunkCount: %d, JobCount: %s, JobSliceDataSize: %" PRId64 ", SliceDataSize: %" PRId64 ")",
        static_cast<int>(inputChunks.size()),
        ~ToString(jobCount),
        jobSliceDataSize,
        sliceDataSize);

    std::vector<TChunkStripePtr> result;

    // Ensure that no input chunk has size larger than sliceSize.
    FOREACH (auto inputChunk, inputChunks) {
        auto chunkId = TChunkId::FromProto(inputChunk->slice().chunk_id());
        if (inputChunk->uncompressed_data_size() > sliceDataSize) {
            int sliceCount = (int) std::ceil((double) inputChunk->uncompressed_data_size() / (double) sliceDataSize);
            auto slicedInputChunks = SliceChunkEvenly(*inputChunk, sliceCount);
            FOREACH (auto slicedInputChunk, slicedInputChunks) {
                auto stripe = New<TChunkStripe>(slicedInputChunk);
                result.push_back(stripe);
            }
            LOG_TRACE("Slicing chunk (ChunkId: %s, SliceCount: %d)",
                ~chunkId.ToString(),
                sliceCount);
        } else {
            auto stripe = New<TChunkStripe>(inputChunk);
            result.push_back(stripe);
            LOG_TRACE("Taking whole chunk (ChunkId: %s)",
                ~chunkId.ToString());
        }
    }

    LOG_DEBUG("Chunk stripes prepared (StripeCount: %d)",
        static_cast<int>(result.size()));

    return result;
}

std::vector<Stroka> TOperationControllerBase::CheckInputTablesSorted(const TNullable< std::vector<Stroka> >& keyColumns)
{
    YCHECK(!InputTables.empty());

    FOREACH (const auto& table, InputTables) {
        if (!table.KeyColumns) {
            THROW_ERROR_EXCEPTION("Input table %s is not sorted",
                ~table.Path.GetPath());
        }
    }

    if (keyColumns) {
        FOREACH (const auto& table, InputTables) {
            if (!CheckKeyColumnsCompatible(table.KeyColumns.Get(), keyColumns.Get())) {
                THROW_ERROR_EXCEPTION("Input table %s is sorted by columns %s that are not compatible with the requested columns %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.KeyColumns.Get(), EYsonFormat::Text).Data(),
                    ~ConvertToYsonString(keyColumns.Get(), EYsonFormat::Text).Data());
            }
        }
        return keyColumns.Get();
    } else {
        const auto& referenceTable = InputTables[0];
        FOREACH (const auto& table, InputTables) {
            if (table.KeyColumns != referenceTable.KeyColumns) {
                THROW_ERROR_EXCEPTION("Key columns do not match: input table %s is sorted by columns %s while input table %s is sorted by columns %s",
                    ~table.Path.GetPath(),
                    ~ConvertToYsonString(table.KeyColumns.Get(), EYsonFormat::Text).Data(),
                    ~referenceTable.Path.GetPath(),
                    ~ConvertToYsonString(referenceTable.KeyColumns.Get(), EYsonFormat::Text).Data());
            }
        }
        return referenceTable.KeyColumns.Get();
    }
}

bool TOperationControllerBase::CheckKeyColumnsCompatible(
    const std::vector<Stroka>& fullColumns,
    const std::vector<Stroka>& prefixColumns)
{
    if (fullColumns.size() < prefixColumns.size()) {
        return false;
    }

    for (int index = 0; index < static_cast<int>(prefixColumns.size()); ++index) {
        if (fullColumns[index] != prefixColumns[index]) {
            return false;
        }
    }

    return true;
}

void TOperationControllerBase::RegisterOutputChunkTree(
    const NChunkServer::TChunkTreeId& chunkTreeId,
    int key,
    int tableIndex)
{
    auto& table = OutputTables[tableIndex];
    table.OutputChunkTreeIds.insert(std::make_pair(key, chunkTreeId));

    LOG_DEBUG("Output chunk tree registered (Table: %d, ChunkTreeId: %s, Key: %d)",
        tableIndex,
        ~chunkTreeId.ToString(),
        key);
}

void TOperationControllerBase::RegisterOutputChunkTrees(
    TJobInProgressPtr jip,
    int key)
{
    for (int tableIndex = 0; tableIndex < static_cast<int>(OutputTables.size()); ++tableIndex) {
        RegisterOutputChunkTree(jip->ChunkListIds[tableIndex], key, tableIndex);
    }
}

bool TOperationControllerBase::HasEnoughChunkLists(int requestedCount)
{
    return ChunkListPool->HasEnough(requestedCount);
}

TChunkListId TOperationControllerBase::ExtractChunkList()
{
    return ChunkListPool->Extract();
}

void TOperationControllerBase::RegisterJobInProgress(TJobInProgressPtr jip)
{
    YCHECK(JobsInProgress.insert(MakePair(jip->Job, jip)).second);
}

TOperationControllerBase::TJobInProgressPtr TOperationControllerBase::GetJobInProgress(TJobPtr job)
{
    auto it = JobsInProgress.find(job);
    YCHECK(it != JobsInProgress.end());
    return it->second;
}

void TOperationControllerBase::RemoveJobInProgress(TJobPtr job)
{
    YCHECK(JobsInProgress.erase(job) == 1);
}

void TOperationControllerBase::BuildProgressYson(IYsonConsumer* consumer)
{
    BuildYsonMapFluently(consumer)
        .Item("jobs").BeginMap()
            .Item("total").Scalar(CompletedJobCount + RunningJobCount + GetPendingJobCount())
            .Item("pending").Scalar(GetPendingJobCount())
            .Item("running").Scalar(RunningJobCount)
            .Item("completed").Scalar(CompletedJobCount)
            .Item("failed").Scalar(FailedJobCount)
            .Item("aborted").Scalar(AbortedJobCount)
        .EndMap();
}

void TOperationControllerBase::BuildResultYson(IYsonConsumer* consumer)
{
    auto error = FromProto(Operation->Result().error());
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("error").Scalar(error)
        .EndMap();
}

std::vector<TRichYPath> TOperationControllerBase::GetFilePaths() const
{
    return std::vector<TRichYPath>();
}

int TOperationControllerBase::SuggestJobCount(
    i64 totalDataSize,
    i64 minDataSizePerJob,
    i64 maxDataSizePerJob,
    TNullable<int> configJobCount,
    int chunkCount)
{
    int minSuggestion = static_cast<int>(std::ceil((double) totalDataSize / maxDataSizePerJob));
    int maxSuggestion = static_cast<int>(std::ceil((double) totalDataSize / minDataSizePerJob));
    int result = configJobCount.Get(minSuggestion);
    result = std::min(result, chunkCount);
    result = std::min(result, maxSuggestion);
    result = std::max(result, 1);
    result = std::min(result, Config->MaxJobCount);
    return result;
}

void TOperationControllerBase::InitUserJobSpec(
    NScheduler::NProto::TUserJobSpec* proto,
    TUserJobSpecPtr config,
    const std::vector<TUserFile>& files)
{
    proto->set_shell_command(config->Command);

    {
        // Set input and output format.
        TFormat inputFormat(EFormatType::Yson);
        TFormat outputFormat(EFormatType::Yson);

        if (config->Format) {
            inputFormat = outputFormat = config->Format.Get();
        }

        if (config->InputFormat) {
            inputFormat = config->InputFormat.Get();
        }

        if (config->OutputFormat) {
            outputFormat = config->OutputFormat.Get();
        }

        proto->set_input_format(ConvertToYsonString(inputFormat).Data());
        proto->set_output_format(ConvertToYsonString(outputFormat).Data());
    }

    auto fillEnvironment = [&] (yhash_map<Stroka, Stroka>& env) {
        FOREACH(const auto& pair, env) {
            proto->add_environment(Sprintf("%s=%s", ~pair.first, ~pair.second));
        }
    };

    // Global environment.
    fillEnvironment(Config->Environment);

    // Local environment.
    fillEnvironment(config->Environment);

    proto->add_environment(Sprintf("YT_OPERATION_ID=%s", 
        ~Operation->GetOperationId().ToString()));

    // TODO(babenko): think about per-job files
    FOREACH (const auto& file, files) {
        *proto->add_files() = *file.FetchResponse;
    }
}

void TOperationControllerBase::AddUserJobEnvironment(
    NScheduler::NProto::TUserJobSpec* proto,
    TJobInProgressPtr jip)
{
    proto->add_environment(Sprintf("YT_JOB_INDEX=%d", jip->JobIndex));
    proto->add_environment(Sprintf("YT_JOB_ID=%s", ~jip->Job->GetId().ToString()));
}

void TOperationControllerBase::InitIntermediateOutputConfig(TJobIOConfigPtr config)
{
    // Don't replicate intermediate output.
    config->TableWriter->ReplicationFactor = 1;
    config->TableWriter->UploadReplicationFactor = 1;

    // Cache blocks on nodes.
    config->TableWriter->EnableNodeCaching = true;

    // Don't move intermediate chunks.
    config->TableWriter->ChunksMovable = false;
}

void TOperationControllerBase::InitIntermediateInputConfig(TJobIOConfigPtr config)
{
    // Disable master requests.
    config->TableReader->AllowFetchingSeedsFromMaster = false;
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

