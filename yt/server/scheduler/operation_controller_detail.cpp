#include "stdafx.h"
#include "operation_controller_detail.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "chunk_pool.h"
#include "helpers.h"
#include "master_connector.h"

#include <ytlib/transaction_client/transaction.h>

#include <ytlib/node_tracker_client/node_directory_builder.h>

#include <ytlib/chunk_client/chunk_list_ypath_proxy.h>
#include <ytlib/chunk_client/key.h>
#include <ytlib/chunk_client/schema.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/chunk_spec.h>

#include <ytlib/erasure/codec.h>

#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/object_client/object_ypath_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/ytree/fluent.h>
#include <ytlib/ytree/convert.h>
#include <ytlib/ytree/attribute_helpers.h>

#include <ytlib/formats/format.h>

#include <ytlib/transaction_client/transaction_ypath_proxy.h>
#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/scheduler/config.h>
#include <ytlib/scheduler/helpers.h>

#include <ytlib/meta_state/rpc_helpers.h>

#include <ytlib/security_client/rpc_helpers.h>

#include <cmath>

namespace NYT {
namespace NScheduler {

using namespace NNodeTrackerClient;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NFormats;
using namespace NJobProxy;
using namespace NSecurityClient;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NNodeTrackerClient::NProto;

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TInputChunkScratcher::TInputChunkScratcher(TOperationControllerBase* controller)
    : Controller(controller)
    , PeriodicInvoker(New<TPeriodicInvoker>(
        Controller->GetCancelableControlInvoker(),
        BIND(&TInputChunkScratcher::LocateChunks, MakeWeak(this)),
        Controller->Config->ChunkScratchPeriod))
    , Proxy(Controller->Host->GetMasterChannel())
    , Started(false)
    , Logger(Controller->Logger)
{ }

void TOperationControllerBase::TInputChunkScratcher::Start()
{
    if (Started)
        return;

    Started = true;

    LOG_DEBUG("Starting input chunk scratcher");

    NextChunkIterator = Controller->InputChunks.begin();
    PeriodicInvoker->Start();
}

void TOperationControllerBase::TInputChunkScratcher::Stop()
{
    if (Started) {
        PeriodicInvoker->Stop();
    }
}

void TOperationControllerBase::TInputChunkScratcher::LocateChunks()
{
    auto startIterator = NextChunkIterator;
    auto req = Proxy.LocateChunks();

    for (int chunkCount = 0; chunkCount < Controller->Config->MaxChunksPerScratch; ++chunkCount) {
        ToProto(req->add_chunk_ids(), NextChunkIterator->first);

        ++NextChunkIterator;
        if (NextChunkIterator == Controller->InputChunks.end()) {
            NextChunkIterator = Controller->InputChunks.begin();
        }

        if (NextChunkIterator == startIterator) {
            // Total number of chunks is less than MaxChunksPerScratch.
            break;
        }
    }

    LOG_DEBUG("Sending locate chunks request for %d chunks", static_cast<int>(req->chunk_ids_size()));

    req->Invoke().Subscribe(
        BIND(&TInputChunkScratcher::OnLocateChunksResponse, MakeWeak(this))
            .Via(Controller->GetCancelableControlInvoker()));
}

void TOperationControllerBase::TInputChunkScratcher::OnLocateChunksResponse(TChunkServiceProxy::TRspLocateChunksPtr rsp)
{
    if (!rsp->IsOK()) {
        LOG_WARNING(*rsp, "Failed to locate input chunks");
        return;
    }

    LOG_DEBUG("Located %d input chunks", static_cast<int>(rsp->chunks_size()));

    Controller->NodeDirectory->MergeFrom(rsp->node_directory());
    FOREACH(const auto& chunkInfo, rsp->chunks()) {
        auto chunkId = FromProto<TChunkId>(chunkInfo.chunk_id());
        auto it = Controller->InputChunks.find(chunkId);
        YCHECK(it != Controller->InputChunks.end());

        auto replicas = FromProto<TChunkReplica, TChunkReplicaList>(chunkInfo.replicas());

        auto& descriptor = it->second;
        YCHECK(!descriptor.ChunkSpecs.empty());
        auto& chunkSpec = descriptor.ChunkSpecs.front();
        auto codecId = NErasure::ECodec(chunkSpec->erasure_codec());

        if (IsUnavailable(replicas, codecId)) {
            Controller->OnInputChunkUnavailable(chunkId, descriptor);
        } else {
            Controller->OnInputChunkAvailable(chunkId, descriptor, replicas);
        }
    }
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TTask::TTask(TOperationControllerBase* controller)
    : Controller(controller)
    , CachedPendingJobCount(0)
    , CachedTotalNeededResources(ZeroNodeResources())
    , LastDemandSanityCheckTime(TInstant::Zero())
    , CompletedFired(false)
    , Logger(Controller->Logger)
{ }

int TOperationControllerBase::TTask::GetPendingJobCount() const
{
    return GetChunkPoolOutput()->GetPendingJobCount();
}

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
    // NB: Don't call GetMinNeededResources if there are no pending jobs.
    return count == 0 ? ZeroNodeResources() : GetMinNeededResources() * count;
}

i64 TOperationControllerBase::TTask::GetLocality(const Stroka& address) const
{
    return GetChunkPoolOutput()->GetLocality(address);
}

bool TOperationControllerBase::TTask::HasInputLocality()
{
    return true;
}

void TOperationControllerBase::TTask::AddInput(TChunkStripePtr stripe)
{
    Controller->RegisterInputStripe(stripe, this);
    if (HasInputLocality()) {
        Controller->AddTaskLocalityHint(this, stripe);
    }
    AddPendingHint();
}

void TOperationControllerBase::TTask::AddInput(const std::vector<TChunkStripePtr>& stripes)
{
    FOREACH (auto stripe, stripes) {
        if (stripe)
            AddInput(stripe);
    }
}

void TOperationControllerBase::TTask::FinishInput()
{
    LOG_DEBUG("Task input finished (Task: %s)", ~GetId());

    GetChunkPoolInput()->Finish();
    AddPendingHint();
}

void TOperationControllerBase::TTask::CheckCompleted()
{
    if (!CompletedFired && IsCompleted()) {
        CompletedFired = true;
        OnTaskCompleted();
    }
}

TJobPtr TOperationControllerBase::TTask::ScheduleJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    int chunkListCount = GetChunkListCountPerJob();
    if (!Controller->HasEnoughChunkLists(chunkListCount)) {
        LOG_DEBUG("Job chunk list demand is not met (Task: %s)", ~GetId());
        return nullptr;
    }

    int jobIndex = Controller->JobIndexGenerator.Next();
    auto joblet = New<TJoblet>(this, jobIndex);

    auto node = context->GetNode();
    const auto& address = node->GetAddress();
    auto* chunkPoolOutput = GetChunkPoolOutput();
    joblet->OutputCookie = chunkPoolOutput->Extract(address);
    if (joblet->OutputCookie == IChunkPoolOutput::NullCookie) {
        LOG_DEBUG("Job input is empty (Task: %s)", ~GetId());
        return nullptr;
    }

    joblet->InputStripeList = chunkPoolOutput->GetStripeList(joblet->OutputCookie);
    auto neededResources = GetNeededResources(joblet);

    // Check the usage against the limits. This is the last chance to give up.
    if (!Dominates(jobLimits, neededResources)) {
        LOG_DEBUG("Job actual resource demand is not met (Task: %s, Limits: {%s}, Demand: {%s})",
            ~GetId(),
            ~FormatResources(jobLimits),
            ~FormatResources(neededResources));
        CheckResourceDemandSanity(node, neededResources);
        chunkPoolOutput->Aborted(joblet->OutputCookie);
        // Seems like cached min needed resources are too optimistic.
        CachedMinNeededResources = GetMinNeededResourcesHeavy();
        return nullptr;
    }

    auto jobType = GetJobType();

    // Async part.
    auto this_ = MakeStrong(this);
    auto jobSpecBuilder = BIND([=] (TJobSpec* jobSpec) -> TVoid {
        this_->BuildJobSpec(joblet, jobSpec);
        this_->Controller->CustomizeJobSpec(joblet, jobSpec);

        // Adjust sizes if approximation flag is set.
        if (joblet->InputStripeList->IsApproximate) {
            auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
            schedulerJobSpecExt->set_input_uncompressed_data_size(static_cast<i64>(
                schedulerJobSpecExt->input_uncompressed_data_size() *
                ApproximateSizesBoostFactor));
            schedulerJobSpecExt->set_input_row_count(static_cast<i64>(
                schedulerJobSpecExt->input_row_count() *
                ApproximateSizesBoostFactor));
        }

        return TVoid();
    });

    joblet->Job = context->StartJob(
        Controller->Operation,
        jobType,
        neededResources,
        jobSpecBuilder);

    LOG_INFO(
        "Job scheduled (JobId: %s, OperationId: %s, JobType: %s, Address: %s, JobIndex: %d, ChunkCount: %d (%d local), "
        "Approximate: %s, DataSize: %" PRId64 "(%" PRId64 " local), RowCount: %" PRId64 ", ResourceLimits: {%s})",
        ~ToString(joblet->Job->GetId()),
        ~ToString(Controller->Operation->GetOperationId()),
        ~jobType.ToString(),
        ~context->GetNode()->GetAddress(),
        ~GetId(),
        jobIndex,
        joblet->InputStripeList->TotalChunkCount,
        joblet->InputStripeList->LocalChunkCount,
        ~FormatBool(joblet->InputStripeList->IsApproximate),
        joblet->InputStripeList->TotalDataSize,
        joblet->InputStripeList->LocalDataSize,
        joblet->InputStripeList->TotalRowCount,
        ~FormatResources(neededResources));

    // Prepare chunk lists.
    for (int index = 0; index < chunkListCount; ++index) {
        auto id = Controller->ExtractChunkList();
        joblet->ChunkListIds.push_back(id);
    }

    // Sync part.
    PrepareJoblet(joblet);
    Controller->CustomizeJoblet(joblet);

    Controller->RegisterJoblet(joblet);

    OnJobStarted(joblet);

    return joblet->Job;
}

bool TOperationControllerBase::TTask::IsPending() const
{
    return GetChunkPoolOutput()->GetPendingJobCount() > 0;
}

bool TOperationControllerBase::TTask::IsCompleted() const
{
    return GetChunkPoolOutput()->IsCompleted();
}

i64 TOperationControllerBase::TTask::GetTotalDataSize() const
{
    return GetChunkPoolOutput()->GetTotalDataSize();
}

i64 TOperationControllerBase::TTask::GetCompletedDataSize() const
{
    return GetChunkPoolOutput()->GetCompletedDataSize();
}

i64 TOperationControllerBase::TTask::GetPendingDataSize() const
{
    return GetChunkPoolOutput()->GetPendingDataSize();
}

void TOperationControllerBase::TTask::PrepareJoblet(TJobletPtr joblet)
{
    UNUSED(joblet);
}

void TOperationControllerBase::TTask::OnJobStarted(TJobletPtr joblet)
{
    UNUSED(joblet);
}

void TOperationControllerBase::TTask::OnJobCompleted(TJobletPtr joblet)
{
    GetChunkPoolOutput()->Completed(joblet->OutputCookie);
}

void TOperationControllerBase::TTask::ReinstallJob(TJobletPtr joblet, EJobReinstallReason reason)
{
    Controller->ChunkListPool->Release(joblet->ChunkListIds);

    auto* chunkPoolOutput = GetChunkPoolOutput();

    auto list =
        HasInputLocality()
        ? chunkPoolOutput->GetStripeList(joblet->OutputCookie)
        : nullptr;

    switch (reason) {
        case EJobReinstallReason::Failed:
            chunkPoolOutput->Failed(joblet->OutputCookie);
            break;
        case EJobReinstallReason::Aborted:
            chunkPoolOutput->Aborted(joblet->OutputCookie);
            break;
        default:
            YUNREACHABLE();
    }

    if (HasInputLocality()) {
        FOREACH (const auto& stripe, list->Stripes) {
            Controller->AddTaskLocalityHint(this, stripe);
        }
    }

    AddPendingHint();
}

void TOperationControllerBase::TTask::OnJobFailed(TJobletPtr joblet)
{
    ReinstallJob(joblet, EJobReinstallReason::Failed);
}

void TOperationControllerBase::TTask::OnJobAborted(TJobletPtr joblet)
{
    ReinstallJob(joblet, EJobReinstallReason::Aborted);
}

void TOperationControllerBase::TTask::OnTaskCompleted()
{
    LOG_DEBUG("Task completed (Task: %s)", ~GetId());
}

void TOperationControllerBase::TTask::DoCheckResourceDemandSanity(
    const TNodeResources& neededResources)
{
    auto nodes = Controller->Host->GetExecNodes();
    FOREACH (auto node, nodes) {
        if (Dominates(node->ResourceLimits(), neededResources))
            return;
    }

    // It seems nobody can satisfy the demand.
    Controller->OnOperationFailed(
        TError("No online exec node can satisfy the resource demand")
            << TErrorAttribute("task", TRawString(GetId()))
            << TErrorAttribute("needed_resources", neededResources));
}

void TOperationControllerBase::TTask::CheckResourceDemandSanity(
    const TNodeResources& neededResources)
{
    // Run sanity check to see if any node can provide enough resources.
    // Don't run these checks too often to avoid jeopardizing performance.
    auto now = TInstant::Now();
    if (now < LastDemandSanityCheckTime + Controller->Config->ResourceDemandSanityCheckPeriod)
        return;
    LastDemandSanityCheckTime = now;

    // Schedule check in control thread.
    Controller->GetCancelableControlInvoker()->Invoke(BIND(
        &TTask::DoCheckResourceDemandSanity,
        MakeWeak(this),
        neededResources));
}

void TOperationControllerBase::TTask::CheckResourceDemandSanity(
    TExecNodePtr node,
    const TNodeResources& neededResources)
{
    // The task is requesting more than some node is willing to provide it.
    // Maybe it's OK and we should wait for some time.
    // Or maybe it's not and the task is requesting something no one is able to provide.

    // First check if this very node has enough resources (including those currently
    // allocated by other jobs).
    if (Dominates(node->ResourceLimits(), neededResources))
        return;

    CheckResourceDemandSanity(neededResources);
}

void TOperationControllerBase::TTask::AddPendingHint()
{
    Controller->AddTaskPendingHint(this);
}

void TOperationControllerBase::TTask::AddLocalityHint(const Stroka& address)
{
    Controller->AddTaskLocalityHint(this, address);
}

void TOperationControllerBase::TTask::AddSequentialInputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet,
    bool enableTableIndex)
{
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    TNodeDirectoryBuilder directoryBuilder(Controller->NodeDirectory, schedulerJobSpecExt->mutable_node_directory());
    auto* inputSpec = schedulerJobSpecExt->add_input_specs();
    auto list = joblet->InputStripeList;
    FOREACH (const auto& stripe, list->Stripes) {
        AddChunksToInputSpec(&directoryBuilder, inputSpec, stripe, list->PartitionTag, enableTableIndex);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::AddParallelInputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet,
    bool enableTableIndex)
{
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    TNodeDirectoryBuilder directoryBuilder(Controller->NodeDirectory, schedulerJobSpecExt->mutable_node_directory());
    auto list = joblet->InputStripeList;
    FOREACH (const auto& stripe, list->Stripes) {
        auto* inputSpec = schedulerJobSpecExt->add_input_specs();
        AddChunksToInputSpec(&directoryBuilder, inputSpec, stripe, list->PartitionTag, enableTableIndex);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::AddChunksToInputSpec(
    TNodeDirectoryBuilder* directoryBuilder,
    TTableInputSpec* inputSpec,
    TChunkStripePtr stripe,
    TNullable<int> partitionTag,
    bool enableTableIndex)
{
    FOREACH (const auto& chunkSlice, stripe->ChunkSlices) {
        auto* chunkSpec = inputSpec->add_chunks();
        ToProto(chunkSpec, *chunkSlice);
        FOREACH (ui32 protoReplica, chunkSlice->GetChunkSpec()->replicas()) {
            auto replica = FromProto<TChunkReplica>(protoReplica);
            directoryBuilder->Add(replica);
        }
        if (!enableTableIndex) {
            chunkSpec->clear_table_index();
        }
        if (partitionTag) {
            chunkSpec->set_partition_tag(partitionTag.Get());
        }
    }
}

void TOperationControllerBase::TTask::UpdateInputSpecTotals(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto list = joblet->InputStripeList;
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    schedulerJobSpecExt->set_input_uncompressed_data_size(
        schedulerJobSpecExt->input_uncompressed_data_size() +
        list->TotalDataSize);
    schedulerJobSpecExt->set_input_row_count(
        schedulerJobSpecExt->input_row_count() +
        list->TotalRowCount);
}

void TOperationControllerBase::TTask::AddFinalOutputSpecs(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    YCHECK(joblet->ChunkListIds.size() == Controller->OutputTables.size());
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    for (int index = 0; index < static_cast<int>(Controller->OutputTables.size()); ++index) {
        const auto& table = Controller->OutputTables[index];
        auto* outputSpec = schedulerJobSpecExt->add_output_specs();
        outputSpec->set_table_writer_options(ConvertToYsonString(table.Options).Data());
        ToProto(outputSpec->mutable_chunk_list_id(), joblet->ChunkListIds[index]);
    }
}

void TOperationControllerBase::TTask::AddIntermediateOutputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    YCHECK(joblet->ChunkListIds.size() == 1);
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    auto* outputSpec = schedulerJobSpecExt->add_output_specs();
    auto options = New<TTableWriterOptions>();
    options->Account = Controller->Spec->IntermediateDataAccount;
    options->ChunksVital = false;
    options->ReplicationFactor = 1;
    outputSpec->set_table_writer_options(ConvertToYsonString(options).Data());
    ToProto(outputSpec->mutable_chunk_list_id(), joblet->ChunkListIds[0]);
}

const TNodeResources& TOperationControllerBase::TTask::GetMinNeededResources() const
{
    if (!CachedMinNeededResources) {
        YCHECK(GetPendingJobCount() > 0);
        CachedMinNeededResources = GetMinNeededResourcesHeavy();
    }
    return *CachedMinNeededResources;
}

TNodeResources TOperationControllerBase::TTask::GetNeededResources(TJobletPtr joblet) const
{
    UNUSED(joblet);
    return GetMinNeededResources();
}

void TOperationControllerBase::TTask::RegisterIntermediate(
    TJobletPtr joblet,
    TChunkStripePtr stripe,
    TTaskPtr destinationTask)
{
    RegisterIntermediate(joblet, stripe, destinationTask->GetChunkPoolInput());

    if (destinationTask->HasInputLocality()) {
        Controller->AddTaskLocalityHint(destinationTask, stripe);
    }
    destinationTask->AddPendingHint();
}

void TOperationControllerBase::TTask::RegisterIntermediate(
    TJobletPtr joblet,
    TChunkStripePtr stripe,
    IChunkPoolInput* destinationPool)
{
    IChunkPoolInput::TCookie inputCookie;

    auto lostIt = LostJobCookieMap.find(joblet->OutputCookie);
    if (lostIt == LostJobCookieMap.end()) {
        inputCookie = destinationPool->Add(stripe);
    } else {
        inputCookie = lostIt->second;
        destinationPool->Resume(inputCookie, stripe);
        LostJobCookieMap.erase(lostIt);
    }

    // Store recovery info.
    auto completedJob = New<TCompletedJob>(
        joblet->Job->GetId(),
        this,
        joblet->OutputCookie,
        destinationPool,
        inputCookie,
        joblet->Job->GetNode());

    Controller->RegisterIntermediate(
        completedJob,
        stripe);
}

TChunkStripePtr TOperationControllerBase::TTask::BuildIntermediateChunkStripe(
    google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>* chunkSpecs)
{
    auto stripe = New<TChunkStripe>();
    FOREACH (auto& chunkSpec, *chunkSpecs) {
        auto chunkSlice = CreateChunkSlice(New<TRefCountedChunkSpec>(std::move(chunkSpec)));
        stripe->ChunkSlices.push_back(chunkSlice);
    }
    return stripe;
}

void TOperationControllerBase::TTask::RegisterOutput(TJobletPtr joblet, int key)
{
    Controller->RegisterOutput(joblet, key);
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TOperationControllerBase(
    TSchedulerConfigPtr config,
    TOperationSpecBasePtr spec,
    IOperationHost* host,
    TOperation* operation)
    : Config(config)
    , Host(host)
    , Operation(operation)
    , AuthenticatedMasterChannel(CreateAuthenticatedChannel(
        host->GetMasterChannel(),
        operation->GetAuthenticatedUser()))
    , Logger(OperationLogger)
    , CancelableContext(New<TCancelableContext>())
    , CancelableControlInvoker(CancelableContext->CreateInvoker(Host->GetControlInvoker()))
    , CancelableBackgroundInvoker(CancelableContext->CreateInvoker(Host->GetBackgroundInvoker()))
    , Running(false)
    , TotalInputChunkCount(0)
    , TotalInputDataSize(0)
    , TotalInputRowCount(0)
    , TotalInputValueCount(0)
    , UnavailableInputChunkCount(0)
    , NodeDirectory(New<NNodeTrackerClient::TNodeDirectory>())
    , Spec(spec)
    , CachedPendingJobCount(0)
    , CachedNeededResources(ZeroNodeResources())
    , InputChunkScratcher(New<TInputChunkScratcher>(this))
{
    Logger.AddTag(Sprintf("OperationId: %s", ~ToString(operation->GetOperationId())));
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
        if (NChunkClient::ExtractOverwriteFlag(path.Attributes())) {
            table.Clear = true;
            table.Overwrite = true;
            table.LockMode = ELockMode::Exclusive;
        }

        table.Options->KeyColumns = path.Attributes().Find< std::vector<Stroka> >("sorted_by");
        if (table.Options->KeyColumns) {
            if (!IsSortedOutputSupported()) {
                THROW_ERROR_EXCEPTION("Sorted outputs are not supported");
            } else {
                table.Clear = true;
                table.LockMode = ELockMode::Exclusive;
            }
        }

        OutputTables.push_back(table);
    }

    if (InputTables.size() > Config->MaxInputTableCount) {
        THROW_ERROR_EXCEPTION(
            "Too many input tables: maximum allowed %d, actual %" PRISZT,
            Config->MaxInputTableCount,
            InputTables.size());
    }

    if (OutputTables.size() > Config->MaxOutputTableCount) {
        THROW_ERROR_EXCEPTION(
            "Too many output tables: maximum allowed %d, actual %" PRISZT,
            Config->MaxOutputTableCount,
            OutputTables.size());
    }

    if (Host->GetExecNodes().empty()) {
        THROW_ERROR_EXCEPTION("No online exec nodes to start operation");
    }

    DoInitialize();

    LOG_INFO("Operation initialized");
}

void TOperationControllerBase::DoInitialize()
{
    Operation->SetMaxStdErrCount(Spec->MaxStdErrCount.Get(Config->MaxStdErrCount));
}

TFuture<TError> TOperationControllerBase::Prepare()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto this_ = MakeStrong(this);
    auto pipeline = StartAsyncPipeline(CancelableBackgroundInvoker)
        ->Add(BIND(&TThis::GetObjectIds, this_))
        ->Add(BIND(&TThis::OnObjectIdsReceived, this_))
        ->Add(BIND(&TThis::GetInputTypes, this_))
        ->Add(BIND(&TThis::OnInputTypesReceived, this_))
        ->Add(BIND(&TThis::RequestInputs, this_))
        ->Add(BIND(&TThis::OnInputsReceived, this_))
        ->Add(BIND(&TThis::CreateLivePreviewTables, this_))
        ->Add(BIND(&TThis::OnLivePreviewTablesCreated, this_))
        ->Add(BIND(&TThis::PrepareLivePreviewTablesForUpdate, this_))
        ->Add(BIND(&TThis::OnLivePreviewTablesPreparedForUpdate, this_))
        ->Add(BIND(&TThis::CollectTotals, this_));
     pipeline = CustomizePreparationPipeline(pipeline);
     pipeline = pipeline->Add(BIND(&TThis::CompletePreparation, this_));
     return pipeline
        ->Run()
        .Apply(BIND([=] (TValueOrError<void> result) -> TError {
            if (result.IsOK()) {
                this_->Running = true;
            }
            return result;
        }));
}

void TOperationControllerBase::SaveSnapshot(TOutputStream* stream)
{
    *stream << "Hello! This is operation " << ToString(Operation->GetOperationId()) << ". Now is "
        << ToString(TInstant::Now());
}

TFuture<TError> TOperationControllerBase::Revive(TInputStream* stream)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    UNUSED(stream);

    try {
        Initialize();
    } catch (const std::exception& ex) {
        return MakeFuture(TError(ex));
    }

    return Prepare();
}

TFuture<TError> TOperationControllerBase::Commit()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto this_ = MakeStrong(this);
    return StartAsyncPipeline(CancelableBackgroundInvoker)
        ->Add(BIND(&TThis::CommitResults, this_))
        ->Add(BIND(&TThis::OnResultsCommitted, this_))
        ->Run()
        .Apply(BIND([] (TValueOrError<void> result) -> TError {
            return result;
        }));
}

void TOperationControllerBase::OnJobRunning(TJobPtr job, const TJobStatus& status)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    UNUSED(job);
    UNUSED(status);
}

void TOperationControllerBase::OnJobStarted(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    UNUSED(job);

    JobCounter.Start(1);
}

void TOperationControllerBase::OnJobCompleted(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    JobCounter.Completed(1);

    auto joblet = GetJoblet(job);

    auto& result = joblet->Job->Result();
    const auto& schedulerResultEx = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    // Populate node directory by adding additional nodes returned from the job.
    NodeDirectory->MergeFrom(schedulerResultEx.node_directory());
    joblet->Task->OnJobCompleted(joblet);

    RemoveJoblet(job);

    OnTaskUpdated(joblet->Task);

    if (JobCounter.GetRunning() == 0 && GetPendingJobCount() == 0) {
        OnOperationCompleted();
    }
}

void TOperationControllerBase::OnJobFailed(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    const auto& result = job->Result();
    const auto& schedulerResultExt = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    // If some input chunks have failed then the job is considered aborted rather than failed.
    if (schedulerResultExt.failed_chunk_ids_size() > 0) {
        job->SetState(EJobState::Aborted);
        OnJobAborted(job);
        FOREACH (const auto& chunkId, schedulerResultExt.failed_chunk_ids()) {
            OnChunkFailed(FromProto<TChunkId>(chunkId));
        }
        return;
    }

    JobCounter.Failed(1);

    auto joblet = GetJoblet(job);
    joblet->Task->OnJobFailed(joblet);

    RemoveJoblet(job);

    auto error = FromProto(job->Result().error());
    if (error.Attributes().Get<bool>("fatal", false)) {
        OnOperationFailed(error);
        return;
    }

    int failedJobCount = JobCounter.GetFailed();
    int maxFailedJobCount = Spec->MaxFailedJobCount.Get(Config->MaxFailedJobCount);
    if (failedJobCount >= maxFailedJobCount) {
        OnOperationFailed(TError("Failed jobs limit %d has been reached",
            maxFailedJobCount));
        return;
    }
}

void TOperationControllerBase::OnJobAborted(TJobPtr job)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    JobCounter.Aborted(1);

    auto joblet = GetJoblet(job);
    joblet->Task->OnJobAborted(joblet);

    RemoveJoblet(job);
}

void TOperationControllerBase::TTask::OnJobLost(TCompleteJobPtr completedJob)
{
    YCHECK(LostJobCookieMap.insert(std::make_pair(
        completedJob->OutputCookie,
        completedJob->InputCookie)).second);
}

void TOperationControllerBase::OnChunkFailed(const TChunkId& chunkId)
{
    auto it = InputChunks.find(chunkId);
    if (it == InputChunks.end()) {
        LOG_WARNING("Intermediate chunk %s has failed", ~ToString(chunkId));
        OnIntermediateChunkUnavailable(chunkId);
    } else {
        LOG_WARNING("Input chunk %s has failed", ~ToString(chunkId));
        OnInputChunkUnavailable(chunkId, it->second);
    }
}

void TOperationControllerBase::OnInputChunkAvailable(const TChunkId& chunkId, TInputChunkDescriptor& descriptor, const TChunkReplicaList& replicas)
{
    if (descriptor.State != EInputChunkState::Waiting)
        return;

    LOG_TRACE("Input chunk is available (ChunkId: %s)", ~ToString(chunkId));

    --UnavailableInputChunkCount;
    YCHECK(UnavailableInputChunkCount >= 0);

    // Update replicas in place for all input chunks with current chunkId.
    FOREACH(auto& chunkSpec, descriptor.ChunkSpecs) {
        chunkSpec->mutable_replicas()->Clear();
        ToProto(chunkSpec->mutable_replicas(), replicas);
    }

    descriptor.State = EInputChunkState::Active;

    FOREACH(const auto& inputStripe, descriptor.InputStripes) {
        --inputStripe.Stripe->WaitingChunkCount;
        if (inputStripe.Stripe->WaitingChunkCount > 0)
            continue;

        auto task = inputStripe.Task;
        task->GetChunkPoolInput()->Resume(inputStripe.Cookie, inputStripe.Stripe);
        if (task->HasInputLocality()) {
            AddTaskLocalityHint(task, inputStripe.Stripe);
        }
        AddTaskPendingHint(task);
    }

}

void TOperationControllerBase::OnInputChunkUnavailable(const TChunkId& chunkId, TInputChunkDescriptor& descriptor)
{
    if (descriptor.State != EInputChunkState::Active)
        return;

    LOG_TRACE("Input chunk is unavailable (ChunkId: %s)", ~ToString(chunkId));

    ++UnavailableInputChunkCount;

    switch (Spec->UnavailableChunkTactics) {
        case EUnavailableChunkAction::Fail:
            OnOperationFailed(TError("Input chunk %s is unavailable",
                ~ToString(chunkId)));
            break;

        case EUnavailableChunkAction::Skip: {
            descriptor.State = EInputChunkState::Skipped;
            FOREACH(const auto& inputStripe, descriptor.InputStripes) {
                inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);

                // Remove given chunk from the stripe list.
                TSmallVector<TChunkSlicePtr, 1> slices;
                std::swap(inputStripe.Stripe->ChunkSlices, slices);

                std::copy_if(
                    slices.begin(),
                    slices.end(),
                    inputStripe.Stripe->ChunkSlices.begin(),
                    [&] (TChunkSlicePtr slice) {
                        return chunkId != FromProto<TChunkId>(slice->GetChunkSpec()->chunk_id());
                    });

                // Reinstall patched stripe.
                inputStripe.Task->GetChunkPoolInput()->Resume(inputStripe.Cookie, inputStripe.Stripe);
                AddTaskPendingHint(inputStripe.Task);
            }
            InputChunkScratcher->Start();
            break;
        }

        case EUnavailableChunkAction::Wait: {
            descriptor.State = EInputChunkState::Waiting;
            FOREACH(const auto& inputStripe, descriptor.InputStripes) {
                if (inputStripe.Stripe->WaitingChunkCount == 0) {
                    inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);
                }
                ++inputStripe.Stripe->WaitingChunkCount;
            }
            InputChunkScratcher->Start();
            break;
        }

        default:
            YUNREACHABLE();
    }
}

void TOperationControllerBase::OnIntermediateChunkUnavailable(const TChunkId& chunkId)
{
    auto it = ChunkOriginMap.find(chunkId);
    YCHECK(it != ChunkOriginMap.end());
    auto completedJob = it->second;
    if (completedJob->IsLost)
        return;

    LOG_INFO("Job is lost (Address: %s, JobId: %s, SourceTask: %s, OutputCookie: %d, InputCookie: %d)",
        ~completedJob->ExecNode->GetAddress(),
        ~ToString(completedJob->JobId),
        ~completedJob->SourceTask->GetId(),
        completedJob->OutputCookie,
        completedJob->InputCookie);

    JobCounter.Lost(1);
    completedJob->IsLost = true;
    completedJob->DestinationPool->Suspend(completedJob->InputCookie);
    completedJob->SourceTask->GetChunkPoolOutput()->Lost(completedJob->OutputCookie);
    completedJob->SourceTask->OnJobLost(completedJob);
    AddTaskPendingHint(completedJob->SourceTask);
}

bool TOperationControllerBase::IsOutputLivePreviewSupported() const
{
    return false;
}

bool TOperationControllerBase::IsIntermediateLivePreviewSupported() const
{
    return false;
}

void TOperationControllerBase::Abort()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Aborting operation");

    Running = false;
    InputChunkScratcher->Stop();
    CancelableContext->Cancel();

    LOG_INFO("Operation aborted");
}

void TOperationControllerBase::OnNodeOnline(TExecNodePtr node)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    UNUSED(node);
}

void TOperationControllerBase::OnNodeOffline(TExecNodePtr node)
{
    VERIFY_THREAD_AFFINITY(ControlThread);
    UNUSED(node);
}

TJobPtr TOperationControllerBase::ScheduleJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (!Running ||
        Operation->GetState() != EOperationState::Running ||
        Operation->GetSuspended())
    {
        LOG_TRACE("Operation is not running, scheduling request ignored");
        return nullptr;
    }

    if (GetPendingJobCount() == 0) {
        LOG_TRACE("No pending jobs left, scheduling request ignored");
        return nullptr;
    }

    auto job = DoScheduleJob(context, jobLimits);
    if (!job) {
        return nullptr;
    }

    OnJobStarted(job);

    return job;
}

void TOperationControllerBase::CustomizeJoblet(TJobletPtr joblet)
{
    UNUSED(joblet);
}

void TOperationControllerBase::CustomizeJobSpec(TJobletPtr joblet, TJobSpec* jobSpec)
{
    UNUSED(joblet);
    UNUSED(jobSpec);
}

void TOperationControllerBase::RegisterTaskGroup(TTaskGroup* group)
{
    TaskGroups.push_back(group);
}

void TOperationControllerBase::OnTaskUpdated(TTaskPtr task)
{
    int oldJobCount = CachedPendingJobCount;
    int newJobCount = CachedPendingJobCount + task->GetPendingJobCountDelta();
    CachedPendingJobCount = newJobCount;

    CachedNeededResources += task->GetTotalNeededResourcesDelta();

    LOG_DEBUG_IF(newJobCount != oldJobCount, "Pending job count updated (JobCount: %d -> %d, NeededResources: {%s})",
        oldJobCount,
        newJobCount,
        ~FormatResources(CachedNeededResources));

    task->CheckCompleted();
}

void TOperationControllerBase::MoveTaskToCandidates(
    TTaskPtr task,
    std::multimap<i64, TTaskPtr>& candidateTasks)
{
    const auto& neededResources = task->GetMinNeededResources();
    task->CheckResourceDemandSanity(neededResources);
    i64 minMemory = neededResources.memory();
    candidateTasks.insert(std::make_pair(minMemory, task));
    LOG_DEBUG("Task moved to candidates (Task: %s, MinMemory: %" PRId64 ")",
        ~task->GetId(),
        minMemory);

}

void TOperationControllerBase::AddTaskPendingHint(TTaskPtr task)
{
    if (task->GetPendingJobCount() > 0) {
        auto* group = task->GetGroup();
        if (group->NonLocalTasks.insert(task).second) {
            LOG_DEBUG("Task pending hint added (Task: %s)", ~task->GetId());
            MoveTaskToCandidates(task, group->CandidateTasks);
        }
    }
    OnTaskUpdated(task);
}

void TOperationControllerBase::DoAddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    auto* group = task->GetGroup();
    if (group->LocalTasks[address].insert(task).second) {
        LOG_TRACE("Task locality hint added (Task: %s, Address: %s)",
            ~task->GetId(),
            ~address);
    }
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, const Stroka& address)
{
    DoAddTaskLocalityHint(task, address);
    OnTaskUpdated(task);
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, TChunkStripePtr stripe)
{
    FOREACH (const auto& chunkSlice, stripe->ChunkSlices) {
        FOREACH (ui32 protoReplica, chunkSlice->GetChunkSpec()->replicas()) {
            auto replica = FromProto<NChunkClient::TChunkReplica>(protoReplica);

            if (chunkSlice->GetLocality(replica.GetIndex()) > 0) {
                const auto& descriptor = NodeDirectory->GetDescriptor(replica);
                DoAddTaskLocalityHint(task, descriptor.Address);
            }
        }
    }
    OnTaskUpdated(task);
}

void TOperationControllerBase::ResetTaskLocalityDelays()
{
    LOG_DEBUG("Task locality delays are reset");
    FOREACH (auto* group, TaskGroups) {
        FOREACH (const auto& pair, group->DelayedTasks) {
            auto task = pair.second;
            if (task->GetPendingJobCount() > 0) {
                MoveTaskToCandidates(task, group->CandidateTasks);
            }
        }
        group->DelayedTasks.clear();
    }
}

bool TOperationControllerBase::CheckJobLimits(TExecNodePtr node, TTaskPtr task, const TNodeResources& jobLimits)
{
    auto neededResources = task->GetMinNeededResources();
    if (Dominates(jobLimits, neededResources)) {
        return true;
    }
    task->CheckResourceDemandSanity(node, neededResources);
    return false;
}

TJobPtr TOperationControllerBase::DoScheduleJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    auto localJob = DoScheduleLocalJob(context, jobLimits);
    if (localJob) {
        return localJob;
    }

    auto nonLocalJob = DoScheduleNonLocalJob(context, jobLimits);
    if (nonLocalJob) {
        return nonLocalJob;
    }

    return nullptr;
}

TJobPtr TOperationControllerBase::DoScheduleLocalJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    auto node = context->GetNode();
    const auto& address = node->GetAddress();

    FOREACH (auto* group, TaskGroups) {
        if (!Dominates(jobLimits, group->MinNeededResources)) {
            continue;
        }

        auto localTasksIt = group->LocalTasks.find(address);
        if (localTasksIt == group->LocalTasks.end()) {
            continue;
        }

        i64 bestLocality = 0;
        TTaskPtr bestTask = nullptr;

        auto& localTasks = localTasksIt->second;
        auto it = localTasks.begin();
        while (it != localTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            // Make sure that the task have positive locality.
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

            if (task->GetPendingJobCount() == 0) {
                OnTaskUpdated(task);
                continue;
            }

            if (!CheckJobLimits(node, task, jobLimits)) {
                continue;
            }

            bestLocality = locality;
            bestTask = task;
        }

        if (!Running) {
            return nullptr;
        }

        if (bestTask) {
            LOG_DEBUG(
                "Attempting to schedule a local job (Task: %s, Address: %s, Locality: %" PRId64 ", JobLimits: {%s}, "
                "PendingDataSize: %" PRId64 ", PendingJobCount: %d)",
                ~bestTask->GetId(),
                ~address,
                bestLocality,
                ~FormatResources(jobLimits),
                bestTask->GetPendingDataSize(),
                bestTask->GetPendingJobCount());
            auto job = bestTask->ScheduleJob(context, jobLimits);
            if (job) {
                bestTask->SetDelayedTime(Null);
                OnTaskUpdated(bestTask);
                return job;
            }
        }
    }
    return nullptr;
}

TJobPtr TOperationControllerBase::DoScheduleNonLocalJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    auto now = TInstant::Now();
    const auto& node = context->GetNode();
    const auto& address = node->GetAddress();

    FOREACH (auto* group, TaskGroups) {
        if (!Dominates(jobLimits, group->MinNeededResources)) {
            continue;
        }

        auto& nonLocalTasks = group->NonLocalTasks;
        auto& candidateTasks = group->CandidateTasks;
        auto& delayedTasks = group->DelayedTasks;

        // Move tasks from delayed to candidates.
        while (!delayedTasks.empty()) {
            auto it = delayedTasks.begin();
            auto deadline = it->first;
            if (now < deadline) {
                break;
            }
            auto task = it->second;
            delayedTasks.erase(it);
            if (task->GetPendingJobCount() == 0) {
                LOG_DEBUG("Task pending hint removed (Task: %s)",
                    ~task->GetId());
                YCHECK(nonLocalTasks.erase(task) == 1);
                OnTaskUpdated(task);
            } else {
                LOG_DEBUG("Task delay deadline reached (Task: %s)", ~task->GetId());
                MoveTaskToCandidates(task, candidateTasks);
            }
        }

        // Consider candidates in the order of increasing memory demand.
        {
            auto it = candidateTasks.begin();
            while (it != candidateTasks.end()) {
                auto task = it->second;

                // Check min memory demand for early exit.
                if (task->GetMinNeededResources().memory() > jobLimits.memory()) {
                    break;
                }

                // Make sure that the task is ready to launch jobs.
                // Remove pending hint if not.
                if (task->GetPendingJobCount() == 0) {
                    LOG_DEBUG("Task pending hint removed (Task: %s)", ~task->GetId());
                    candidateTasks.erase(it++);
                    YCHECK(nonLocalTasks.erase(task) == 1);
                    OnTaskUpdated(task);
                    continue;
                }

                if (!CheckJobLimits(node, task, jobLimits)) {
                    ++it;
                    continue;
                }

                if (!task->GetDelayedTime()) {
                    task->SetDelayedTime(now);
                }

                auto deadline = task->GetDelayedTime().Get() + task->GetLocalityTimeout();
                if (deadline > now) {
                    LOG_DEBUG("Task delayed (Task: %s, Deadline: %s)",
                        ~task->GetId(),
                        ~ToString(deadline));
                    delayedTasks.insert(std::make_pair(deadline, task));
                    candidateTasks.erase(it++);
                    continue;
                }

                if (!Running) {
                    return nullptr;
                }

                LOG_DEBUG(
                    "Attempting to schedule a non-local job (Task: %s, Address: %s, JobLimits: {%s}, "
                    "PendingDataSize: %" PRId64 ", PendingJobCount: %d)",
                    ~task->GetId(),
                    ~address,
                    ~FormatResources(jobLimits),
                    task->GetPendingDataSize(),
                    task->GetPendingJobCount());

                auto job = task->ScheduleJob(context, jobLimits);
                if (job) {
                    OnTaskUpdated(task);
                    return job;
                }

                // If task failed to schedule job, its min resources might have been updated.
                auto minMemory = task->GetMinNeededResources().memory();
                if (it->first == minMemory) {
                    ++it;
                } else {
                    it = candidateTasks.erase(it);
                    candidateTasks.insert(std::make_pair(minMemory, task));
                }
            }
        }
    }
    return nullptr;
}

TCancelableContextPtr TOperationControllerBase::GetCancelableContext()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableContext;
}

IInvokerPtr TOperationControllerBase::GetCancelableControlInvoker()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableControlInvoker;
}

IInvokerPtr TOperationControllerBase::GetCancelableBackgroundInvoker()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableBackgroundInvoker;
}

int TOperationControllerBase::GetPendingJobCount()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    // NB: For suspended operations we still report proper pending job count
    // but zero demand.
    if (Operation->GetState() != EOperationState::Running) {
        return 0;
    }

    return CachedPendingJobCount;
}

TNodeResources TOperationControllerBase::GetNeededResources()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    if (Operation->GetState() != EOperationState::Running) {
        return ZeroNodeResources();
    }

    return CachedNeededResources;
}

void TOperationControllerBase::OnOperationCompleted()
{
    VERIFY_THREAD_AFFINITY_ANY();

    CancelableControlInvoker->Invoke(BIND(&TThis::DoOperationCompleted, MakeStrong(this)));
}

void TOperationControllerBase::DoOperationCompleted()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Operation completed");

    JobCounter.Finalize();

    InputChunkScratcher->Stop();
    Running = false;

    Host->OnOperationCompleted(Operation);
}

void TOperationControllerBase::OnOperationFailed(const TError& error)
{
    VERIFY_THREAD_AFFINITY_ANY();

    CancelableControlInvoker->Invoke(BIND(&TThis::DoOperationFailed, MakeStrong(this), error));
}

void TOperationControllerBase::DoOperationFailed(const TError& error)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    InputChunkScratcher->Stop();
    Running = false;

    Host->OnOperationFailed(Operation, error);
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::CommitResults()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Committing results");

    TObjectServiceProxy proxy(AuthenticatedMasterChannel);
    auto batchReq = proxy.ExecuteBatch();

    FOREACH (auto& table, OutputTables) {
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

            auto addChunkTree = [&] (const NChunkServer::TChunkTreeId chunkTreeId) {
                if (!req) {
                    req = TChunkListYPathProxy::Attach(FromObjectId(table.OutputChunkListId));
                    NMetaState::GenerateMutationId(req);
                }
                ToProto(req->add_children_ids(), chunkTreeId);
                ++reqSize;
                if (reqSize >= Config->MaxChildrenPerAttachRequest) {
                    flushReq();
                }
            };

            if (table.Options->KeyColumns && IsSortedOutputSupported()) {
                // Sorted output generated by user operation requires rearranging.
                YCHECK(table.Endpoints.size() % 2 == 0);

                LOG_DEBUG("Sorting %d endpoints", static_cast<int>(table.Endpoints.size()));
                std::sort(
                    table.Endpoints.begin(),
                    table.Endpoints.end(),
                    [=] (const TOutputTable::TEndpoint& lhs, const TOutputTable::TEndpoint& rhs) -> bool {
                        // First sort by keys.
                        // Then sort by ChunkTreeKeys.
                        auto keysResult = NChunkClient::NProto::CompareKeys(lhs.Key, rhs.Key);
                        if (keysResult != 0) {
                            return keysResult < 0;
                        }
                        return (lhs.ChunkTreeKey - rhs.ChunkTreeKey) < 0;
                });

                auto outputCount = static_cast<int>(table.Endpoints.size()) / 2;
                for (int outputIndex = 0; outputIndex < outputCount; ++outputIndex) {
                    auto& leftEndpoint = table.Endpoints[2 * outputIndex];
                    auto& rightEndpoint = table.Endpoints[2 * outputIndex + 1];
                    if (leftEndpoint.ChunkTreeKey != rightEndpoint.ChunkTreeKey) {
                        auto error = TError("Output table %s is not sorted: job outputs have overlapping key ranges",
                            ~table.Path.GetPath());

                        LOG_DEBUG(error);
                        THROW_ERROR error;
                    }

                    auto pair = table.OutputChunkTreeIds.equal_range(leftEndpoint.ChunkTreeKey);
                    auto it = pair.first;
                    addChunkTree(it->second);
                    // In user operations each ChunkTreeKey corresponds to a single OutputChunkTreeId.
                    // Let's check it.
                    YCHECK(++it == pair.second);
                }
            } else {
                FOREACH (const auto& pair, table.OutputChunkTreeIds) {
                    addChunkTree(pair.second);
                }
            }

            flushReq();
        }

        if (table.Options->KeyColumns) {
            LOG_INFO("Table %s will be marked as sorted by %s",
                ~table.Path.GetPath(),
                ~ConvertToYsonString(table.Options->KeyColumns.Get(), EYsonFormat::Text).Data());
            auto req = TTableYPathProxy::SetSorted(path);
            ToProto(req->mutable_key_columns(), table.Options->KeyColumns.Get());
            SetTransactionId(req, Operation->GetOutputTransaction());
            NMetaState::GenerateMutationId(req);
            batchReq->AddRequest(req, "set_out_sorted");
        }
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnResultsCommitted(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(batchRsp->GetCumulativeError(), "Error committing results");

    LOG_INFO("Results committed");
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::CreateLivePreviewTables()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    // NB: use root credentials.
    TObjectServiceProxy proxy(Host->GetMasterChannel());
    auto batchReq = proxy.ExecuteBatch();

    auto processTable = [&] (
            const Stroka& path,
            int replicationFactor,
            const Stroka& key) {
        auto req = TCypressYPathProxy::Create(path);

        req->set_type(EObjectType::Table);
        req->set_ignore_existing(true);

        auto attributes = CreateEphemeralAttributes();
        attributes->Set("replication_factor", replicationFactor);

        ToProto(req->mutable_node_attributes(), *attributes);

        batchReq->AddRequest(req, key);
    };

    LOG_INFO("Creating output tables for live preview");
    if (IsOutputLivePreviewSupported()) {
        LOG_INFO("Creating output tables for live preview");

        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            const auto& table = OutputTables[index];
            auto path = GetLivePreviewOutputPath(Operation->GetOperationId(), index);
            processTable(path, table.Options->ReplicationFactor, "create_output");
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        LOG_INFO("Creating intermediate table for live preview");

        auto path = GetLivePreviewIntermediatePath(Operation->GetOperationId());
        processTable(path, 1, "create_intermediate");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnLivePreviewTablesCreated(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(batchRsp->GetCumulativeError(), "Error creating live preview tables");

    auto processTable = [&] (TLivePreviewTableBase& table, TCypressYPathProxy::TRspCreatePtr rsp) {
        table.LivePreviewTableId = FromProto<NCypressClient::TNodeId>(rsp->node_id());
    };

    if (IsOutputLivePreviewSupported()) {
        auto rsps = batchRsp->GetResponses<TCypressYPathProxy::TRspCreate>("create_output");
        YCHECK(rsps.size() == OutputTables.size());
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            processTable(OutputTables[index], rsps[index]);
        }

        LOG_INFO("Output live preview tables created");
    }

    if (IsIntermediateLivePreviewSupported()) {
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCreate>("create_intermediate");
        processTable(IntermediateTable, rsp);

        LOG_INFO("Intermediate live preview table created");
    }
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::PrepareLivePreviewTablesForUpdate()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    // NB: use root credentials.
    TObjectServiceProxy proxy(Host->GetMasterChannel());
    auto batchReq = proxy.ExecuteBatch();

    auto processTable = [&] (const TLivePreviewTableBase& table, const Stroka& key) {
        auto req = TTableYPathProxy::PrepareForUpdate(FromObjectId(table.LivePreviewTableId));
        req->set_mode(EUpdateMode::Overwrite);
        SetTransactionId(req, Operation->GetAsyncSchedulerTransaction());
        batchReq->AddRequest(req, key);
    };

    if (IsOutputLivePreviewSupported()) {
        LOG_INFO("Preparing live preview output tables for update");

        FOREACH (const auto& table, OutputTables) {
            processTable(table, "prepare_output");
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        LOG_INFO("Preparing live preview intermediate table for update");

        processTable(IntermediateTable, "prepare_intermediate");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnLivePreviewTablesPreparedForUpdate(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(batchRsp->GetCumulativeError(), "Error preparing live preview tables for update");

    auto processTable = [&] (TLivePreviewTableBase& table, TTableYPathProxy::TRspPrepareForUpdatePtr rsp) {
        table.LivePreviewChunkListId = FromProto<NCypressClient::TNodeId>(rsp->chunk_list_id());
    };

    if (IsOutputLivePreviewSupported()) {
        auto rsps = batchRsp->GetResponses<TTableYPathProxy::TRspPrepareForUpdate>("prepare_output");
        YCHECK(rsps.size() == OutputTables.size());
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            processTable(OutputTables[index], rsps[index]);
        }

        LOG_INFO("Output live preview tables prepared for update");
    }

    if (IsIntermediateLivePreviewSupported()) {
        auto rsp = batchRsp->GetResponse<TTableYPathProxy::TRspPrepareForUpdate>("prepare_intermediate");
        processTable(IntermediateTable, rsp);

        LOG_INFO("Intermediate live preview table prepared for update");
    }
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::GetObjectIds()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Getting object ids");

    TObjectServiceProxy proxy(AuthenticatedMasterChannel);
    auto batchReq = proxy.ExecuteBatch();

    FOREACH (const auto& table, InputTables) {
        auto req = TObjectYPathProxy::GetId(table.Path.GetPath());
        SetTransactionId(req, Operation->GetInputTransaction());
        batchReq->AddRequest(req, "get_in_id");
    }

    FOREACH (const auto& table, OutputTables) {
        auto req = TObjectYPathProxy::GetId(table.Path.GetPath());
        SetTransactionId(req, Operation->GetInputTransaction());
        batchReq->AddRequest(req, "get_out_id");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnObjectIdsReceived(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error getting ids for input objects");

    {
        auto getInIdRsps = batchRsp->GetResponses<TObjectYPathProxy::TRspGetId>("get_in_id");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            {
                auto rsp = getInIdRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting id for input table %s",
                    ~table.Path.GetPath());
                table.ObjectId = FromProto<TObjectId>(rsp->object_id());
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
                table.ObjectId = FromProto<TObjectId>(rsp->object_id());
            }
        }
    }

    LOG_INFO("Object ids received");
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::GetInputTypes()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Getting input object types");

    TObjectServiceProxy proxy(AuthenticatedMasterChannel);
    auto batchReq = proxy.ExecuteBatch();

    FOREACH (const auto& table, InputTables) {
        auto req = TObjectYPathProxy::Get(FromObjectId(table.ObjectId) + "/@type");
        SetTransactionId(req, Operation->GetInputTransaction());
        batchReq->AddRequest(req, "get_input_types");
    }

    FOREACH (const auto& table, OutputTables) {
        auto req = TObjectYPathProxy::Get(FromObjectId(table.ObjectId) + "/@type");
        SetTransactionId(req, Operation->GetInputTransaction());
        batchReq->AddRequest(req, "get_output_types");
    }

    FOREACH (const auto& pair, GetFilePaths()) {
        const auto& path = pair.first;
        auto req = TObjectYPathProxy::Get(path.GetPath() + "/@type");
        SetTransactionId(req, Operation->GetInputTransaction());
        batchReq->AddRequest(req, "get_file_types");
    }

    return batchReq->Invoke();
}

void TOperationControllerBase::OnInputTypesReceived(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp)
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    THROW_ERROR_EXCEPTION_IF_FAILED(*batchRsp, "Error getting input object types");

    {
        auto getInputTypes = batchRsp->GetResponses<TObjectYPathProxy::TRspGet>("get_input_types");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            auto path = table.Path.GetPath();
            auto rsp = getInputTypes[index];
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting type for input %s",
                ~path);

            auto type = ConvertTo<EObjectType>(TYsonString(rsp->value()));
            if (type != EObjectType::Table) {
                THROW_ERROR_EXCEPTION("Object %s has invalid type: expected %s, actual %s",
                    ~path,
                    ~FormatEnum(EObjectType(EObjectType::Table)).Quote(),
                    ~FormatEnum(type).Quote());
            }
        }
    }

    {
        auto getOutputTypes = batchRsp->GetResponses<TObjectYPathProxy::TRspGet>("get_output_types");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            auto path = table.Path.GetPath();
            auto rsp = getOutputTypes[index];
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting type for output %s",
                ~path);

            auto type = ConvertTo<EObjectType>(TYsonString(rsp->value()));
            if (type != EObjectType::Table) {
                THROW_ERROR_EXCEPTION("Object %s has invalid type: expected %s, actual %s",
                    ~path,
                    ~FormatEnum(EObjectType(EObjectType::Table)).Quote(),
                    ~FormatEnum(type).Quote());
            }
        }
    }

    {
        auto paths = GetFilePaths();
        auto getFileTypes = batchRsp->GetResponses<TObjectYPathProxy::TRspGet>("get_file_types");
        for (int index = 0; index < static_cast<int>(paths.size()); ++index) {
            auto richPath = paths[index].first;
            auto path = richPath.GetPath();
            auto stage = paths[index].second;
            auto rsp = getFileTypes[index];
            THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting type for file %s",
                ~path);

            auto type = ConvertTo<EObjectType>(TYsonString(rsp->value()));
            TUserFile* file;
            switch (type) {
                case EObjectType::File:
                    RegularFiles.push_back(TRegularUserFile());
                    file = &RegularFiles.back();
                    break;
                case EObjectType::Table:
                    TableFiles.push_back(TUserTableFile());
                    file = &TableFiles.back();
                    break;
                default:
                    THROW_ERROR_EXCEPTION("Object %s has invalid type: expected %s or %s, actual %s",
                        ~path,
                        ~FormatEnum(EObjectType(EObjectType::File)).Quote(),
                        ~FormatEnum(EObjectType(EObjectType::Table)).Quote(),
                        ~FormatEnum(type).Quote());
            }
            file->Stage = stage;
            file->Path = richPath;
        }
    }

    LOG_INFO("Input types received");
}

TObjectServiceProxy::TInvExecuteBatch TOperationControllerBase::RequestInputs()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    LOG_INFO("Requesting inputs");

    TObjectServiceProxy proxy(AuthenticatedMasterChannel);
    auto batchReq = proxy.ExecuteBatch();

    FOREACH (const auto& table, InputTables) {
        auto path = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(path);
            req->set_mode(ELockMode::Snapshot);
            SetTransactionId(req, Operation->GetInputTransaction());
            NMetaState::GenerateMutationId(req);
            batchReq->AddRequest(req, "lock_in");
        }
        {
            auto attributes = table.Path.Attributes().Clone();
            if (table.ComplementFetch) {
                attributes->Set("complement", !attributes->Get("complement", false));
            }
            auto req = TTableYPathProxy::Fetch(path);
            req->set_fetch_all_meta_extensions(true);
            ToProto(req->mutable_attributes(), *attributes);
            SetTransactionId(req, Operation->GetInputTransaction());
            batchReq->AddRequest(req, "fetch_in");
        }
        {
            auto req = TYPathProxy::Get(path);
            TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
            attributeFilter.Keys.push_back("sorted");
            attributeFilter.Keys.push_back("sorted_by");
            ToProto(req->mutable_attribute_filter(), attributeFilter);
            SetTransactionId(req, Operation->GetInputTransaction());
            batchReq->AddRequest(req, "get_in_attributes");
        }
    }

    FOREACH (const auto& table, OutputTables) {
        auto path = FromObjectId(table.ObjectId);
        {
            auto req = TCypressYPathProxy::Lock(path);
            req->set_mode(table.LockMode);
            NMetaState::GenerateMutationId(req);
            SetTransactionId(req, Operation->GetOutputTransaction());
            batchReq->AddRequest(req, "lock_out");
        }
        {
            auto req = TYPathProxy::Get(path);
            TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
            attributeFilter.Keys.push_back("channels");
            attributeFilter.Keys.push_back("compression_codec");
            attributeFilter.Keys.push_back("erasure_codec");
            attributeFilter.Keys.push_back("row_count");
            attributeFilter.Keys.push_back("replication_factor");
            attributeFilter.Keys.push_back("account");
            attributeFilter.Keys.push_back("vital");
            ToProto(req->mutable_attribute_filter(), attributeFilter);
            SetTransactionId(req, Operation->GetOutputTransaction());
            batchReq->AddRequest(req, "get_out_attributes");
        }
        {
            auto req = TTableYPathProxy::PrepareForUpdate(path);
            SetTransactionId(req, Operation->GetOutputTransaction());
            NMetaState::GenerateMutationId(req);
            req->set_mode(table.Clear ? NChunkClient::EUpdateMode::Overwrite : NChunkClient::EUpdateMode::Append);
            batchReq->AddRequest(req, "prepare_for_update");
        }
    }

    FOREACH (const auto& file, RegularFiles) {
        auto path = file.Path.GetPath();
        {
            auto req = TCypressYPathProxy::Lock(path);
            req->set_mode(ELockMode::Snapshot);
            NMetaState::GenerateMutationId(req);
            SetTransactionId(req, Operation->GetInputTransaction());
            batchReq->AddRequest(req, "lock_regular_file");
        }
        {
            auto req = TYPathProxy::GetKey(path);
            SetTransactionId(req, Operation->GetInputTransaction()->GetId());
            batchReq->AddRequest(req, "get_regular_file_name");
        }
        {
            auto req = TYPathProxy::Get(path);
            SetTransactionId(req, Operation->GetOutputTransaction());
            TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
            attributeFilter.Keys.push_back("executable");
            attributeFilter.Keys.push_back("file_name");
            ToProto(req->mutable_attribute_filter(), attributeFilter);
            batchReq->AddRequest(req, "get_regular_file_attributes");
        }
        {
            auto req = TFileYPathProxy::Fetch(path);
            SetTransactionId(req, Operation->GetInputTransaction()->GetId());
            req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
            batchReq->AddRequest(req, "fetch_regular_file");
        }
    }

    FOREACH (const auto& file, TableFiles) {
        auto path = file.Path.GetPath();
        {
            auto req = TCypressYPathProxy::Lock(path);
            req->set_mode(ELockMode::Snapshot);
            NMetaState::GenerateMutationId(req);
            SetTransactionId(req, Operation->GetInputTransaction());
            batchReq->AddRequest(req, "lock_table_file");
        }
        {
            auto req = TTableYPathProxy::Fetch(path);
            req->set_fetch_all_meta_extensions(true);
            ToProto(req->mutable_attributes(), file.Path.Attributes());
            SetTransactionId(req, Operation->GetInputTransaction()->GetId());
            batchReq->AddRequest(req, "fetch_table_file_chunks");
        }
        {
            auto req = TYPathProxy::GetKey(path);
            SetTransactionId(req, Operation->GetInputTransaction()->GetId());
            batchReq->AddRequest(req, "get_table_file_name");
        }
        {
            auto req = TYPathProxy::Get(path + "/@uncompressed_data_size");
            SetTransactionId(req, Operation->GetInputTransaction()->GetId());
            batchReq->AddRequest(req, "get_table_file_size");
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
        auto getInAttributesRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_in_attributes");
        for (int index = 0; index < static_cast<int>(InputTables.size()); ++index) {
            auto& table = InputTables[index];
            auto path = table.Path.GetPath();
            {
                auto rsp = lockInRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error locking input table %s",
                    ~path);

                LOG_INFO("Input table %s locked",
                    ~path);
            }
            {
                auto rsp = fetchInRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error fetching input table %s",
                    ~path);

                NodeDirectory->MergeFrom(rsp->node_directory());

                table.FetchResponse = rsp;
                LOG_INFO("Input table %s has %d chunks",
                    ~path,
                    rsp->chunks_size());
            }
            {
                auto rsp = getInAttributesRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting attributes for input table %s",
                    ~path);

                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();

                if (attributes.Get<bool>("sorted")) {
                    table.KeyColumns = attributes.Get< std::vector<Stroka> >("sorted_by");
                    LOG_INFO("Input table %s is sorted by %s",
                        ~path,
                        ~ConvertToYsonString(table.KeyColumns.Get(), EYsonFormat::Text).Data());
                } else {
                    LOG_INFO("Input table %s is not sorted",
                        ~path);
                }
            }
        }
    }

    {
        auto lockOutRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_out");
        auto getOutAttributesRsps = batchRsp->GetResponses<TYPathProxy::TRspGet>("get_out_attributes");
        auto prepareForUpdateRsps = batchRsp->GetResponses<TTableYPathProxy::TRspPrepareForUpdate>("prepare_for_update");
        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto& table = OutputTables[index];
            auto path = table.Path.GetPath();
            {
                auto rsp = lockOutRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error locking output table %s",
                    ~path);

                LOG_INFO("Output table %s locked",
                    ~path);
            }
            {
                auto rsp = getOutAttributesRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting attributes for output table %s",
                    ~path);

                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();

                Deserialize(
                    table.Options->Channels,
                    ConvertToNode(attributes.GetYson("channels")));

                i64 initialRowCount = attributes.Get<i64>("row_count");
                if (initialRowCount > 0 && table.Clear && !table.Overwrite) {
                    THROW_ERROR_EXCEPTION("Output table %s must be empty (use \"overwrite\" attribute to force clearing it)",
                        ~table.Path.GetPath());
                }
                table.Options->CompressionCodec = attributes.Get<NCompression::ECodec>("compression_codec");
                table.Options->ErasureCodec = attributes.Get<NErasure::ECodec>("erasure_codec", NErasure::ECodec::None);
                table.Options->ReplicationFactor = attributes.Get<int>("replication_factor");
                table.Options->Account = attributes.Get<Stroka>("account");
                table.Options->ChunksVital = attributes.Get<bool>("vital");

                LOG_INFO("Output table %s attributes received (Options: %s)",
                    ~path,
                    ~ConvertToYsonString(table.Options, EYsonFormat::Text).Data());
            }
            {
                auto rsp = prepareForUpdateRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error preparing output table %s for update",
                    ~path);

                table.OutputChunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());
                LOG_INFO("Output table %s has output chunk list %s",
                    ~path,
                    ~ToString(table.OutputChunkListId));
            }
        }
    }

    {
        auto lockRegularFileRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_regular_file");
        auto fetchRegularFileRsps = batchRsp->GetResponses<TFileYPathProxy::TRspFetch>("fetch_regular_file");
        auto getRegularFileNameRsps = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_regular_file_name");
        auto getRegularFileAttributesRsps = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_regular_file_attributes");
        for (int index = 0; index < static_cast<int>(RegularFiles.size()); ++index) {
            auto& file = RegularFiles[index];
            auto path = file.Path.GetPath();
            {
                auto rsp = lockRegularFileRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error locking regular file %s",
                    ~path);

                LOG_INFO("Regular file %s locked",
                    ~path);
            }
            {
                auto rsp = getRegularFileNameRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    *rsp,
                    "Error getting file name for regular file %s",
                    ~path);

                file.FileName = rsp->value();
            }
            {
                auto rsp = getRegularFileAttributesRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting attributes for regular file %s",
                    ~path);

                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();

                file.FileName = attributes.Get<Stroka>("file_name", file.FileName);
                file.Executable = attributes.Get<bool>("executable", false);
            }
            {
                auto rsp = fetchRegularFileRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    *rsp,
                    "Error fetching regular file %s",
                    ~path);

                file.FetchResponse = rsp;
                LOG_INFO("File %s attributes received", ~path);
            }

            file.FileName = file.Path.Attributes().Get<Stroka>("file_name", file.FileName);
            file.Executable = file.Path.Attributes().Get<bool>("executable", file.Executable);
        }
    }

    {
        auto lockTableFileRsps = batchRsp->GetResponses<TCypressYPathProxy::TRspLock>("lock_table_file");
        auto getTableFileSizeRsps = batchRsp->GetResponses<TTableYPathProxy::TRspGet>("get_table_file_size");
        auto fetchTableFileRsps = batchRsp->GetResponses<TTableYPathProxy::TRspFetch>("fetch_table_file_chunks");
        auto getTableFileNameRsps = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_table_file_name");
        for (int index = 0; index < static_cast<int>(TableFiles.size()); ++index) {
            auto& file = TableFiles[index];
            auto path = file.Path.GetPath();
            {
                auto rsp = lockTableFileRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error locking table file %s",
                    ~path);

                LOG_INFO("Table file %s locked",
                    ~path);
            }
            {
                auto rsp = getTableFileSizeRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting table file size");
                i64 tableSize = ConvertTo<i64>(TYsonString(rsp->value()));
                if (tableSize > Config->MaxTableFileSize) {
                    THROW_ERROR_EXCEPTION(
                        "Table file %s exceeds the size limit: " PRId64 " > " PRId64,
                        ~path,
                        tableSize,
                        Config->MaxTableFileSize);
                }
            }
            {
                auto rsp = fetchTableFileRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error fetching table file chunks");
                NodeDirectory->MergeFrom(rsp->node_directory());
                file.FetchResponse = rsp;
            }
            {
                auto rsp = getTableFileNameRsps[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(*rsp, "Error getting table file name");
                auto key = ConvertTo<Stroka>(TYsonString(rsp->value()));
                file.FileName = file.Path.Attributes().Get<Stroka>("file_name", key);
                file.Format = file.Path.Attributes().GetYson("format");
            }
            {
                std::vector<TChunkId> chunkIds;
                FOREACH (const auto& chunk, file.FetchResponse->chunks()) {
                    chunkIds.push_back(FromProto<TChunkId>(chunk.chunk_id()));
                }
                LOG_INFO("Table file %s attributes received (FileName: %s, Format: %s, ChunkIds: [%s])",
                    ~path,
                    ~file.FileName,
                    ~file.Format.Data(),
                    ~JoinToString(chunkIds));
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

TFuture<void> TOperationControllerBase::CollectTotals()
{
    VERIFY_THREAD_AFFINITY(BackgroundThread);

    FOREACH (const auto& table, InputTables) {
        FOREACH (const auto& chunk, table.FetchResponse->chunks()) {
            i64 chunkDataSize;
            i64 chunkRowCount;
            i64 chunkValueCount;
            NChunkClient::GetStatistics(chunk, &chunkDataSize, &chunkRowCount, &chunkValueCount);

            TotalInputDataSize += chunkDataSize;
            TotalInputRowCount += chunkRowCount;
            TotalInputValueCount += chunkValueCount;
            ++TotalInputChunkCount;
        }
    }

    LOG_INFO("Input totals collected (ChunkCount: %d, DataSize: %" PRId64 ", RowCount: % " PRId64 ", ValueCount: %" PRId64 ")",
        TotalInputChunkCount,
        TotalInputDataSize,
        TotalInputRowCount,
        TotalInputValueCount);

    if (TotalInputChunkCount == 0) {
        LOG_INFO("Empty input");
        OnOperationCompleted();
        // Break initialization pipeline
        return NewPromise();
    }

    return MakeFuture();
}

TAsyncPipeline<void>::TPtr TOperationControllerBase::CustomizePreparationPipeline(TAsyncPipeline<void>::TPtr pipeline)
{
    return pipeline;
}

std::vector<TRefCountedChunkSpecPtr> TOperationControllerBase::CollectInputChunks() const
{
    std::vector<TRefCountedChunkSpecPtr> result;
    for (int tableIndex = 0; tableIndex < InputTables.size(); ++tableIndex) {
        const auto& table = InputTables[tableIndex];
        FOREACH (const auto& chunkSpec, table.FetchResponse->chunks()) {
            auto chunkId = FromProto<TChunkId>(chunkSpec.chunk_id());
            if (IsUnavailable(chunkSpec)) {
                switch (Spec->UnavailableChunkStrategy) {
                    case EUnavailableChunkAction::Fail:
                        THROW_ERROR_EXCEPTION("Input chunk %s is unavailable",
                            ~ToString(chunkId));

                    case EUnavailableChunkAction::Skip:
                        LOG_TRACE("Skipping unavailable chunk (ChunkId: %s)",
                            ~ToString(chunkId));
                        continue;

                    case EUnavailableChunkAction::Wait:
                        // Do nothing.
                        break;

                    default:
                        YUNREACHABLE();
                }
            }
            auto chunk = New<TRefCountedChunkSpec>(chunkSpec);
            chunk->set_table_index(tableIndex);
            result.push_back(chunk);
        }
    }
    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::SliceInputChunks(i64 maxSliceDataSize, int jobCount)
{
    std::vector<TChunkStripePtr> result;
    auto appendStripes = [&] (std::vector<TChunkSlicePtr> slices) {
        FOREACH(const auto& slice, slices) {
            result.push_back(New<TChunkStripe>(slice));
        }
    };

    i64 sliceDataSize = std::min(maxSliceDataSize, std::max(TotalInputDataSize / jobCount, (i64)1));

    FOREACH (const auto& chunkSpec, CollectInputChunks()) {
        int oldSize = result.size();

        bool hasNontrivialLimits =
            (chunkSpec->has_start_limit() && IsNontrivial(chunkSpec->start_limit())) ||
            (chunkSpec->has_end_limit() && IsNontrivial(chunkSpec->end_limit()));

        auto codecId = NErasure::ECodec(chunkSpec->erasure_codec());
        if (hasNontrivialLimits || codecId == NErasure::ECodec::None) {
            auto slices = CreateChunkSlice(chunkSpec)->SliceEvenly(sliceDataSize);
            appendStripes(slices);
        } else {
            FOREACH(const auto& slice, CreateErasureChunkSlices(chunkSpec, codecId)) {
                auto slices = slice->SliceEvenly(sliceDataSize);
                appendStripes(slices);
            }
        }

        LOG_TRACE("Slicing chunk (ChunkId: %s, SliceCount: %d)",
            ~ToString(FromProto<TChunkId>(chunkSpec->chunk_id())),
            static_cast<int>(result.size() - oldSize));
    }
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

bool TOperationControllerBase::IsSortedOutputSupported() const
{
    return false;
}

void TOperationControllerBase::RegisterOutput(
    const NChunkServer::TChunkTreeId& chunkTreeId,
    int key,
    int tableIndex,
    TOutputTable& table)
{
    table.OutputChunkTreeIds.insert(std::make_pair(key, chunkTreeId));

    if (IsOutputLivePreviewSupported()) {
        auto masterConnector = Host->GetMasterConnector();
        masterConnector->AttachLivePreviewChunkTree(
            Operation,
            table.LivePreviewChunkListId,
            chunkTreeId);
    }

    LOG_DEBUG("Output chunk tree registered (Table: %d, ChunkTreeId: %s, Key: %d)",
        tableIndex,
        ~ToString(chunkTreeId),
        key);
}

void TOperationControllerBase::RegisterOutput(
    const NChunkServer::TChunkTreeId& chunkTreeId,
    int key,
    int tableIndex)
{
    auto& table = OutputTables[tableIndex];
    RegisterOutput(chunkTreeId, key, tableIndex, table);
}

void TOperationControllerBase::RegisterOutput(TJobletPtr joblet, int key)
{
    const auto* userJobResult = FindUserJobResult(joblet);

    for (int tableIndex = 0; tableIndex < static_cast<int>(OutputTables.size()); ++tableIndex) {
        auto& table = OutputTables[tableIndex];
        RegisterOutput(joblet->ChunkListIds[tableIndex], key, tableIndex, table);

        if (table.Options->KeyColumns && IsSortedOutputSupported()) {
            YCHECK(userJobResult);
            auto& boundaryKeys = userJobResult->output_boundary_keys(tableIndex);
            YCHECK(boundaryKeys.start() <= boundaryKeys.end());
            {
                TOutputTable::TEndpoint endpoint;
                endpoint.Key = boundaryKeys.start();
                endpoint.Left = true;
                endpoint.ChunkTreeKey = key;
                table.Endpoints.push_back(endpoint);
            }
            {
                TOutputTable::TEndpoint endpoint;
                endpoint.Key = boundaryKeys.end();
                endpoint.Left = false;
                endpoint.ChunkTreeKey = key;
                table.Endpoints.push_back(endpoint);
            }
        }
    }
}

void TOperationControllerBase::CompletePreparation()
{
    if (InputChunks.empty()) {
        // Possible reasons:
        // - All input chunks are unavailable && Strategy == Skip
        // - Merge decided to passthrough all input chunks
        // - Anything else?
        LOG_INFO("Empty input");
        OnOperationCompleted();
        return;
    }

    ChunkListPool = New<TChunkListPool>(
        Config,
        Host->GetMasterChannel(),
        CancelableControlInvoker,
        Operation->GetOperationId(),
        Operation->GetOutputTransaction()->GetId());

    if (Spec->UnavailableChunkStrategy != EUnavailableChunkAction::Wait)
        return;

    YCHECK(UnavailableInputChunkCount == 0);
    FOREACH(auto& pair, InputChunks) {
        auto& chunkDescriptor = pair.second;

        if (chunkDescriptor.State == EInputChunkState::Waiting) {
            LOG_DEBUG("Input chunk is unavailable (ChunkId: %s)", ~ToString(pair.first));
            ++UnavailableInputChunkCount;
            FOREACH(const auto& inputStripe, chunkDescriptor.InputStripes) {
                inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);
                ++inputStripe.Stripe->WaitingChunkCount;
            }
        }
    }

    if (UnavailableInputChunkCount > 0) {
        LOG_DEBUG("Waiting for %d unavailable chunks", UnavailableInputChunkCount);
        InputChunkScratcher->Start();
    }
}

void TOperationControllerBase::RegisterInputStripe(TChunkStripePtr stripe, TTaskPtr task)
{
    yhash_set<TChunkId> visitedChunks;

    TStripeDescriptor stripeDescriptor;
    stripeDescriptor.Stripe = stripe;
    stripeDescriptor.Task = task;
    stripeDescriptor.Cookie = task->GetChunkPoolInput()->Add(stripe);

    FOREACH(const auto& slice, stripe->ChunkSlices) {
        auto chunkSpec = slice->GetChunkSpec();
        auto chunkId = FromProto<TChunkId>(chunkSpec->chunk_id());

        if (!visitedChunks.insert(chunkId).second) {
            // We have already seen this chunk in this stripe.
            continue;
        }

        auto pair = InputChunks.insert(std::make_pair(chunkId, TInputChunkDescriptor()));
        auto& chunkDescriptor = pair.first->second;
        chunkDescriptor.InputStripes.push_back(stripeDescriptor);

        if (InputChunkSpecs.insert(chunkSpec).second) {
            chunkDescriptor.ChunkSpecs.push_back(chunkSpec);
        }

        if (IsUnavailable(*chunkSpec)) {
            chunkDescriptor.State = EInputChunkState::Waiting;
        }
    }
}

void TOperationControllerBase::RegisterIntermediate(
    TCompleteJobPtr completedJob,
    TChunkStripePtr stripe)
{
    FOREACH (const auto& chunkSlice, stripe->ChunkSlices) {
        auto chunkId = FromProto<TChunkId>(chunkSlice->GetChunkSpec()->chunk_id());
        YCHECK(ChunkOriginMap.insert(std::make_pair(chunkId, completedJob)).second);

        if (IsIntermediateLivePreviewSupported()) {
            auto masterConnector = Host->GetMasterConnector();
            masterConnector->AttachLivePreviewChunkTree(
                Operation,
                IntermediateTable.LivePreviewChunkListId,
                chunkId);
        }
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

void TOperationControllerBase::RegisterJoblet(TJobletPtr joblet)
{
    YCHECK(JobletMap.insert(std::make_pair(joblet->Job, joblet)).second);
}

TOperationControllerBase::TJobletPtr TOperationControllerBase::GetJoblet(TJobPtr job)
{
    auto it = JobletMap.find(job);
    YCHECK(it != JobletMap.end());
    return it->second;
}

void TOperationControllerBase::RemoveJoblet(TJobPtr job)
{
    YCHECK(JobletMap.erase(job) == 1);
}

void TOperationControllerBase::BuildProgressYson(IYsonConsumer* consumer)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    BuildYsonMapFluently(consumer)
        .Item("jobs").BeginMap()
            .Item("total").Value(JobCounter.GetCompleted() + JobCounter.GetRunning() + GetPendingJobCount())
            .Item("pending").Value(GetPendingJobCount())
            .Item("running").Value(JobCounter.GetRunning())
            .Item("completed").Value(JobCounter.GetCompleted())
            .Item("failed").Value(JobCounter.GetFailed())
            .Item("aborted").Value(JobCounter.GetAborted())
            .Item("lost").Value(JobCounter.GetLost())
        .EndMap()
        .Item("job_statistics").BeginMap()
            .Item("completed").Value(Operation->CompletedJobStatistics())
            .Item("failed").Value(Operation->FailedJobStatistics())
            .Item("aborted").Value(Operation->AbortedJobStatistics())
        .EndMap();
}

void TOperationControllerBase::BuildResultYson(IYsonConsumer* consumer)
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto error = FromProto(Operation->Result().error());
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("error").Value(error)
        .EndMap();
}

std::vector<TOperationControllerBase::TPathWithStage> TOperationControllerBase::GetFilePaths() const
{
    return std::vector<TPathWithStage>();
}

int TOperationControllerBase::SuggestJobCount(
    i64 totalDataSize,
    i64 dataSizePerJob,
    TNullable<int> configJobCount) const
{
    i64 suggestionBySize = 1 + totalDataSize / dataSizePerJob;
    i64 jobCount = configJobCount.Get(suggestionBySize);
    return static_cast<int>(Clamp(jobCount, 1, Config->MaxJobCount));
}

void TOperationControllerBase::InitUserJobSpec(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TUserJobSpecPtr config,
    const std::vector<TRegularUserFile>& regularFiles,
    const std::vector<TUserTableFile>& tableFiles)
{
    jobSpec->set_shell_command(config->Command);
    jobSpec->set_memory_limit(config->MemoryLimit);
    i64 memoryReserve = static_cast<i64>(config->MemoryLimit * config->MemoryReserveFactor);
    jobSpec->set_memory_reserve(memoryReserve);
    jobSpec->set_use_yamr_descriptors(config->UseYamrDescriptors);
    jobSpec->set_max_stderr_size(config->MaxStderrSize);

    {
        if (Operation->GetStdErrCount() < Operation->GetMaxStdErrCount()) {
            auto stdErrTransactionId = Operation->GetAsyncSchedulerTransaction()->GetId();
            ToProto(jobSpec->mutable_stderr_transaction_id(), stdErrTransactionId);
        }
    }

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

        jobSpec->set_input_format(ConvertToYsonString(inputFormat).Data());
        jobSpec->set_output_format(ConvertToYsonString(outputFormat).Data());
    }

    auto fillEnvironment = [&] (yhash_map<Stroka, Stroka>& env) {
        FOREACH (const auto& pair, env) {
            jobSpec->add_environment(Sprintf("%s=%s", ~pair.first, ~pair.second));
        }
    };

    // Global environment.
    fillEnvironment(Config->Environment);

    // Local environment.
    fillEnvironment(config->Environment);

    jobSpec->add_environment(Sprintf("YT_OPERATION_ID=%s",
        ~ToString(Operation->GetOperationId())));

    FOREACH (const auto& file, regularFiles) {
        auto *descriptor = jobSpec->add_regular_files();
        *descriptor->mutable_file() = *file.FetchResponse;
        descriptor->set_executable(file.Executable);
        descriptor->set_file_name(file.FileName);
    }

    FOREACH (const auto& file, tableFiles) {
        auto* descriptor = jobSpec->add_table_files();
        *descriptor->mutable_table() = *file.FetchResponse;
        descriptor->set_file_name(file.FileName);
        descriptor->set_format(file.Format.Data());
    }
}

void TOperationControllerBase::AddUserJobEnvironment(
    NScheduler::NProto::TUserJobSpec* proto,
    TJobletPtr joblet)
{
    proto->add_environment(Sprintf("YT_JOB_INDEX=%d", joblet->JobIndex));
    proto->add_environment(Sprintf("YT_JOB_ID=%s", ~ToString(joblet->Job->GetId())));
    if (joblet->StartRowIndex >= 0) {
        proto->add_environment(Sprintf("YT_START_ROW_INDEX=%" PRId64, joblet->StartRowIndex));
    }
}

i64 TOperationControllerBase::GetFinalOutputIOMemorySize(TJobIOConfigPtr ioConfig) const
{
    i64 result = 0;
    FOREACH (const auto& outputTable, OutputTables) {
        if (outputTable.Options->ErasureCodec == NErasure::ECodec::None) {
            i64 maxBufferSize = std::max(
                ioConfig->TableWriter->MaxRowWeight,
                ioConfig->TableWriter->MaxBufferSize);
            result += GetOutputWindowMemorySize(ioConfig) + maxBufferSize;
        } else {
            auto* codec = NErasure::GetCodec(outputTable.Options->ErasureCodec);
            double replicationFactor = (double) codec->GetTotalPartCount() / codec->GetDataPartCount();
            result += static_cast<i64>(ioConfig->TableWriter->DesiredChunkSize * replicationFactor);
        }
    }

    // Each writer may have up to 2 active chunks: closing one and current one.
    result *= 2;
    return result;
}

i64 TOperationControllerBase::GetFinalIOMemorySize(
    TJobIOConfigPtr ioConfig,
    const TChunkStripeStatisticsVector& stripeStatistics) const
{
    i64 result = 0;
    FOREACH (const auto& stat, stripeStatistics) {
        result += GetInputIOMemorySize(ioConfig, stat);
    }
    result += GetFinalOutputIOMemorySize(ioConfig);
    return result;
}

void TOperationControllerBase::InitIntermediateInputConfig(TJobIOConfigPtr config)
{
    // Disable master requests.
    config->TableReader->AllowFetchingSeedsFromMaster = false;
}

void TOperationControllerBase::InitIntermediateOutputConfig(TJobIOConfigPtr config)
{
    // Don't replicate intermediate output.
    config->TableWriter->UploadReplicationFactor = 1;

    // Cache blocks on nodes.
    config->TableWriter->EnableNodeCaching = true;

    // Don't move intermediate chunks.
    config->TableWriter->ChunksMovable = false;
}

void TOperationControllerBase::InitFinalOutputConfig(TJobIOConfigPtr config)
{
    UNUSED(config);
}

const NProto::TUserJobResult* TOperationControllerBase::FindUserJobResult(TJobletPtr joblet)
{
    const auto& result = joblet->Job->Result();

    if (result.HasExtension(TReduceJobResultExt::reduce_job_result_ext)) {
        return &result
               .GetExtension(TReduceJobResultExt::reduce_job_result_ext)
               .reducer_result();
    }

    if (result.HasExtension(TMapJobResultExt::map_job_result_ext)) {
        return &result
               .GetExtension(TMapJobResultExt::map_job_result_ext)
               .mapper_result();
    }

    return nullptr;
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

