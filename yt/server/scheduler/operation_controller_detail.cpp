#include "operation_controller_detail.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "chunk_pool.h"
#include "helpers.h"
#include "master_connector.h"

#include <yt/ytlib/chunk_client/chunk_list_ypath_proxy.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_scraper.h>
#include <yt/ytlib/chunk_client/chunk_slice.h>
#include <yt/ytlib/chunk_client/data_statistics.h>
#include <yt/ytlib/chunk_client/chunk_teleporter.h>
#include <yt/ytlib/chunk_client/helpers.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/node_tracker_client/node_directory_builder.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/query_client/plan_fragment.h>
#include <yt/ytlib/query_client/query_preparer.h>
#include <yt/ytlib/query_client/udf_descriptor.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/schema.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_ypath_proxy.h>

#include <yt/ytlib/api/transaction.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/misc/fs.h>

#include <yt/core/concurrency/action_queue.h>

#include <functional>

namespace NYT {
namespace NScheduler {

using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NFormats;
using namespace NJobProxy;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NNodeTrackerClient::NProto;
using namespace NConcurrency;
using namespace NApi;
using namespace NRpc;
using namespace NTableClient;
using namespace NQueryClient;

using NNodeTrackerClient::TNodeId;
using NTableClient::NProto::TBoundaryKeysExt;

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TUserObjectBase::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Path);
    Persist(context, ObjectId);
    Persist(context, CellTag);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TLivePreviewTableBase::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, LivePreviewTableId);
    Persist(context, LivePreviewChunkListId);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TInputTable::Persist(TPersistenceContext& context)
{
    TUserObjectBase::Persist(context);

    using NYT::Persist;
    Persist(context, ChunkCount);
    Persist(context, Chunks);
    Persist(context, KeyColumns);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TJobBoundaryKeys::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, MinKey);
    Persist(context, MaxKey);
    Persist(context, ChunkTreeKey);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TOutputTable::Persist(TPersistenceContext& context)
{
    TUserObjectBase::Persist(context);
    TLivePreviewTableBase::Persist(context);

    using NYT::Persist;
    Persist(context, AppendRequested);
    Persist(context, UpdateMode);
    Persist(context, LockMode);
    Persist(context, Options);
    Persist(context, KeyColumns);
    Persist(context, UploadTransactionId);
    Persist(context, OutputChunkListId);
    Persist(context, DataStatistics);
    // NB: Scheduler snapshots need not be stable.
    Persist<
        TMultiMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, OutputChunkTreeIds);
    Persist(context, BoundaryKeys);
    Persist(context, EffectiveAcl);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TIntermediateTable::Persist(TPersistenceContext& context)
{
    TLivePreviewTableBase::Persist(context);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TUserFile::Persist(TPersistenceContext& context)
{
    TUserObjectBase::Persist(context);

    using NYT::Persist;
    Persist<TAttributeDictionaryRefSerializer>(context, Attributes);
    Persist(context, Stage);
    Persist(context, FileName);
    Persist(context, ChunkSpecs);
    Persist(context, Type);
    Persist(context, Executable);
    Persist(context, Format);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TCompletedJob::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, IsLost);
    Persist(context, JobId);
    Persist(context, SourceTask);
    Persist(context, OutputCookie);
    Persist(context, DestinationPool);
    Persist(context, InputCookie);
    Persist(context, NodeId);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TJoblet::Persist(TPersistenceContext& context)
{
    // NB: Every joblet is aborted after snapshot is loaded.
    // Here we only serialize a subset of members required for ReinstallJob to work
    // properly.
    using NYT::Persist;
    Persist(context, Task);
    Persist(context, InputStripeList);
    Persist(context, OutputCookie);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TTaskGroup::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, MinNeededResources);
    // NB: Scheduler snapshots need not be stable.
    Persist<
        TSetSerializer<
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, NonLocalTasks);
    Persist<
        TMultiMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, CandidateTasks);
    Persist<
        TMultiMapSerializer<
            TDefaultSerializer,
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, DelayedTasks);
    Persist<
        TMapSerializer<
            TDefaultSerializer,
            TSetSerializer<
                TDefaultSerializer,
                TUnsortedTag
            >,
            TUnsortedTag
        >
    >(context, NodeIdToTasks);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TStripeDescriptor::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Stripe);
    Persist(context, Cookie);
    Persist(context, Task);
}

////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TInputChunkDescriptor::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, InputStripes);
    Persist(context, ChunkSpecs);
    Persist(context, State);
}

////////////////////////////////////////////////////////////////////

TOperationControllerBase::TTask::TTask()
    : CachedPendingJobCount(-1)
    , CachedTotalJobCount(-1)
    , LastDemandSanityCheckTime(TInstant::Zero())
    , CompletedFired(false)
    , Logger(OperationLogger)
{ }

TOperationControllerBase::TTask::TTask(TOperationControllerBase* controller)
    : Controller(controller)
    , CachedPendingJobCount(0)
    , CachedTotalJobCount(0)
    , LastDemandSanityCheckTime(TInstant::Zero())
    , CompletedFired(false)
    , Logger(OperationLogger)
{ }

void TOperationControllerBase::TTask::Initialize()
{
    Logger = Controller->Logger;
    Logger.AddTag("Task: %v", GetId());
}

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

int TOperationControllerBase::TTask::GetTotalJobCount() const
{
    return GetChunkPoolOutput()->GetTotalJobCount();
}

int TOperationControllerBase::TTask::GetTotalJobCountDelta()
{
    int oldValue = CachedTotalJobCount;
    int newValue = GetTotalJobCount();
    CachedTotalJobCount = newValue;
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

bool TOperationControllerBase::TTask::IsIntermediateOutput() const
{
    return false;
}

i64 TOperationControllerBase::TTask::GetLocality(TNodeId nodeId) const
{
    return HasInputLocality()
        ? GetChunkPoolOutput()->GetLocality(nodeId)
        : 0;
}

bool TOperationControllerBase::TTask::HasInputLocality() const
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
    for (auto stripe : stripes) {
        if (stripe) {
            AddInput(stripe);
        }
    }
}

void TOperationControllerBase::TTask::FinishInput()
{
    LOG_DEBUG("Task input finished");

    GetChunkPoolInput()->Finish();
    AddPendingHint();
    CheckCompleted();
}

void TOperationControllerBase::TTask::CheckCompleted()
{
    if (!CompletedFired && IsCompleted()) {
        CompletedFired = true;
        OnTaskCompleted();
    }
}

TJobId TOperationControllerBase::TTask::ScheduleJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    bool intermediateOutput = IsIntermediateOutput();
    if (!Controller->HasEnoughChunkLists(intermediateOutput)) {
        LOG_DEBUG("Job chunk list demand is not met");
        return NullJobId;
    }

    int jobIndex = Controller->JobIndexGenerator.Next();
    auto joblet = New<TJoblet>(this, jobIndex);

    const auto& nodeResourceLimits = context->ResourceLimits();
    auto nodeId = context->GetNode()->GetId();
    const auto& address = context->GetAddress();

    auto* chunkPoolOutput = GetChunkPoolOutput();
    auto localityNodeId = HasInputLocality() ? nodeId : InvalidNodeId;
    joblet->OutputCookie = chunkPoolOutput->Extract(localityNodeId);
    if (joblet->OutputCookie == IChunkPoolOutput::NullCookie) {
        LOG_DEBUG("Job input is empty");
        return NullJobId;
    }

    joblet->InputStripeList = chunkPoolOutput->GetStripeList(joblet->OutputCookie);
    joblet->MemoryReserveEnabled = IsMemoryReserveEnabled();

    auto neededResources = GetNeededResources(joblet);

    // Check the usage against the limits. This is the last chance to give up.
    if (!Dominates(jobLimits, neededResources)) {
        LOG_DEBUG("Job actual resource demand is not met (Limits: {%v}, Demand: {%v})",
            FormatResources(jobLimits),
            FormatResources(neededResources));
        CheckResourceDemandSanity(nodeResourceLimits, neededResources);
        chunkPoolOutput->Aborted(joblet->OutputCookie);
        // Seems like cached min needed resources are too optimistic.
        ResetCachedMinNeededResources();
        return NullJobId;
    }

    auto jobType = GetJobType();

    // Async part.
    auto controller = MakeStrong(Controller); // hold the controller
    auto jobSpecBuilder = BIND([=, this_ = MakeStrong(this)] (TJobSpec* jobSpec) {
        BuildJobSpec(joblet, jobSpec);
        controller->CustomizeJobSpec(joblet, jobSpec);

        auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_enable_job_proxy_memory_control(controller->Spec->EnableJobProxyMemoryControl);
        schedulerJobSpecExt->set_enable_sort_verification(controller->Spec->EnableSortVerification);

        // Adjust sizes if approximation flag is set.
        if (joblet->InputStripeList->IsApproximate) {
            schedulerJobSpecExt->set_input_uncompressed_data_size(static_cast<i64>(
                schedulerJobSpecExt->input_uncompressed_data_size() *
                ApproximateSizesBoostFactor));
            schedulerJobSpecExt->set_input_row_count(static_cast<i64>(
                schedulerJobSpecExt->input_row_count() *
                ApproximateSizesBoostFactor));
        }

        if (schedulerJobSpecExt->input_uncompressed_data_size() > Controller->Spec->MaxDataSizePerJob) {
            Controller->OnOperationFailed(TError(
                "Maximum allowed data size per job violated: %v > %v",
                schedulerJobSpecExt->input_uncompressed_data_size(),
                Controller->Spec->MaxDataSizePerJob));
        }
    });

    auto restarted = LostJobCookieMap.find(joblet->OutputCookie) != LostJobCookieMap.end();
    joblet->JobId = context->StartJob(
        Controller->Operation,
        jobType,
        neededResources,
        restarted,
        jobSpecBuilder);

    joblet->JobType = jobType;
    joblet->Address = address;
    joblet->NodeId = nodeId;

    LOG_INFO(
        "Job scheduled (JobId: %v, OperationId: %v, JobType: %v, Address: %v, JobIndex: %v, ChunkCount: %v (%v local), "
        "Approximate: %v, DataSize: %v (%v local), RowCount: %v, Restarted: %v, ResourceLimits: {%v})",
        joblet->JobId,
        Controller->OperationId,
        jobType,
        address,
        jobIndex,
        joblet->InputStripeList->TotalChunkCount,
        joblet->InputStripeList->LocalChunkCount,
        joblet->InputStripeList->IsApproximate,
        joblet->InputStripeList->TotalDataSize,
        joblet->InputStripeList->LocalDataSize,
        joblet->InputStripeList->TotalRowCount,
        restarted,
        FormatResources(neededResources));

    // Prepare chunk lists.
    if (intermediateOutput) {
        joblet->ChunkListIds.push_back(Controller->ExtractChunkList(Controller->IntermediateOutputCellTag));
    } else {
        for (const auto& table : Controller->OutputTables) {
            joblet->ChunkListIds.push_back(Controller->ExtractChunkList(table.CellTag));
        }
    }

    // Sync part.
    PrepareJoblet(joblet);
    Controller->CustomizeJoblet(joblet);

    Controller->RegisterJoblet(joblet);

    OnJobStarted(joblet);

    return joblet->JobId;
}

bool TOperationControllerBase::TTask::IsPending() const
{
    return GetChunkPoolOutput()->GetPendingJobCount() > 0;
}

bool TOperationControllerBase::TTask::IsCompleted() const
{
    return IsActive() && GetChunkPoolOutput()->IsCompleted();
}

bool TOperationControllerBase::TTask::IsActive() const
{
    return true;
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

void TOperationControllerBase::TTask::Persist(TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, DelayedTime_);

    Persist(context, Controller);

    Persist(context, CachedPendingJobCount);
    Persist(context, CachedTotalJobCount);

    Persist(context, CachedTotalNeededResources);
    Persist(context, CachedMinNeededResources);

    Persist(context, LastDemandSanityCheckTime);

    Persist(context, CompletedFired);

    Persist(context, LostJobCookieMap);
}

void TOperationControllerBase::TTask::PrepareJoblet(TJobletPtr /* joblet */)
{ }

void TOperationControllerBase::TTask::OnJobStarted(TJobletPtr /* joblet */)
{ }

void TOperationControllerBase::TTask::OnJobCompleted(TJobletPtr joblet, const TCompletedJobSummary& jobSummary)
{
    if (!jobSummary.Abandoned) {
        const auto& statistics = jobSummary.Statistics;
        auto outputStatisticsMap = GetOutputDataStatistics(statistics);
        for (int index = 0; index < static_cast<int>(joblet->ChunkListIds.size()); ++index) {
            YCHECK(outputStatisticsMap.find(index) != outputStatisticsMap.end());
            auto outputStatistics = outputStatisticsMap[index];
            if (outputStatistics.chunk_count() == 0) {
                Controller->ChunkListPool->Reinstall(joblet->ChunkListIds[index]);
                joblet->ChunkListIds[index] = NullChunkListId;
            }
        }

        auto inputStatistics = GetTotalInputDataStatistics(statistics);
        auto outputStatistics = GetTotalOutputDataStatistics(statistics);
        if (Controller->IsRowCountPreserved()) {
            if (inputStatistics.row_count() != outputStatistics.row_count()) {
                Controller->OnOperationFailed(TError(
                    "Input/output row count mismatch in completed job: %v != %v",
                    inputStatistics.row_count(),
                    outputStatistics.row_count())
                    << TErrorAttribute("task", GetId()));
            }
        }
    } else {
        for (int index = 0; index < static_cast<int>(joblet->ChunkListIds.size()); ++index) {
            Controller->ChunkListPool->Reinstall(joblet->ChunkListIds[index]);
            joblet->ChunkListIds[index] = NullChunkListId;
        }
    }
    GetChunkPoolOutput()->Completed(joblet->OutputCookie);
}

void TOperationControllerBase::TTask::ReinstallJob(TJobletPtr joblet, EJobReinstallReason reason)
{
    Controller->ReleaseChunkLists(joblet->ChunkListIds);

    auto* chunkPoolOutput = GetChunkPoolOutput();

    auto list = HasInputLocality()
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
        for (const auto& stripe : list->Stripes) {
            Controller->AddTaskLocalityHint(this, stripe);
        }
    }

    AddPendingHint();
}

void TOperationControllerBase::TTask::OnJobFailed(TJobletPtr joblet, const TFailedJobSummary& /* jobSummary */)
{
    ReinstallJob(joblet, EJobReinstallReason::Failed);
}

void TOperationControllerBase::TTask::OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& /* jobSummary */)
{
    ReinstallJob(joblet, EJobReinstallReason::Aborted);
}

void TOperationControllerBase::TTask::OnJobLost(TCompletedJobPtr completedJob)
{
    YCHECK(LostJobCookieMap.insert(std::make_pair(
        completedJob->OutputCookie,
        completedJob->InputCookie)).second);
}

void TOperationControllerBase::TTask::OnTaskCompleted()
{
    LOG_DEBUG("Task completed");
}

void TOperationControllerBase::TTask::DoCheckResourceDemandSanity(
    const TNodeResources& neededResources)
{
    auto nodes = Controller->Host->GetExecNodes();
    if (nodes.size() < Controller->Config->SafeOnlineNodeCount)
        return;

    for (const auto& node : nodes) {
        if (node->CanSchedule(Controller->Operation->GetSchedulingTag()) &&
            Dominates(node->ResourceLimits(), neededResources))
        {
            return;
        }
    }

    // It seems nobody can satisfy the demand.
    Controller->OnOperationFailed(
        TError("No online node can satisfy the resource demand")
            << TErrorAttribute("task", GetId())
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

    // Schedule check in controller thread.
    Controller->GetCancelableInvoker()->Invoke(BIND(
        &TTask::DoCheckResourceDemandSanity,
        MakeWeak(this),
        neededResources));
}

void TOperationControllerBase::TTask::CheckResourceDemandSanity(
    const TNodeResources& nodeResourceLimits,
    const TNodeResources& neededResources)
{
    // The task is requesting more than some node is willing to provide it.
    // Maybe it's OK and we should wait for some time.
    // Or maybe it's not and the task is requesting something no one is able to provide.

    // First check if this very node has enough resources (including those currently
    // allocated by other jobs).
    if (Dominates(nodeResourceLimits, neededResources))
        return;

    CheckResourceDemandSanity(neededResources);
}

void TOperationControllerBase::TTask::AddPendingHint()
{
    Controller->AddTaskPendingHint(this);
}

void TOperationControllerBase::TTask::AddLocalityHint(TNodeId nodeId)
{
    Controller->AddTaskLocalityHint(this, nodeId);
}

void TOperationControllerBase::TTask::AddSequentialInputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    TNodeDirectoryBuilder directoryBuilder(
        Controller->InputNodeDirectory,
        schedulerJobSpecExt->mutable_input_node_directory());
    auto* inputSpec = schedulerJobSpecExt->add_input_specs();
    inputSpec->set_table_reader_options(ConvertToYsonString(GetTableReaderOptions()).Data());
    const auto& list = joblet->InputStripeList;
    for (const auto& stripe : list->Stripes) {
        AddChunksToInputSpec(&directoryBuilder, inputSpec, stripe, list->PartitionTag);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::AddParallelInputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    TNodeDirectoryBuilder directoryBuilder(
        Controller->InputNodeDirectory,
        schedulerJobSpecExt->mutable_input_node_directory());
    const auto& list = joblet->InputStripeList;
    for (const auto& stripe : list->Stripes) {
        auto* inputSpec = schedulerJobSpecExt->add_input_specs();
        inputSpec->set_table_reader_options(ConvertToYsonString(GetTableReaderOptions()).Data());
        AddChunksToInputSpec(&directoryBuilder, inputSpec, stripe, list->PartitionTag);
    }
    UpdateInputSpecTotals(jobSpec, joblet);
}

void TOperationControllerBase::TTask::AddChunksToInputSpec(
    TNodeDirectoryBuilder* directoryBuilder,
    TTableInputSpec* inputSpec,
    TChunkStripePtr stripe,
    TNullable<int> partitionTag)
{
    for (const auto& chunkSlice : stripe->ChunkSlices) {
        auto* chunkSpec = inputSpec->add_chunks();
        ToProto(chunkSpec, chunkSlice);
        for (ui32 protoReplica : chunkSlice->GetChunkSpec()->replicas()) {
            auto replica = FromProto<TChunkReplica>(protoReplica);
            directoryBuilder->Add(replica);
        }
        if (partitionTag) {
            chunkSpec->set_partition_tag(*partitionTag);
        }
    }
}

void TOperationControllerBase::TTask::UpdateInputSpecTotals(
    TJobSpec* jobSpec,
    TJobletPtr joblet)
{
    const auto& list = joblet->InputStripeList;
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
    for (int index = 0; index < Controller->OutputTables.size(); ++index) {
        const auto& table = Controller->OutputTables[index];
        auto* outputSpec = schedulerJobSpecExt->add_output_specs();
        outputSpec->set_table_writer_options(ConvertToYsonString(table.Options).Data());
        if (!table.KeyColumns.empty()) {
            ToProto(outputSpec->mutable_key_columns(), table.KeyColumns);
        }
        ToProto(outputSpec->mutable_chunk_list_id(), joblet->ChunkListIds[index]);
    }
}

void TOperationControllerBase::TTask::AddIntermediateOutputSpec(
    TJobSpec* jobSpec,
    TJobletPtr joblet,
    const TKeyColumns& keyColumns)
{
    YCHECK(joblet->ChunkListIds.size() == 1);
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    auto* outputSpec = schedulerJobSpecExt->add_output_specs();
    auto options = New<TTableWriterOptions>();
    options->Account = Controller->Spec->IntermediateDataAccount;
    options->ChunksVital = false;
    options->ChunksMovable = false;
    options->ReplicationFactor = 1;
    options->CompressionCodec = Controller->Spec->IntermediateCompressionCodec;
    outputSpec->set_table_writer_options(ConvertToYsonString(options).Data());

    if (!keyColumns.empty()) {
        ToProto(outputSpec->mutable_key_columns(), keyColumns);
    }
    ToProto(outputSpec->mutable_chunk_list_id(), joblet->ChunkListIds[0]);
}

void TOperationControllerBase::TTask::ResetCachedMinNeededResources()
{
    CachedMinNeededResources.Reset();
}

const TNodeResources& TOperationControllerBase::TTask::GetMinNeededResources() const
{
    if (!CachedMinNeededResources) {
        YCHECK(GetPendingJobCount() > 0);
        CachedMinNeededResources = GetMinNeededResourcesHeavy();
    }
    return *CachedMinNeededResources;
}

TNodeResources TOperationControllerBase::TTask::GetNeededResources(TJobletPtr /* joblet */) const
{
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
        joblet->JobId,
        this,
        joblet->OutputCookie,
        destinationPool,
        inputCookie,
        joblet->Address,
        joblet->NodeId);

    Controller->RegisterIntermediate(
        joblet,
        completedJob,
        stripe);
}

TChunkStripePtr TOperationControllerBase::TTask::BuildIntermediateChunkStripe(
    google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>* chunkSpecs)
{
    auto stripe = New<TChunkStripe>();
    for (auto& chunkSpec : *chunkSpecs) {
        auto chunkSlice = CreateChunkSlice(New<TRefCountedChunkSpec>(std::move(chunkSpec)));
        stripe->ChunkSlices.push_back(chunkSlice);
    }
    return stripe;
}

void TOperationControllerBase::TTask::RegisterOutput(
    TJobletPtr joblet,
    int key,
    const TCompletedJobSummary& jobSummary)
{
    Controller->RegisterOutput(joblet, key, jobSummary);
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
    , OperationId(Operation->GetId())
    , AuthenticatedMasterClient(CreateClient())
    , AuthenticatedInputMasterClient(AuthenticatedMasterClient)
    , AuthenticatedOutputMasterClient(AuthenticatedMasterClient)
    , Logger(OperationLogger)
    , CancelableContext(New<TCancelableContext>())
    , CancelableControlInvoker(CancelableContext->CreateInvoker(Host->GetControlInvoker()))
    , Invoker(Host->CreateOperationControllerInvoker())
    , SuspendableInvoker(CreateSuspendableInvoker(Invoker))
    , CancelableInvoker(CancelableContext->CreateInvoker(SuspendableInvoker))
    , JobCounter(0)
    , Spec(spec)
    , CachedNeededResources(ZeroNodeResources())
    , CheckTimeLimitExecutor(New<TPeriodicExecutor>(
        GetCancelableInvoker(),
        BIND(&TThis::CheckTimeLimit, MakeWeak(this)),
        Config->OperationTimeLimitCheckPeriod))
{
    Logger.AddTag("OperationId: %v", operation->GetId());
}

void TOperationControllerBase::Initialize()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    LOG_INFO("Initializing operation (Title: %v)",
        Spec->Title);

    InputNodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
    AuxNodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();

    for (const auto& path : GetInputTablePaths()) {
        TInputTable table;
        table.Path = path;
        InputTables.push_back(table);
    }

    for (const auto& path : GetOutputTablePaths()) {
        TOutputTable table;
        table.Path = path;

        if (path.GetAppend()) {
            table.AppendRequested = true;
            table.UpdateMode = EUpdateMode::Append;
            table.LockMode = ELockMode::Shared;
        }

        table.KeyColumns = path.Attributes().Get<TKeyColumns>("sorted_by", TKeyColumns());
        if (!table.KeyColumns.empty()) {
            if (!IsSortedOutputSupported()) {
                THROW_ERROR_EXCEPTION("Sorted outputs are not supported");
            }
            table.UpdateMode = EUpdateMode::Overwrite;
            table.LockMode = ELockMode::Exclusive;
        }

        OutputTables.push_back(table);
    }

    for (const auto& pair : GetFilePaths()) {
        TUserFile file;
        file.Path = pair.first;
        file.Stage = pair.second;
        Files.push_back(file);
    }

    if (InputTables.size() > Config->MaxInputTableCount) {
        THROW_ERROR_EXCEPTION(
            "Too many input tables: maximum allowed %v, actual %v",
            Config->MaxInputTableCount,
            InputTables.size());
    }

    if (OutputTables.size() > Config->MaxOutputTableCount) {
        THROW_ERROR_EXCEPTION(
            "Too many output tables: maximum allowed %v, actual %v",
            Config->MaxOutputTableCount,
            OutputTables.size());
    }

    DoInitialize();

    LOG_INFO("Operation initialized");
}

void TOperationControllerBase::Essentiate()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    Operation->SetMaxStderrCount(Spec->MaxStderrCount.Get(Config->MaxStderrCount));
    Operation->SetSchedulingTag(Spec->SchedulingTag);

    InitializeTransactions();
}

void TOperationControllerBase::DoInitialize()
{ }

void TOperationControllerBase::Prepare()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    GetInputTablesBasicAttributes();
    GetOutputTablesBasicAttributes();
    GetFilesBasicAttributes(&Files);

    LockInputTables();
    LockUserFiles(&Files, {});

    BeginUploadOutputTables();
    GetOutputTablesUploadParams();
}

void TOperationControllerBase::Materialize()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    try {
        FetchInputTables();
        FetchUserFiles(&Files);

        PickIntermediateDataCell();
        InitChunkListPool();

        CreateLivePreviewTables();

        PrepareLivePreviewTablesForUpdate();

        CollectTotals();

        CustomPrepare();

        if (InputChunkMap.empty()) {
            // Possible reasons:
            // - All input chunks are unavailable && Strategy == Skip
            // - Merge decided to passthrough all input chunks
            // - Anything else?
            LOG_INFO("No jobs needed");
            OnOperationCompleted();
            return;
        }

        SuspendUnavailableInputStripes();

        AddAllTaskPendingHints();

        // Input chunk scraper initialization should be the last step to avoid races,
        // because input chunk scraper works in control thread.
        InitInputChunkScraper();

        CheckTimeLimitExecutor->Start();

        SetState(EControllerState::Running);
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Materialization failed");
        auto wrappedError = TError("Materialization failed") << ex;
        OnOperationFailed(wrappedError);
        return;
    }

    LOG_INFO("Materialization finished");
}

void TOperationControllerBase::SaveSnapshot(TOutputStream* output)
{
    DoSaveSnapshot(output);
}

void TOperationControllerBase::DoSaveSnapshot(TOutputStream* output)
{
    TSaveContext context;
    context.SetOutput(output);

    Save(context, this);
}

void TOperationControllerBase::Revive()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    InitChunkListPool();

    DoLoadSnapshot();

    PrepareLivePreviewTablesForUpdate();

    AbortAllJoblets();

    AddAllTaskPendingHints();

    // Input chunk scraper initialization should be the last step to avoid races.
    InitInputChunkScraper();

    ReinstallLivePreview();

    CheckTimeLimitExecutor->Start();

    SetState(EControllerState::Running);
}

void TOperationControllerBase::InitializeTransactions()
{
    StartAsyncSchedulerTransaction();
    if (Operation->GetCleanStart()) {
        StartSyncSchedulerTransaction();
        StartInputTransaction(SyncSchedulerTransactionId);
        StartOutputTransaction(SyncSchedulerTransactionId);
    } else {
        InputTransactionId = Operation->GetInputTransaction()->GetId();
        OutputTransactionId = Operation->GetOutputTransaction()->GetId();
    }
}

TTransactionId TOperationControllerBase::StartTransaction(
    const Stroka& transactionName,
    IClientPtr client,
    const TNullable<TTransactionId>& parentTransactionId = Null)
{
    LOG_INFO("Starting %v transaction", transactionName);

    auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    {
        auto req = TMasterYPathProxy::CreateObject();
        if (parentTransactionId) {
            ToProto(req->mutable_transaction_id(), parentTransactionId.Get());
        }
        req->set_type(static_cast<int>(EObjectType::Transaction));

        auto* reqExt = req->mutable_extensions()->MutableExtension(
            NTransactionClient::NProto::TTransactionCreationExt::transaction_creation_ext);
        reqExt->set_timeout(ToProto(Config->OperationTransactionTimeout));

        auto attributes = CreateEphemeralAttributes();
        attributes->Set("title", Format("Scheduler %v for operation %v", transactionName, OperationId));
        attributes->Set("operation_id", OperationId);
        ToProto(req->mutable_object_attributes(), *attributes);

        GenerateMutationId(req);
        batchReq->AddRequest(req, Format("start_%v_tx", transactionName));
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        GetCumulativeError(batchRspOrError),
        "Error starting %v transaction",
        transactionName);

    if (Operation->GetState() != EOperationState::Initializing &&
        Operation->GetState() != EOperationState::Reviving)
        throw TFiberCanceledException();

    const auto& batchRsp = batchRspOrError.Value();
    auto rspOrError = batchRsp->GetResponse<TMasterYPathProxy::TRspCreateObjects>(
        Format("start_%v_tx", transactionName));
    THROW_ERROR_EXCEPTION_IF_FAILED(
        rspOrError,
        "Error starting %v transaction",
        transactionName);

    const auto& rsp = rspOrError.Value();
    return FromProto<TTransactionId>(rsp->object_ids(0));
}

void TOperationControllerBase::StartSyncSchedulerTransaction()
{
    TNullable<TTransactionId> userTransactionId;
    if (Operation->GetUserTransaction()) {
        userTransactionId = Operation->GetUserTransaction()->GetId();
    }
    SyncSchedulerTransactionId =
        StartTransaction("sync", AuthenticatedMasterClient, userTransactionId);
    auto transaction = AuthenticatedMasterClient->AttachTransaction(SyncSchedulerTransactionId);
    Operation->SetSyncSchedulerTransaction(transaction);

    LOG_INFO("Scheduler sync transaction started (SyncTransactionId: %v)",
        SyncSchedulerTransactionId);
}

void TOperationControllerBase::StartAsyncSchedulerTransaction()
{
    AsyncSchedulerTransactionId = StartTransaction("async", AuthenticatedMasterClient);
    auto transaction = AuthenticatedMasterClient->AttachTransaction(AsyncSchedulerTransactionId);
    Operation->SetAsyncSchedulerTransaction(transaction);

    LOG_INFO("Scheduler async transaction started (AsyncTranasctionId: %v)",
        AsyncSchedulerTransactionId);
}

void TOperationControllerBase::StartInputTransaction(TTransactionId parentTransactionId)
{
    InputTransactionId = StartTransaction("input", AuthenticatedInputMasterClient, parentTransactionId);
    auto transaction = AuthenticatedInputMasterClient->AttachTransaction(InputTransactionId);
    Operation->SetInputTransaction(transaction);

    LOG_INFO("Input transaction started (InputTransactionId: %v)",
        InputTransactionId);
}

void TOperationControllerBase::StartOutputTransaction(TTransactionId parentTransactionId)
{
    OutputTransactionId = StartTransaction("output", AuthenticatedOutputMasterClient, parentTransactionId);
    auto transaction = AuthenticatedOutputMasterClient->AttachTransaction(OutputTransactionId);
    Operation->SetOutputTransaction(transaction);

    LOG_INFO("Output transaction started (OutputTransactionId: %v)",
        OutputTransactionId);
}

void TOperationControllerBase::PickIntermediateDataCell()
{
    auto connection = AuthenticatedOutputMasterClient->GetConnection();
    const auto& secondaryCellTags = connection->GetSecondaryMasterCellTags();
    IntermediateOutputCellTag = secondaryCellTags.empty()
        ? connection->GetPrimaryMasterCellTag()
        : secondaryCellTags[rand() % secondaryCellTags.size()];
}

void TOperationControllerBase::InitChunkListPool()
{
    ChunkListPool = New<TChunkListPool>(
        Config,
        AuthenticatedOutputMasterClient,
        CancelableInvoker,
        OperationId,
        OutputTransactionId);

    for (const auto& table : OutputTables) {
        ++CellTagToOutputTableCount[table.CellTag];
    }
}

void TOperationControllerBase::InitInputChunkScraper()
{
    yhash_set<TChunkId> chunkIds;
    for (const auto& pair : InputChunkMap) {
        chunkIds.insert(pair.first);
    }

    YCHECK(!InputChunkScraper);
    InputChunkScraper = New<TChunkScraper>(
        Config,
        CancelableInvoker,
        Host->GetChunkLocationThrottlerManager(),
        AuthenticatedInputMasterClient,
        InputNodeDirectory,
        std::move(chunkIds),
        BIND(&TThis::OnInputChunkLocated, MakeWeak(this))
            .Via(CancelableControlInvoker),
        Logger
    );

    if (UnavailableInputChunkCount > 0) {
        LOG_INFO("Waiting for %v unavailable input chunks", UnavailableInputChunkCount);
        InputChunkScraper->Start();
    }
}

void TOperationControllerBase::SuspendUnavailableInputStripes()
{
    YCHECK(UnavailableInputChunkCount == 0);

    for (const auto& pair : InputChunkMap) {
        const auto& chunkDescriptor = pair.second;
        if (chunkDescriptor.State == EInputChunkState::Waiting) {
            LOG_TRACE("Input chunk is unavailable (ChunkId: %v)", pair.first);
            for (const auto& inputStripe : chunkDescriptor.InputStripes) {
                if (inputStripe.Stripe->WaitingChunkCount == 0) {
                    inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);
                }
                ++inputStripe.Stripe->WaitingChunkCount;
            }
            ++UnavailableInputChunkCount;
        }
    }
}

void TOperationControllerBase::ReinstallLivePreview()
{
    auto masterConnector = Host->GetMasterConnector();

    if (IsOutputLivePreviewSupported()) {
        for (const auto& table : OutputTables) {
            std::vector<TChunkTreeId> childrenIds;
            childrenIds.reserve(table.OutputChunkTreeIds.size());
            for (const auto& pair : table.OutputChunkTreeIds) {
                childrenIds.push_back(pair.second);
            }
            masterConnector->AttachToLivePreview(
                Operation,
                table.LivePreviewChunkListId,
                childrenIds);
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        std::vector<TChunkTreeId> childrenIds;
        childrenIds.reserve(ChunkOriginMap.size());
        for (const auto& pair : ChunkOriginMap) {
            if (!pair.second->IsLost) {
                childrenIds.push_back(pair.first);
            }
        }
        masterConnector->AttachToLivePreview(
            Operation,
            IntermediateTable.LivePreviewChunkListId,
            childrenIds);
    }
}

void TOperationControllerBase::AbortAllJoblets()
{
    for (const auto& pair : JobletMap) {
        auto joblet = pair.second;
        JobCounter.Aborted(1);
        joblet->Task->OnJobAborted(joblet, TAbortedJobSummary(pair.first, EAbortReason::Scheduler));
    }
    JobletMap.clear();
}

void TOperationControllerBase::DoLoadSnapshot()
{
    LOG_INFO("Started loading snapshot");

    auto snapshot = Operation->Snapshot();
    TMemoryInput input(snapshot.Begin(), snapshot.Size());

    TLoadContext context;
    context.SetInput(&input);

    NPhoenix::TSerializer::InplaceLoad(context, this);

    LOG_INFO("Finished loading snapshot");
}

void TOperationControllerBase::Commit()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    TeleportOutputChunks();
    AttachOutputChunks();
    EndUploadOutputTables();
    CustomCommit();

    LOG_INFO("Results committed");
}

void TOperationControllerBase::TeleportOutputChunks()
{
    auto teleporter = New<TChunkTeleporter>(
        Config,
        AuthenticatedOutputMasterClient,
        CancelableInvoker,
        Operation->GetOutputTransaction()->GetId(),
        Logger);

    for (auto& table : OutputTables) {
        for (const auto& pair : table.OutputChunkTreeIds) {
            const auto& id = pair.second;
            if (TypeFromId(id) == EObjectType::ChunkList)
                continue;
            table.ChunkPropertiesUpdateNeeded = true;
            teleporter->RegisterChunk(id, table.CellTag);
        }
    }

    WaitFor(teleporter->Run())
        .ThrowOnError();
}

void TOperationControllerBase::AttachOutputChunks()
{
    for (auto& table : OutputTables) {
        auto objectIdPath = FromObjectId(table.ObjectId);
        const auto& path = table.Path.GetPath();

        LOG_INFO("Attaching output chunks (Path: %v)",
            path);

        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(
            EMasterChannelKind::Leader, table.CellTag);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        // Split large outputs into separate requests.
        {
            TChunkListYPathProxy::TReqAttachPtr req;
            int reqSize = 0;
            auto flushReq = [ & ]() {
                if (req) {
                    batchReq->AddRequest(req, "attach");
                    reqSize = 0;
                    req.Reset();
                }
            };

            auto addChunkTree = [ & ](const TChunkTreeId& chunkTreeId) {
                if (!req) {
                    req = TChunkListYPathProxy::Attach(FromObjectId(table.OutputChunkListId));
                    req->set_request_statistics(false);
                    GenerateMutationId(req);
                }
                ToProto(req->add_children_ids(), chunkTreeId);
                ++reqSize;
                if (reqSize >= Config->MaxChildrenPerAttachRequest) {
                    flushReq();
                }
            };

            if (!table.KeyColumns.empty() && IsSortedOutputSupported()) {
                // Sorted output generated by user operation requires rearranging.
                LOG_DEBUG("Sorting %v boundary key pairs", table.BoundaryKeys.size());
                std::sort(
                    table.BoundaryKeys.begin(),
                    table.BoundaryKeys.end(),
                    [=] (const TJobBoundaryKeys& lhs, const TJobBoundaryKeys& rhs) -> bool {
                        auto keysResult = CompareRows(lhs.MinKey, rhs.MinKey);
                        if (keysResult != 0) {
                            return keysResult < 0;
                        }
                        return lhs.MaxKey < rhs.MaxKey;
                });

                for (auto current = table.BoundaryKeys.begin(); current != table.BoundaryKeys.end(); ++current) {
                    auto next = current + 1;
                    if (next != table.BoundaryKeys.end() && next->MinKey < current->MaxKey) {
                        THROW_ERROR_EXCEPTION("Output table %v is not sorted: job outputs have overlapping key ranges [MinKey %v, MaxKey: %v]",
                            table.Path.GetPath(),
                            next->MinKey,
                            current->MaxKey);
                    }

                    auto pair = table.OutputChunkTreeIds.equal_range(current->ChunkTreeKey);
                    auto it = pair.first;
                    if (it != pair.second) {
                        // Chunk tree may be absent if no data was written in the job.
                        addChunkTree(it->second);
                        // In user operations each ChunkTreeKey corresponds to a single OutputChunkTreeId.
                        // Let's check it.
                        YCHECK(++it == pair.second);
                    }
                }
            } else {
                for (const auto& pair : table.OutputChunkTreeIds) {
                    addChunkTree(pair.second);
                }
            }

            flushReq();
        }

        {
            auto req = TChunkListYPathProxy::GetStatistics(FromObjectId(table.OutputChunkListId));
            batchReq->AddRequest(req, "get_statistics");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error attaching chunks to output table %v",
            path);
        const auto& batchRsp = batchRspOrError.Value();

        {
            auto rsp = batchRsp->GetResponse<TChunkListYPathProxy::TRspGetStatistics>("get_statistics").Value();
            table.DataStatistics = rsp->statistics();
        }
    }
}

void TOperationControllerBase::CustomCommit()
{ }

void TOperationControllerBase::EndUploadOutputTables()
{
    auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& table : OutputTables) {
        auto objectIdPath = FromObjectId(table.ObjectId);
        const auto& path = table.Path.GetPath();

        LOG_INFO("Finishing upload to output to table (Path: %v, KeyColumns: [%v])",
            path,
            JoinToString(table.KeyColumns));

        {
            auto req = TTableYPathProxy::EndUpload(objectIdPath);
            *req->mutable_statistics() = table.DataStatistics;
            ToProto(req->mutable_key_columns(), table.KeyColumns);
            req->set_chunk_properties_update_needed(table.ChunkPropertiesUpdateNeeded);

            SetTransactionId(req, table.UploadTransactionId);
            GenerateMutationId(req);
            batchReq->AddRequest(req, "end_upload");
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error finishing upload to output tables");
}

void TOperationControllerBase::OnJobRunning(const TJobId& /* jobId */, const TJobStatus& /* status */)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);
}

void TOperationControllerBase::OnJobStarted(const TJobId& /* jobId */)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    JobCounter.Start(1);
}

void TOperationControllerBase::OnJobCompleted(const TCompletedJobSummary& jobSummary)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    const auto& jobId = jobSummary.Id;
    const auto& result = jobSummary.Result;

    JobCounter.Completed(1);
    UpdateJobStatistics(jobSummary);

    const auto& schedulerResultEx = result->GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    // Populate node directory by adding additional nodes returned from the job.
    // NB: Job's output may become some other job's input.
    InputNodeDirectory->MergeFrom(schedulerResultEx.output_node_directory());

    auto joblet = GetJoblet(jobId);
    joblet->Task->OnJobCompleted(joblet, jobSummary);

    RemoveJoblet(jobId);

    UpdateTask(joblet->Task);

    if (IsCompleted()) {
        OnOperationCompleted();
    }
}

void TOperationControllerBase::OnJobFailed(const TFailedJobSummary& jobSummary)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    const auto& jobId = jobSummary.Id;
    const auto& result = jobSummary.Result;

    auto error = FromProto<TError>(result->error());

    JobCounter.Failed(1);
    UpdateJobStatistics(jobSummary);

    auto joblet = GetJoblet(jobId);
    joblet->Task->OnJobFailed(joblet, jobSummary);

    RemoveJoblet(jobId);

    if (error.Attributes().Get<bool>("fatal", false)) {
        OnOperationFailed(error);
        return;
    }

    int failedJobCount = JobCounter.GetFailed();
    int maxFailedJobCount = Spec->MaxFailedJobCount.Get(Config->MaxFailedJobCount);
    if (failedJobCount >= maxFailedJobCount) {
        OnOperationFailed(TError("Failed jobs limit exceeded")
            << TErrorAttribute("max_failed_job_count", maxFailedJobCount));
    }
}

void TOperationControllerBase::OnJobAborted(const TAbortedJobSummary& jobSummary)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    const auto& jobId = jobSummary.Id;
    auto abortReason = jobSummary.AbortReason;

    JobCounter.Aborted(1, abortReason);
    UpdateJobStatistics(jobSummary);

    auto joblet = GetJoblet(jobId);
    joblet->Task->OnJobAborted(joblet, jobSummary);


    RemoveJoblet(jobId);

    if (abortReason == EAbortReason::FailedChunks) {
        const auto& result = jobSummary.Result;
        const auto& schedulerResultExt = result->GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
        for (const auto& chunkId : schedulerResultExt.failed_chunk_ids()) {
            OnChunkFailed(FromProto<TChunkId>(chunkId));
        }
    }
}

void TOperationControllerBase::OnChunkFailed(const TChunkId& chunkId)
{
    auto it = InputChunkMap.find(chunkId);
    if (it == InputChunkMap.end()) {
        LOG_WARNING("Intermediate chunk %v has failed", chunkId);
        OnIntermediateChunkUnavailable(chunkId);
    } else {
        LOG_WARNING("Input chunk %v has failed", chunkId);
        OnInputChunkUnavailable(chunkId, it->second);
    }
}

void TOperationControllerBase::OnInputChunkLocated(const TChunkId& chunkId, const TChunkReplicaList& replicas)
{
    auto it = InputChunkMap.find(chunkId);
    YCHECK(it != InputChunkMap.end());

    auto& descriptor = it->second;
    YCHECK(!descriptor.ChunkSpecs.empty());
    auto& chunkSpec = descriptor.ChunkSpecs.front();
    auto codecId = NErasure::ECodec(chunkSpec->erasure_codec());

    if (IsUnavailable(replicas, codecId, IsParityReplicasFetchEnabled())) {
        OnInputChunkUnavailable(chunkId, descriptor);
    } else {
        OnInputChunkAvailable(chunkId, descriptor, replicas);
    }
}

void TOperationControllerBase::OnInputChunkAvailable(const TChunkId& chunkId, TInputChunkDescriptor& descriptor, const TChunkReplicaList& replicas)
{
    if (descriptor.State != EInputChunkState::Waiting)
        return;

    LOG_TRACE("Input chunk is available (ChunkId: %v)", chunkId);

    --UnavailableInputChunkCount;
    YCHECK(UnavailableInputChunkCount >= 0);

    if (UnavailableInputChunkCount == 0) {
        InputChunkScraper->Stop();
    }

    // Update replicas in place for all input chunks with current chunkId.
    for (auto& chunkSpec : descriptor.ChunkSpecs) {
        chunkSpec->mutable_replicas()->Clear();
        ToProto(chunkSpec->mutable_replicas(), replicas);
    }

    descriptor.State = EInputChunkState::Active;

    for (const auto& inputStripe : descriptor.InputStripes) {
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

    LOG_TRACE("Input chunk is unavailable (ChunkId: %v)", chunkId);

    ++UnavailableInputChunkCount;

    switch (Spec->UnavailableChunkTactics) {
        case EUnavailableChunkAction::Fail:
            OnOperationFailed(TError("Input chunk %v is unavailable",
                chunkId));
            break;

        case EUnavailableChunkAction::Skip: {
            descriptor.State = EInputChunkState::Skipped;
            for (const auto& inputStripe : descriptor.InputStripes) {
                inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);

                // Remove given chunk from the stripe list.
                SmallVector<TChunkSlicePtr, 1> slices;
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
            InputChunkScraper->Start();
            break;
        }

        case EUnavailableChunkAction::Wait: {
            descriptor.State = EInputChunkState::Waiting;
            for (const auto& inputStripe : descriptor.InputStripes) {
                if (inputStripe.Stripe->WaitingChunkCount == 0) {
                    inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);
                }
                ++inputStripe.Stripe->WaitingChunkCount;
            }
            InputChunkScraper->Start();
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

    LOG_DEBUG("Job is lost (Address: %v, JobId: %v, SourceTask: %v, OutputCookie: %v, InputCookie: %v)",
        completedJob->Address,
        completedJob->JobId,
        completedJob->SourceTask->GetId(),
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

    SetState(EControllerState::Finished);

    CancelableContext->Cancel();

    LOG_INFO("Operation aborted");
}

void TOperationControllerBase::CheckTimeLimit()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto timeLimit = Config->OperationTimeLimit;
    if (Spec->TimeLimit) {
        timeLimit = Spec->TimeLimit;
    }

    if (timeLimit) {
        if (TInstant::Now() - Operation->GetStartTime() > *timeLimit) {
            OnOperationFailed(TError("Operation is running for too long, aborted")
                << TErrorAttribute("time_limit", *timeLimit));
        }
    }
}

TJobId TOperationControllerBase::ScheduleJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (Spec->TestingOperationOptions) {
        Sleep(Spec->TestingOperationOptions->SchedulingDelay);
    }

    if (!IsRunning()) {
        LOG_TRACE("Operation is not running, scheduling request ignored");
        return NullJobId;
    }

    if (GetPendingJobCount() == 0) {
        LOG_TRACE("No pending jobs left, scheduling request ignored");
        return NullJobId;
    }

    auto jobId = DoScheduleJob(context, jobLimits);
    if (jobId) {
        OnJobStarted(jobId);
    }
    return jobId;
}

void TOperationControllerBase::CustomizeJoblet(TJobletPtr /* joblet */)
{ }

void TOperationControllerBase::CustomizeJobSpec(TJobletPtr /* joblet */, TJobSpec* /* jobSpec */)
{ }

void TOperationControllerBase::RegisterTask(TTaskPtr task)
{
    Tasks.push_back(std::move(task));
}

void TOperationControllerBase::RegisterTaskGroup(TTaskGroupPtr group)
{
    TaskGroups.push_back(std::move(group));
}

void TOperationControllerBase::UpdateTask(TTaskPtr task)
{
    int oldPendingJobCount = CachedPendingJobCount;
    int newPendingJobCount = CachedPendingJobCount + task->GetPendingJobCountDelta();
    CachedPendingJobCount = newPendingJobCount;

    int oldTotalJobCount = JobCounter.GetTotal();
    JobCounter.Increment(task->GetTotalJobCountDelta());
    int newTotalJobCount = JobCounter.GetTotal();

    IncreaseNeededResources(task->GetTotalNeededResourcesDelta());

    LOG_DEBUG_IF(
        newPendingJobCount != oldPendingJobCount || newTotalJobCount != oldTotalJobCount,
        "Task updated (Task: %v, PendingJobCount: %v -> %v, TotalJobCount: %v -> %v, NeededResources: {%v})",
        task->GetId(),
        oldPendingJobCount,
        newPendingJobCount,
        oldTotalJobCount,
        newTotalJobCount,
        FormatResources(CachedNeededResources));

    task->CheckCompleted();
}

void TOperationControllerBase::UpdateAllTasks()
{
    for (auto& task: Tasks) {
        task->ResetCachedMinNeededResources();
        UpdateTask(task);
    }
}

void TOperationControllerBase::MoveTaskToCandidates(
    TTaskPtr task,
    std::multimap<i64, TTaskPtr>& candidateTasks)
{
    const auto& neededResources = task->GetMinNeededResources();
    task->CheckResourceDemandSanity(neededResources);
    i64 minMemory = neededResources.memory();
    candidateTasks.insert(std::make_pair(minMemory, task));
    LOG_DEBUG("Task moved to candidates (Task: %v, MinMemory: %v)",
        task->GetId(),
        minMemory / (1024 * 1024));

}

void TOperationControllerBase::AddTaskPendingHint(TTaskPtr task)
{
    if (task->GetPendingJobCount() > 0) {
        auto group = task->GetGroup();
        if (group->NonLocalTasks.insert(task).second) {
            LOG_DEBUG("Task pending hint added (Task: %v)", task->GetId());
            MoveTaskToCandidates(task, group->CandidateTasks);
        }
    }
    UpdateTask(task);
}

void TOperationControllerBase::AddAllTaskPendingHints()
{
    for (const auto& task : Tasks) {
        AddTaskPendingHint(task);
    }
}

void TOperationControllerBase::DoAddTaskLocalityHint(TTaskPtr task, TNodeId nodeId)
{
    auto group = task->GetGroup();
    if (group->NodeIdToTasks[nodeId].insert(task).second) {
        LOG_TRACE("Task locality hint added (Task: %v, Address: %v)",
            task->GetId(),
            InputNodeDirectory->GetDescriptor(nodeId).GetDefaultAddress());
    }
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, TNodeId nodeId)
{
    DoAddTaskLocalityHint(task, nodeId);
    UpdateTask(task);
}

void TOperationControllerBase::AddTaskLocalityHint(TTaskPtr task, TChunkStripePtr stripe)
{
    for (const auto& chunkSlice : stripe->ChunkSlices) {
        for (ui32 protoReplica : chunkSlice->GetChunkSpec()->replicas()) {
            auto replica = FromProto<NChunkClient::TChunkReplica>(protoReplica);
            auto locality = chunkSlice->GetLocality(replica.GetIndex());
            if (locality > 0) {
                DoAddTaskLocalityHint(task, replica.GetNodeId());
            }
        }
    }
    UpdateTask(task);
}

void TOperationControllerBase::ResetTaskLocalityDelays()
{
    LOG_DEBUG("Task locality delays are reset");
    for (auto group : TaskGroups) {
        for (const auto& pair : group->DelayedTasks) {
            auto task = pair.second;
            if (task->GetPendingJobCount() > 0) {
                MoveTaskToCandidates(task, group->CandidateTasks);
            }
        }
        group->DelayedTasks.clear();
    }
}

bool TOperationControllerBase::CheckJobLimits(
    TTaskPtr task,
    const TNodeResources& jobLimits,
    const TNodeResources& nodeResourceLimits)
{
    auto neededResources = task->GetMinNeededResources();
    if (Dominates(jobLimits, neededResources)) {
        return true;
    }
    task->CheckResourceDemandSanity(nodeResourceLimits, neededResources);
    return false;
}

TJobId TOperationControllerBase::DoScheduleJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto localJobId = DoScheduleLocalJob(context, jobLimits);
    if (localJobId) {
        return localJobId;
    }

    auto nonLocalJobId = DoScheduleNonLocalJob(context, jobLimits);
    if (nonLocalJobId) {
        return nonLocalJobId;
    }

    return NullJobId;
}

TJobId TOperationControllerBase::DoScheduleLocalJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    const auto& nodeResourceLimits = context->ResourceLimits();
    const auto& address = context->GetAddress();
    auto nodeId = context->GetNode()->GetId();

    for (const auto& group : TaskGroups) {
        if (!Dominates(jobLimits, group->MinNeededResources)) {
            continue;
        }

        auto localTasksIt = group->NodeIdToTasks.find(nodeId);
        if (localTasksIt == group->NodeIdToTasks.end()) {
            continue;
        }

        i64 bestLocality = 0;
        TTaskPtr bestTask = nullptr;

        auto& localTasks = localTasksIt->second;
        auto it = localTasks.begin();
        while (it != localTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            // Make sure that the task has positive locality.
            // Remove pending hint if not.
            auto locality = task->GetLocality(nodeId);
            if (locality <= 0) {
                localTasks.erase(jt);
                LOG_TRACE("Task locality hint removed (Task: %v, Address: %v)",
                    task->GetId(),
                    address);
                continue;
            }

            if (locality <= bestLocality) {
                continue;
            }

            if (task->GetPendingJobCount() == 0) {
                UpdateTask(task);
                continue;
            }

            if (!CheckJobLimits(task, jobLimits, nodeResourceLimits)) {
                continue;
            }

            bestLocality = locality;
            bestTask = task;
        }

        if (!IsRunning()) {
            return NullJobId;
        }

        if (bestTask) {
            LOG_DEBUG(
                "Attempting to schedule a local job (Task: %v, Address: %v, Locality: %v, JobLimits: {%v}, "
                "PendingDataSize: %v, PendingJobCount: %v)",
                bestTask->GetId(),
                address,
                bestLocality,
                FormatResources(jobLimits),
                bestTask->GetPendingDataSize(),
                bestTask->GetPendingJobCount());
            auto jobId = bestTask->ScheduleJob(context, jobLimits);
            if (jobId) {
                UpdateTask(bestTask);
                return jobId;
            }
        }
    }
    return NullJobId;
}

TJobId TOperationControllerBase::DoScheduleNonLocalJob(
    ISchedulingContext* context,
    const TNodeResources& jobLimits)
{
    auto now = context->GetNow();
    const auto& nodeResourceLimits = context->ResourceLimits();
    const auto& address = context->GetAddress();

    for (const auto& group : TaskGroups) {
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
                LOG_DEBUG("Task pending hint removed (Task: %v)",
                    task->GetId());
                YCHECK(nonLocalTasks.erase(task) == 1);
                UpdateTask(task);
            } else {
                LOG_DEBUG("Task delay deadline reached (Task: %v)", task->GetId());
                MoveTaskToCandidates(task, candidateTasks);
            }
        }

        // Consider candidates in the order of increasing memory demand.
        {
            int processedTaskCount = 0;
            auto it = candidateTasks.begin();
            while (it != candidateTasks.end()) {
                ++processedTaskCount;
                auto task = it->second;

                // Make sure that the task is ready to launch jobs.
                // Remove pending hint if not.
                if (task->GetPendingJobCount() == 0) {
                    LOG_DEBUG("Task pending hint removed (Task: %v)", task->GetId());
                    candidateTasks.erase(it++);
                    YCHECK(nonLocalTasks.erase(task) == 1);
                    UpdateTask(task);
                    continue;
                }

                // Check min memory demand for early exit.
                if (task->GetMinNeededResources().memory() > jobLimits.memory()) {
                    break;
                }

                if (!CheckJobLimits(task, jobLimits, nodeResourceLimits)) {
                    ++it;
                    continue;
                }

                if (!task->GetDelayedTime()) {
                    task->SetDelayedTime(now);
                }

                auto deadline = *task->GetDelayedTime() + task->GetLocalityTimeout();
                if (deadline > now) {
                    LOG_DEBUG("Task delayed (Task: %v, Deadline: %v)",
                        task->GetId(),
                        deadline);
                    delayedTasks.insert(std::make_pair(deadline, task));
                    candidateTasks.erase(it++);
                    continue;
                }

                if (!IsRunning()) {
                    return NullJobId;
                }

                LOG_DEBUG(
                    "Attempting to schedule a non-local job (Task: %v, Address: %v, JobLimits: {%v}, "
                    "PendingDataSize: %v, PendingJobCount: %v)",
                    task->GetId(),
                    address,
                    FormatResources(jobLimits),
                    task->GetPendingDataSize(),
                    task->GetPendingJobCount());

                auto jobId = task->ScheduleJob(context, jobLimits);
                if (jobId) {
                    UpdateTask(task);
                    LOG_DEBUG("Processed %v tasks", processedTaskCount);
                    return jobId;
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

            LOG_DEBUG("Processed %v tasks", processedTaskCount);
        }
    }
    return NullJobId;
}

TCancelableContextPtr TOperationControllerBase::GetCancelableContext() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableContext;
}

IInvokerPtr TOperationControllerBase::GetCancelableControlInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableControlInvoker;
}

IInvokerPtr TOperationControllerBase::GetCancelableInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableInvoker;
}

IInvokerPtr TOperationControllerBase::GetInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return SuspendableInvoker;
}

TFuture<void> TOperationControllerBase::Suspend()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    return SuspendableInvoker->Suspend();
}

void TOperationControllerBase::Resume()
{
    VERIFY_THREAD_AFFINITY(ControlThread);

    SuspendableInvoker->Resume();
}

int TOperationControllerBase::GetPendingJobCount() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    // Avoid accessing the state while not prepared.
    if (!IsPrepared()) {
        return 0;
    }

    // NB: For suspended operations we still report proper pending job count
    // but zero demand.
    if (!IsRunning()) {
        return 0;
    }

    return CachedPendingJobCount;
}

int TOperationControllerBase::GetTotalJobCount() const
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    // Avoid accessing the state while not prepared.
    if (!IsPrepared()) {
        return 0;
    }

    return JobCounter.GetTotal();
}

void TOperationControllerBase::IncreaseNeededResources(const TNodeResources& resourcesDelta)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TWriterGuard guard(CachedNeededResourcesLock);
    CachedNeededResources += resourcesDelta;
}

TNodeResources TOperationControllerBase::GetNeededResources() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TReaderGuard guard(CachedNeededResourcesLock);
    return CachedNeededResources;
}

void TOperationControllerBase::OnOperationCompleted()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    // This can happen if operation failed during completion in derived class (e.x. SortController).
    if (IsFinished()) {
        return;
    }

    LOG_INFO("Operation completed");

    SetState(EControllerState::Finished);

    Host->OnOperationCompleted(Operation);
}

void TOperationControllerBase::OnOperationFailed(const TError& error)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    // During operation failing job aborting can lead to another operation fail, we don't want to invoke it twice.
    if (IsFinished()) {
        return;
    }

    SetState(EControllerState::Finished);

    Host->OnOperationFailed(Operation, error);
}

void TOperationControllerBase::SetState(EControllerState state)
{
    TWriterGuard guard(StateLock);
    State = state;
}

bool TOperationControllerBase::IsPrepared() const
{
    TReaderGuard guard(StateLock);
    return State != EControllerState::Preparing;
}

bool TOperationControllerBase::IsRunning() const
{
    TReaderGuard guard(StateLock);
    return State == EControllerState::Running;
}

bool TOperationControllerBase::IsFinished() const
{
    TReaderGuard guard(StateLock);
    return State == EControllerState::Finished;
}

void TOperationControllerBase::CreateLivePreviewTables()
{
    // NB: use root credentials.
    auto channel = Host->GetMasterClient()->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    auto addRequest = [&] (
            const Stroka& path,
            int replicationFactor,
            const Stroka& key,
            const TYsonString& acl)
    {
        {
            auto req = TCypressYPathProxy::Create(path);

            req->set_type(static_cast<int>(EObjectType::Table));
            req->set_ignore_existing(true);

            auto attributes = CreateEphemeralAttributes();
            attributes->Set("replication_factor", replicationFactor);

            ToProto(req->mutable_node_attributes(), *attributes);

            batchReq->AddRequest(req, key);
        }

        {
            auto req = TYPathProxy::Set(path + "/@acl");
            req->set_value(acl.Data());

            batchReq->AddRequest(req, key);
        }

        {
            auto req = TYPathProxy::Set(path + "/@inherit_acl");
            req->set_value(ConvertToYsonString(false).Data());

            batchReq->AddRequest(req, key);
        }
    };

    if (IsOutputLivePreviewSupported()) {
        LOG_INFO("Creating output tables for live preview");

        for (int index = 0; index < OutputTables.size(); ++index) {
            const auto& table = OutputTables[index];
            auto path = GetLivePreviewOutputPath(OperationId, index);
            addRequest(path, table.Options->ReplicationFactor, "create_output", OutputTables[index].EffectiveAcl);
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        LOG_INFO("Creating intermediate table for live preview");

        auto path = GetLivePreviewIntermediatePath(OperationId);
        addRequest(path, 1, "create_intermediate", ConvertToYsonString(Spec->IntermediateDataAcl));
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error creating live preview tables");
    const auto& batchRsp = batchRspOrError.Value();

    auto handleResponse = [&] (TLivePreviewTableBase& table, TCypressYPathProxy::TRspCreatePtr rsp) {
        table.LivePreviewTableId = FromProto<NCypressClient::TNodeId>(rsp->node_id());
    };

    if (IsOutputLivePreviewSupported()) {
        auto rspsOrError = batchRsp->GetResponses<TCypressYPathProxy::TRspCreate>("create_output");
        YCHECK(rspsOrError.size() == 3 * OutputTables.size());
        for (int index = 0; index < OutputTables.size(); ++index) {
            handleResponse(OutputTables[index], rspsOrError[3 * index].Value());
        }

        LOG_INFO("Output live preview tables created");
    }

    if (IsIntermediateLivePreviewSupported()) {
        auto rspsOrError = batchRsp->GetResponses<TCypressYPathProxy::TRspCreate>("create_intermediate");
        handleResponse(IntermediateTable, rspsOrError[0].Value());

        LOG_INFO("Intermediate live preview table created");
    }
}

void TOperationControllerBase::PrepareLivePreviewTablesForUpdate()
{
    // XXX(babenko): fixme
    //// NB: use root credentials.
    //auto channel = Host->GetMasterClient()->GetMasterChannel(EMasterChannelKind::Leader);
    //TObjectServiceProxy proxy(channel);
    //
    //auto batchReq = proxy.ExecuteBatch();
    //
    //// XXX(babenko): multicell
    //auto addRequest = [&] (const TLivePreviewTableBase& table, const Stroka& key) {
    //    auto req = TTableYPathProxy::PrepareForUpdate(FromObjectId(table.LivePreviewTableId));
    //    req->set_update_mode(static_cast<int>(EUpdateMode::Overwrite));
    //    req->set_lock_mode(static_cast<int>(ELockMode::Exclusive));
    //    SetTransactionId(req, AsyncSchedulerTransactionId);
    //    batchReq->AddRequest(req, key);
    //};
    //
    //if (IsOutputLivePreviewSupported()) {
    //    LOG_INFO("Preparing live preview output tables for update");
    //
    //    for (const auto& table : OutputTables) {
    //        addRequest(table, "prepare_output");
    //    }
    //}
    //
    //if (IsIntermediateLivePreviewSupported()) {
    //    LOG_INFO("Preparing live preview intermediate table for update");
    //
    //    addRequest(IntermediateTable, "prepare_intermediate");
    //}
    //
    //auto batchRspOrError = WaitFor(batchReq->Invoke());
    //THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error preparing live preview tables for update");
    //const auto& batchRsp = batchRspOrError.Value();
    //
    //auto handleResponse = [&] (TLivePreviewTableBase& table, TTableYPathProxy::TRspPrepareForUpdatePtr rsp) {
    //    table.LivePreviewChunkListId = FromProto<NCypressClient::TNodeId>(rsp->chunk_list_id());
    //};
    //
    //if (IsOutputLivePreviewSupported()) {
    //    auto rspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspPrepareForUpdate>("prepare_output");
    //    YCHECK(rspsOrError.size() == OutputTables.size());
    //    for (int index = 0; index < OutputTables.size(); ++index) {
    //        handleResponse(OutputTables[index], rspsOrError[index].Value());
    //    }
    //
    //    LOG_INFO("Output live preview tables prepared for update");
    //}
    //
    //if (IsIntermediateLivePreviewSupported()) {
    //    auto rspOrError = batchRsp->GetResponse<TTableYPathProxy::TRspPrepareForUpdate>("prepare_intermediate");
    //    handleResponse(IntermediateTable, rspOrError.Value());
    //
    //    LOG_INFO("Intermediate live preview table prepared for update");
    //}
}

void TOperationControllerBase::GetInputTablesBasicAttributes()
{
    LOG_INFO("Getting basic attributes of input tables");

    auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& table : InputTables) {
        auto req = TTableYPathProxy::GetBasicAttributes(table.Path.GetPath());
        req->set_permissions(static_cast<ui32>(EPermission::Read));
        SetTransactionId(req, InputTransactionId);
        batchReq->AddRequest(req, "get_basic_attributes");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting basic attributes of input tables");
    const auto& batchRsp = batchRspOrError.Value();

    auto rspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGetBasicAttributes>("get_basic_attributes");
    for (int index = 0; index < InputTables.size(); ++index) {
        auto& table = InputTables[index];
        auto path = table.Path.GetPath();

        {
            const auto& rspOrError = rspsOrError[index];
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting basic attributes of input table %v",
                path);
            const auto& rsp = rspOrError.Value();

            table.ObjectId = FromProto<TObjectId>(rsp->object_id());
            table.CellTag = rsp->cell_tag();

            auto type = TypeFromId(table.ObjectId);
            if (type != EObjectType::Table) {
                THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv, actual %Qlv",
                    table.Path.GetPath(),
                    EObjectType::Table,
                    type);
            }

            LOG_INFO("Basic attributes of input table received (Path: %v, ObjectId: %v, CellTag: %v)",
                path,
                table.ObjectId,
                table.CellTag);
        }
    }
}

void TOperationControllerBase::GetOutputTablesBasicAttributes()
{
    LOG_INFO("Getting basic attributes of output tables");

    auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& table : OutputTables) {
        auto req = TTableYPathProxy::GetBasicAttributes(table.Path.GetPath());
        req->set_permissions(static_cast<ui32>(EPermission::Write));
        SetTransactionId(req, OutputTransactionId);
        batchReq->AddRequest(req, "get_basic_attributes");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting basic attributes of output tables");
    const auto& batchRsp = batchRspOrError.Value();

    auto rspsOrError = batchRsp->GetResponses<TObjectYPathProxy::TRspGetBasicAttributes>("get_basic_attributes");
    for (int index = 0; index < OutputTables.size(); ++index) {
        auto& table = OutputTables[index];
        const auto& path = table.Path.GetPath();
        {
            const auto& rspOrError = rspsOrError[index];
            THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting basic attributes of output table %v",
                path);
            const auto& rsp = rspOrError.Value();

            table.ObjectId = FromProto<TObjectId>(rsp->object_id());
            table.CellTag = rsp->cell_tag();

            auto type = TypeFromId(table.ObjectId);
            if (type != EObjectType::Table) {
                THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv, actual %Qlv",
                    table.Path.GetPath(),
                    EObjectType::Table,
                    type);
            }

            LOG_INFO("Basic attributes of output table received (Path: %v, ObjectId: %v, CellTag: %v)",
                path,
                table.ObjectId,
                table.CellTag);
        }
    }
}

void TOperationControllerBase::GetFilesBasicAttributes(std::vector<TUserFile>* files)
{
    LOG_INFO("Getting basic attributes of files");

    auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& file : *files) {
        auto req = TObjectYPathProxy::GetBasicAttributes(file.Path.GetPath());
        req->set_permissions(static_cast<ui32>(EPermission::Read));
        SetTransactionId(req, InputTransactionId);
        batchReq->AddRequest(req, "get_basic_attributes");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting basic attributes of files");
    const auto& batchRsp = batchRspOrError.Value();

    auto rspsOrError = batchRsp->GetResponses<TObjectYPathProxy::TRspGetBasicAttributes>("get_basic_attributes");
    for (int index = 0; index < files->size(); ++index) {
        auto& file = (*files)[index];
        const auto& path = file.Path.GetPath();
        const auto& rspOrError = rspsOrError[index];
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting basic attributes of file %v",
            path);
        const auto& rsp = rspOrError.Value();

        file.ObjectId = FromProto<TObjectId>(rsp->object_id());
        file.CellTag = rsp->cell_tag();

        file.Type = TypeFromId(file.ObjectId);
        if (file.Type != EObjectType::File && file.Type != EObjectType::Table) {
            THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv or %Qlv, actual %Qlv",
                path,
                EObjectType::File,
                EObjectType::Table,
                file.Type);
        }
    }
}

void TOperationControllerBase::FetchInputTables()
{
    for (int tableIndex = 0; tableIndex < static_cast<int>(InputTables.size()); ++tableIndex) {
        auto& table = InputTables[tableIndex];
        auto objectIdPath = FromObjectId(table.ObjectId);
        const auto& path = table.Path.GetPath();
        const auto& ranges = table.Path.GetRanges();
        if (ranges.empty())
            continue;

        LOG_INFO("Fetching input table (Path: %v, RangeCount: %v)",
            path,
            ranges.size());

        auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(
            EMasterChannelKind::LeaderOrFollower, table.CellTag);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        for (const auto& range : ranges) {
            for (i64 index = 0; index * Config->MaxChunksPerFetch < table.ChunkCount; ++index) {
                auto adjustedRange = range;
                auto chunkCountLowerLimit = index * Config->MaxChunksPerFetch;
                if (adjustedRange.LowerLimit().HasChunkIndex()) {
                    chunkCountLowerLimit = std::max(chunkCountLowerLimit, adjustedRange.LowerLimit().GetChunkIndex());
                }
                adjustedRange.LowerLimit().SetChunkIndex(chunkCountLowerLimit);

                auto chunkCountUpperLimit = (index + 1) * Config->MaxChunksPerFetch;
                if (adjustedRange.UpperLimit().HasChunkIndex()) {
                    chunkCountUpperLimit = std::min(chunkCountUpperLimit, adjustedRange.UpperLimit().GetChunkIndex());
                }
                adjustedRange.UpperLimit().SetChunkIndex(chunkCountUpperLimit);

                auto req = TTableYPathProxy::Fetch(FromObjectId(table.ObjectId));
                InitializeFetchRequest(req.Get(), table.Path);
                ToProto(req->mutable_ranges(), std::vector<TReadRange>({adjustedRange}));
                req->set_fetch_all_meta_extensions(false);
                req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                if (IsBoundaryKeysFetchEnabled()) {
                    req->add_extension_tags(TProtoExtensionTag<NTableClient::NProto::TBoundaryKeysExt>::Value);
                }
                req->set_fetch_parity_replicas(IsParityReplicasFetchEnabled());
                SetTransactionId(req, InputTransactionId);
                batchReq->AddRequest(req, "fetch");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error fetching input table %v",
            path);
        const auto& batchRsp = batchRspOrError.Value();

        auto rspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspFetch>("fetch");
        for (const auto& rspOrError : rspsOrError) {
            const auto& rsp = rspOrError.Value();
            std::vector<NChunkClient::NProto::TChunkSpec> chunkSpecs;
            ProcessFetchResponse(
                AuthenticatedInputMasterClient,
                rsp,
                table.CellTag,
                InputNodeDirectory,
                Config->MaxChunksPerLocateRequest,
                Logger,
                &chunkSpecs);

            for (auto& chunk : chunkSpecs) {
                auto chunkSpec = New<TRefCountedChunkSpec>(std::move(chunk));
                chunkSpec->set_table_index(tableIndex);
                table.Chunks.push_back(chunkSpec);
            }
        }

        LOG_INFO("Input table fetched (Path: %v, ChunkCount: %v)",
            path,
            table.Chunks.size());
    }
}

void TOperationControllerBase::LockInputTables()
{
    LOG_INFO("Locking input tables");

    {
        auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        for (const auto& table : InputTables) {
            auto objectIdPath = FromObjectId(table.ObjectId);
            {
                auto req = TTableYPathProxy::Lock(objectIdPath);
                req->set_mode(static_cast<int>(ELockMode::Snapshot));
                SetTransactionId(req, InputTransactionId);
                GenerateMutationId(req);
                batchReq->AddRequest(req, "lock");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error locking input tables");
    }

    LOG_INFO("Getting input tables attributes");

    {
        auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        for (const auto& table : InputTables) {
            auto objectIdPath = FromObjectId(table.ObjectId);
            {
                auto req = TTableYPathProxy::Get(objectIdPath);
                TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
                attributeFilter.Keys.push_back("dynamic");
                attributeFilter.Keys.push_back("sorted");
                attributeFilter.Keys.push_back("sorted_by");
                attributeFilter.Keys.push_back("chunk_count");
                ToProto(req->mutable_attribute_filter(), attributeFilter);
                SetTransactionId(req, InputTransactionId);
                batchReq->AddRequest(req, "get_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting attributes of input tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto lockInRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspLock>("lock");
        auto getInAttributesRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGet>("get_attributes");
        for (int index = 0; index < InputTables.size(); ++index) {
            auto& table = InputTables[index];
            auto path = table.Path.GetPath();
            {
                const auto& rsp = getInAttributesRspsOrError[index].Value();
                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();

                if (attributes.Get<bool>("dynamic")) {
                    THROW_ERROR_EXCEPTION("Expected a static table, but got dynamic")
                        << TErrorAttribute("input_table", path);
                }

                if (attributes.Get<bool>("sorted")) {
                    table.KeyColumns = attributes.Get<TKeyColumns>("sorted_by");
                }

                table.ChunkCount = attributes.Get<int>("chunk_count");
            }
            LOG_INFO("Input table locked (Path: %v, KeyColumns: [%v], ChunkCount: %v)",
                path,
                JoinToString(table.KeyColumns),
                table.ChunkCount);
        }
    }
}

void TOperationControllerBase::BeginUploadOutputTables()
{
    LOG_INFO("Locking output tables");

    {
        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        for (const auto& table : OutputTables) {
            auto objectIdPath = FromObjectId(table.ObjectId);
            {
                auto req = TTableYPathProxy::Lock(objectIdPath);
                req->set_mode(static_cast<int>(table.LockMode));
                GenerateMutationId(req);
                SetTransactionId(req, OutputTransactionId);
                batchReq->AddRequest(req, "lock");
            }
            {
                auto req = TTableYPathProxy::BeginUpload(objectIdPath);
                SetTransactionId(req, OutputTransactionId);
                GenerateMutationId(req);
                req->set_update_mode(static_cast<int>(table.UpdateMode));
                req->set_lock_mode(static_cast<int>(table.LockMode));
                batchReq->AddRequest(req, "begin_upload");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error locking output tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto beginUploadRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspBeginUpload>("begin_upload");
        for (int index = 0; index < OutputTables.size(); ++index) {
            auto& table = OutputTables[index];
            {
                const auto& rsp = beginUploadRspsOrError[index].Value();
                table.UploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());
            }
        }
    }

    LOG_INFO("Getting output tables attributes");

    {
        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto& table : OutputTables) {
            auto objectIdPath = FromObjectId(table.ObjectId);
            {
                auto req = TTableYPathProxy::Get(objectIdPath);
                TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
                attributeFilter.Keys.push_back("channels");
                attributeFilter.Keys.push_back("compression_codec");
                attributeFilter.Keys.push_back("erasure_codec");
                attributeFilter.Keys.push_back("row_count");
                attributeFilter.Keys.push_back("replication_factor");
                attributeFilter.Keys.push_back("account");
                attributeFilter.Keys.push_back("vital");
                attributeFilter.Keys.push_back("effective_acl");
                ToProto(req->mutable_attribute_filter(), attributeFilter);
                SetTransactionId(req, OutputTransactionId);
                batchReq->AddRequest(req, "get_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting attributes of output tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto getOutAttributesRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGet>("get_attributes");
        for (int index = 0; index < OutputTables.size(); ++index) {
            auto& table = OutputTables[index];
            const auto& path = table.Path.GetPath();
            {
                const auto& rsp = getOutAttributesRspsOrError[index].Value();
                auto node = ConvertToNode(TYsonString(rsp->value()));
                const auto& attributes = node->Attributes();

                if (attributes.Get<i64>("row_count") > 0 &&
                    table.AppendRequested &&
                    table.UpdateMode == EUpdateMode::Overwrite) {
                    THROW_ERROR_EXCEPTION("Cannot append sorted data to non-empty output table %v",
                        path);
                }

                table.Options->Channels = attributes.Get<NChunkClient::TChannels>("channels", TChannels());
                table.Options->CompressionCodec = attributes.Get<NCompression::ECodec>("compression_codec");
                table.Options->ErasureCodec = attributes.Get<NErasure::ECodec>("erasure_codec", NErasure::ECodec::None);
                table.Options->ReplicationFactor = attributes.Get<int>("replication_factor");
                table.Options->Account = attributes.Get<Stroka>("account");
                table.Options->ChunksVital = attributes.Get<bool>("vital");

                table.EffectiveAcl = attributes.GetYson("effective_acl");
            }
            LOG_INFO("Output table locked (Path: %v, Options: %v, UploadTransactionId: %v)",
                path,
                ConvertToYsonString(table.Options, EYsonFormat::Text).Data(),
                table.UploadTransactionId);
        }
    }
}

void TOperationControllerBase::GetOutputTablesUploadParams()
{
    yhash<TCellTag, std::vector<TOutputTable*>> cellTagToTables;
    for (auto& table : OutputTables) {
        cellTagToTables[table.CellTag].push_back(&table);
    }

    for (const auto& pair : cellTagToTables) {
        auto cellTag = pair.first;
        const auto& tables = pair.second;

        LOG_INFO("Getting output tables upload parameters (CellTag: %v)", cellTag);

        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(
            EMasterChannelKind::LeaderOrFollower,
            cellTag);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();
        for (const auto& table : tables) {
            auto objectIdPath = FromObjectId(table->ObjectId);
            {
                auto req = TTableYPathProxy::GetUploadParams(objectIdPath);
                SetTransactionId(req, table->UploadTransactionId);
                batchReq->AddRequest(req, "get_upload_params");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting upload parameters of output tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto getUploadParamsRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGetUploadParams>("get_upload_params");
        for (int index = 0; index < tables.size(); ++index) {
            auto* table = tables[index];
            const auto& path = table->Path.GetPath();
            {
                const auto& rspOrError = getUploadParamsRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting upload parameters of output table %v",
                    path);

                const auto& rsp = rspOrError.Value();
                table->OutputChunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());

                LOG_INFO("Upload parameters of output table received (Path: %v, ChunkListId: %v)",
                    path,
                    table->OutputChunkListId);
            }
        }
    }
}

void TOperationControllerBase::FetchUserFiles(std::vector<TUserFile>* files)
{
    for (auto& file : *files) {
        auto objectIdPath = FromObjectId(file.ObjectId);
        const auto& path = file.Path.GetPath();

        LOG_INFO("Fetching user file (Path: %v)",
            path);

        auto channel = AuthenticatedInputMasterClient->GetMasterChannelOrThrow(
            EMasterChannelKind::LeaderOrFollower,
            file.CellTag);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        {
            auto req = TChunkOwnerYPathProxy::Fetch(objectIdPath);
            ToProto(req->mutable_ranges(), std::vector<TReadRange>({TReadRange()}));
            switch (file.Type) {
                case EObjectType::Table:
                    req->set_fetch_all_meta_extensions(true);
                    InitializeFetchRequest(req.Get(), file.Path);
                    break;

                case EObjectType::File:
                    req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                    break;

                default:
                    YUNREACHABLE();
            }
            SetTransactionId(req, InputTransactionId);
            batchReq->AddRequest(req, "fetch");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error fetching user file %v",
             path);
        const auto& batchRsp = batchRspOrError.Value();

        {
            auto rsp = batchRsp->GetResponse<TChunkOwnerYPathProxy::TRspFetch>("fetch").Value();
            ProcessFetchResponse(
                AuthenticatedInputMasterClient,
                rsp,
                file.CellTag,
                AuxNodeDirectory,
                Config->MaxChunksPerLocateRequest,
                Logger,
                &file.ChunkSpecs);
        }

        LOG_INFO("User file fetched (Path: %v, FileName: %v)",
            path,
            file.FileName);
    }
}

void TOperationControllerBase::LockUserFiles(
    std::vector<TUserFile>* files,
    const std::vector<Stroka>& attributeKeys)
{
    LOG_INFO("Locking user files");

    {
        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto& file : *files) {
            auto objectIdPath = FromObjectId(file.ObjectId);

            {
                auto req = TCypressYPathProxy::Lock(objectIdPath);
                req->set_mode(static_cast<int>(ELockMode::Snapshot));
                GenerateMutationId(req);
                SetTransactionId(req, InputTransactionId);
                batchReq->AddRequest(req, "lock");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error locking user files");
    }

    LOG_INFO("Getting user files attributes");

    {
        auto channel = AuthenticatedOutputMasterClient->GetMasterChannelOrThrow(EMasterChannelKind::LeaderOrFollower);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto& file : *files) {
            auto objectIdPath = FromObjectId(file.ObjectId);
            {
                auto req = TYPathProxy::Get(objectIdPath);
                SetTransactionId(req, InputTransactionId);
                TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
                if (file.Type == EObjectType::File) {
                    attributeFilter.Keys.push_back("executable");
                    attributeFilter.Keys.push_back("file_name");
                }
                attributeFilter.Keys.push_back("key");
                attributeFilter.Keys.push_back("chunk_count");
                attributeFilter.Keys.push_back("uncompressed_data_size");
                attributeFilter.Keys.insert(attributeFilter.Keys.end(), attributeKeys.begin(), attributeKeys.end());
                ToProto(req->mutable_attribute_filter(), attributeFilter);
                batchReq->AddRequest(req, "get_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting attributes of user files");
        const auto& batchRsp = batchRspOrError.Value();

        TEnumIndexedVector<yhash_set<Stroka>, EOperationStage> userFileNames;
        auto validateUserFileName = [&] (const TUserFile& file) {
            // TODO(babenko): more sanity checks?
            auto path = file.Path.GetPath();
            const auto& fileName = file.FileName;
            if (fileName.empty()) {
                THROW_ERROR_EXCEPTION("Empty user file name for %v",
                    path);
            }
            if (!userFileNames[file.Stage].insert(fileName).second) {
                THROW_ERROR_EXCEPTION("Duplicate user file name %Qv for %v",
                    fileName,
                    path);
            }
        };

        auto getAttributesRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_attributes");
        for (int index = 0; index < files->size(); ++index) {
            auto& file = (*files)[index];
            const auto& path = file.Path.GetPath();

            {
                const auto& rsp = getAttributesRspsOrError[index].Value();

                auto node = ConvertToNode(TYsonString(rsp->value()));
                file.Attributes = node->Attributes().Clone();
                const auto& attributes = *file.Attributes;

                file.FileName = attributes.Get<Stroka>("key");
                file.FileName = attributes.Get<Stroka>("file_name", file.FileName);
                file.FileName = file.Path.FindFileName().Get(file.FileName);

                switch (file.Type) {
                    case EObjectType::File:
                        file.Executable = attributes.Get<bool>("executable", false);
                        break;

                    case EObjectType::Table:
                        file.Format = file.Path.Attributes().GetYson("format");
                        break;

                    default:
                        YUNREACHABLE();
                }

                i64 fileSize = attributes.Get<i64>("uncompressed_data_size");
                if (fileSize > Config->MaxFileSize) {
                    THROW_ERROR_EXCEPTION(
                        "User file %v exceeds size limit: %v > %v",
                        path,
                        fileSize,
                        Config->MaxFileSize);
                }

                i64 chunkCount = attributes.Get<i64>("chunk_count");
                if (chunkCount > Config->MaxChunksPerFetch) {
                    THROW_ERROR_EXCEPTION(
                        "User file %v exceeds chunk count limit: %v > %v",
                        path,
                        chunkCount,
                        Config->MaxChunksPerFetch);
                }

                LOG_INFO("User file locked (Path: %v, Stage: %v, FileName: %v)",
                    path,
                    file.Stage,
                    file.FileName);
            }

            validateUserFileName(file);
        }
    }
}

void TOperationControllerBase::InitQuerySpec(
    NProto::TSchedulerJobSpecExt* schedulerJobSpecExt,
    const Stroka& queryString,
    const TTableSchema& schema)
{
    auto* querySpec = schedulerJobSpecExt->mutable_input_query_spec();
    auto ast = PrepareJobQueryAst(queryString);
    auto registry = CreateBuiltinFunctionRegistry();
    auto externalFunctions = GetExternalFunctions(ast, registry);

    std::vector<TUserFile> udfFiles;
    std::vector<TUdfDescriptorPtr> udfDescriptors;

    if (!externalFunctions.empty()) {
        if (!Config->UdfRegistryPath) {
            THROW_ERROR_EXCEPTION("External UDF registry is not configured");
        }

        for (const auto& function : externalFunctions) {
            LOG_INFO("Requesting UDF descriptor (Function: %v)", function);
            TUserFile file;
            file.Path = GetUdfDescriptorPath(*Config->UdfRegistryPath, function);
            udfFiles.push_back(file);
        }

        GetFilesBasicAttributes(&udfFiles);

        LockUserFiles(
            &udfFiles,
            {
                FunctionDescriptorAttribute,
                AggregateDescriptorAttribute
            });

        FetchUserFiles(&udfFiles);

        for (const auto& file : udfFiles) {
            if (file.Type != EObjectType::File) {
                THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv, actual %Qlv",
                    file.Path,
                    EObjectType::File,
                    file.Type);
            }
            auto descriptor = New<TUdfDescriptor>();
            descriptor->Name = file.FileName;
            descriptor->FunctionDescriptor = file.Attributes->Find<TCypressFunctionDescriptorPtr>(FunctionDescriptorAttribute);
            descriptor->AggregateDescriptor = file.Attributes->Find<TCypressAggregateDescriptorPtr>(AggregateDescriptorAttribute);
            udfDescriptors.push_back(std::move(descriptor));
        }

        registry = CreateJobFunctionRegistry(udfDescriptors, Null, std::move(registry));
    }

    auto query = PrepareJobQuery(queryString, std::move(ast), schema, registry);
    ToProto(querySpec->mutable_query(), query);

    for (const auto& descriptor : udfDescriptors) {
        auto* protoDescriptor = querySpec->add_udf_descriptors();
        ToProto(protoDescriptor, ConvertToYsonString(descriptor).Data());
    }

    for (const auto& file : udfFiles) {
        auto* protoDescriptor = querySpec->add_udf_files();
        protoDescriptor->set_type(static_cast<int>(file.Type));
        protoDescriptor->set_file_name(file.FileName);
        ToProto(protoDescriptor->mutable_chunks(), file.ChunkSpecs);
    }
}

void TOperationControllerBase::CollectTotals()
{
    for (const auto& table : InputTables) {
        for (const auto& chunkSpec : table.Chunks) {
            if (IsUnavailable(*chunkSpec, IsParityReplicasFetchEnabled())) {
                auto chunkId = FromProto<TChunkId>(chunkSpec->chunk_id());
                switch (Spec->UnavailableChunkStrategy) {
                    case EUnavailableChunkAction::Fail:
                        THROW_ERROR_EXCEPTION("Input chunk %v is unavailable",
                            chunkId);

                    case EUnavailableChunkAction::Skip:
                        LOG_TRACE("Skipping unavailable chunk (ChunkId: %v)",
                            chunkId);
                        continue;

                    case EUnavailableChunkAction::Wait:
                        // Do nothing.
                        break;

                    default:
                        YUNREACHABLE();
                }
            }
            i64 chunkDataSize;
            i64 chunkRowCount;
            i64 chunkValueCount;
            i64 chunkCompressedDataSize;
            NChunkClient::GetStatistics(*chunkSpec, &chunkDataSize, &chunkRowCount, &chunkValueCount, &chunkCompressedDataSize);

            TotalEstimatedInputDataSize += chunkDataSize;
            TotalEstimatedInputRowCount += chunkRowCount;
            TotalEstimatedInputValueCount += chunkValueCount;
            TotalEstimatedCompressedDataSize += chunkCompressedDataSize;
            ++TotalEstimatedInputChunkCount;
        }
    }

    LOG_INFO("Estimated input totals collected (ChunkCount: %v, DataSize: %v, RowCount: %v, ValueCount: %v, CompressedDataSize: %v)",
        TotalEstimatedInputChunkCount,
        TotalEstimatedInputDataSize,
        TotalEstimatedInputRowCount,
        TotalEstimatedInputValueCount,
        TotalEstimatedCompressedDataSize);
}

void TOperationControllerBase::CustomPrepare()
{ }

// NB: must preserve order of chunks in the input tables, no shuffling.
std::vector<TRefCountedChunkSpecPtr> TOperationControllerBase::CollectInputChunks() const
{
    std::vector<TRefCountedChunkSpecPtr> result;
    for (const auto& table : InputTables) {
        for (const auto& chunkSpec : table.Chunks) {
            if (IsUnavailable(*chunkSpec, IsParityReplicasFetchEnabled())) {
                switch (Spec->UnavailableChunkStrategy) {
                    case EUnavailableChunkAction::Skip:
                        continue;

                    case EUnavailableChunkAction::Wait:
                        // Do nothing.
                        break;

                    default:
                        YUNREACHABLE();
                }
            }
            result.push_back(chunkSpec);
        }
    }
    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::SliceChunks(
    const std::vector<TRefCountedChunkSpecPtr>& chunkSpecs,
    i64 maxSliceDataSize,
    int* jobCount)
{
    std::vector<TChunkStripePtr> result;
    auto appendStripes = [&] (std::vector<TChunkSlicePtr> slices) {
        for (const auto& slice : slices) {
            result.push_back(New<TChunkStripe>(slice));
        }
    };

    // TODO(ignat): we slice on two parts even if TotalEstimatedInputDataSize very small.
    i64 sliceDataSize = std::min(
        maxSliceDataSize,
        (i64)std::max(Config->SliceDataSizeMultiplier * TotalEstimatedInputDataSize / *jobCount, 1.0));

    for (const auto& chunkSpec : chunkSpecs) {
        int oldSize = result.size();

        bool hasNontrivialLimits = !IsCompleteChunk(*chunkSpec);

        auto codecId = NErasure::ECodec(chunkSpec->erasure_codec());
        if (hasNontrivialLimits || codecId == NErasure::ECodec::None) {
            auto slices = SliceChunkByRowIndexes(chunkSpec, sliceDataSize);
            appendStripes(slices);
        } else {
            for (const auto& slice : CreateErasureChunkSlices(chunkSpec, codecId)) {
                auto slices = slice->SliceEvenly(sliceDataSize);
                appendStripes(slices);
            }
        }

        LOG_TRACE("Slicing chunk (ChunkId: %v, SliceCount: %v)",
            FromProto<TChunkId>(chunkSpec->chunk_id()),
            result.size() - oldSize);
    }

    *jobCount = std::min(*jobCount, static_cast<int>(result.size()));
    if (!result.empty()) {
        *jobCount = std::max(*jobCount, 1 + (static_cast<int>(result.size()) - 1) / Config->MaxChunkStripesPerJob);
    }

    return result;
}

std::vector<TChunkStripePtr> TOperationControllerBase::SliceInputChunks(
    i64 maxSliceDataSize,
    int* jobCount)
{
    return SliceChunks(CollectInputChunks(), maxSliceDataSize, jobCount);
}

TKeyColumns TOperationControllerBase::CheckInputTablesSorted(const TKeyColumns& keyColumns)
{
    YCHECK(!InputTables.empty());

    for (const auto& table : InputTables) {
        if (table.KeyColumns.empty()) {
            THROW_ERROR_EXCEPTION("Input table %v is not sorted",
                table.Path.GetPath());
        }
    }

    if (!keyColumns.empty()) {
        for (const auto& table : InputTables) {
            if (!CheckKeyColumnsCompatible(table.KeyColumns, keyColumns)) {
                THROW_ERROR_EXCEPTION("Input table %v is sorted by columns [%v] that are not compatible with the requested columns [%v]",
                    table.Path.GetPath(),
                    JoinToString(table.KeyColumns),
                    JoinToString(keyColumns));
            }
        }
        return keyColumns;
    } else {
        const auto& referenceTable = InputTables[0];
        for (const auto& table : InputTables) {
            if (table.KeyColumns != referenceTable.KeyColumns) {
                THROW_ERROR_EXCEPTION("Key columns do not match: input table %v is sorted by columns [%v] while input table %v is sorted by columns [%v]",
                    table.Path.GetPath(),
                    JoinToString(table.KeyColumns),
                    referenceTable.Path.GetPath(),
                    JoinToString(referenceTable.KeyColumns));
            }
        }
        return referenceTable.KeyColumns;
    }
}

bool TOperationControllerBase::CheckKeyColumnsCompatible(
    const TKeyColumns& fullColumns,
    const TKeyColumns& prefixColumns)
{
    if (fullColumns.size() < prefixColumns.size()) {
        return false;
    }

    for (int index = 0; index < prefixColumns.size(); ++index) {
        if (fullColumns[index] != prefixColumns[index]) {
            return false;
        }
    }

    return true;
}

TKeyColumns TOperationControllerBase::GetCommonInputKeyPrefix()
{
    auto commonKey = InputTables[0].KeyColumns;
    for (const auto& table : InputTables) {
        if (table.KeyColumns.size() < commonKey.size()) {
            commonKey.erase(commonKey.begin() + table.KeyColumns.size(), commonKey.end());
        }

        int i = 0;
        for (; i < static_cast<int>(commonKey.size()); ++i) {
            if (commonKey[i] != table.KeyColumns[i]) {
                break;
            }
        }
        commonKey.erase(commonKey.begin() + i, commonKey.end());
    }
    return commonKey;
}

bool TOperationControllerBase::IsSortedOutputSupported() const
{
    return false;
}

bool TOperationControllerBase::IsParityReplicasFetchEnabled() const
{
    return false;
}

bool TOperationControllerBase::IsBoundaryKeysFetchEnabled() const
{
    return false;
}

void TOperationControllerBase::UpdateAllTasksIfNeeded(const TProgressCounter& jobCounter)
{
    if (jobCounter.GetAborted(EAbortReason::ResourceOverdraft) == Config->MaxMemoryReserveAbortJobCount) {
        UpdateAllTasks();
    }
}

bool TOperationControllerBase::IsMemoryReserveEnabled(const TProgressCounter& jobCounter) const
{
    return jobCounter.GetAborted(EAbortReason::ResourceOverdraft) < Config->MaxMemoryReserveAbortJobCount;
}

i64 TOperationControllerBase::GetMemoryReserve(bool memoryReserveEnabled, TUserJobSpecPtr userJobSpec) const
{
    if (memoryReserveEnabled) {
        return static_cast<i64>(userJobSpec->MemoryLimit * userJobSpec->MemoryReserveFactor);
    } else {
        return userJobSpec->MemoryLimit;
    }
}

void TOperationControllerBase::RegisterOutput(
    const TChunkTreeId& chunkTreeId,
    int key,
    int tableIndex,
    TOutputTable& table)
{
    if (chunkTreeId == NullChunkTreeId) {
        return;
    }

    table.OutputChunkTreeIds.insert(std::make_pair(key, chunkTreeId));

    if (IsOutputLivePreviewSupported()) {
        auto masterConnector = Host->GetMasterConnector();
        masterConnector->AttachToLivePreview(
            Operation,
            table.LivePreviewChunkListId,
            {chunkTreeId});
    }

    LOG_DEBUG("Output chunk tree registered (Table: %v, ChunkTreeId: %v, Key: %v)",
        tableIndex,
        chunkTreeId,
        key);
}

void TOperationControllerBase::RegisterBoundaryKeys(
    const TBoundaryKeysExt& boundaryKeys,
    int key,
    TOutputTable* outputTable)
{
    TJobBoundaryKeys jobBoundaryKeys;
    FromProto(&jobBoundaryKeys.MinKey, boundaryKeys.min());
    FromProto(&jobBoundaryKeys.MaxKey, boundaryKeys.max());
    jobBoundaryKeys.ChunkTreeKey = key;
    outputTable->BoundaryKeys.push_back(jobBoundaryKeys);
}

void TOperationControllerBase::RegisterOutput(
    TRefCountedChunkSpecPtr chunkSpec,
    int key,
    int tableIndex)
{
    auto& table = OutputTables[tableIndex];

    if (!table.KeyColumns.empty() && IsSortedOutputSupported()) {
        auto boundaryKeys = GetProtoExtension<TBoundaryKeysExt>(chunkSpec->chunk_meta().extensions());
        RegisterBoundaryKeys(boundaryKeys, key, &table);
    }

    RegisterOutput(FromProto<TChunkId>(chunkSpec->chunk_id()), key, tableIndex, table);
}

void TOperationControllerBase::RegisterOutput(
    TJobletPtr joblet,
    int key,
    const TCompletedJobSummary& jobSummary)
{
    const auto* userJobResult = FindUserJobResult(jobSummary.Result);

    for (int tableIndex = 0; tableIndex < OutputTables.size(); ++tableIndex) {
        auto& table = OutputTables[tableIndex];
        RegisterOutput(joblet->ChunkListIds[tableIndex], key, tableIndex, table);

        if (!table.KeyColumns.empty() && IsSortedOutputSupported()) {
            YCHECK(userJobResult);
            const auto& boundaryKeys = userJobResult->output_boundary_keys(tableIndex);
            RegisterBoundaryKeys(boundaryKeys, key, &table);
        }
    }
}

void TOperationControllerBase::RegisterInputStripe(TChunkStripePtr stripe, TTaskPtr task)
{
    yhash_set<TChunkId> visitedChunks;

    TStripeDescriptor stripeDescriptor;
    stripeDescriptor.Stripe = stripe;
    stripeDescriptor.Task = task;
    stripeDescriptor.Cookie = task->GetChunkPoolInput()->Add(stripe);

    for (const auto& slice : stripe->ChunkSlices) {
        auto chunkSpec = slice->GetChunkSpec();
        auto chunkId = FromProto<TChunkId>(chunkSpec->chunk_id());

        auto pair = InputChunkMap.insert(std::make_pair(chunkId, TInputChunkDescriptor()));
        auto& chunkDescriptor = pair.first->second;

        if (InputChunkSpecs.insert(chunkSpec).second) {
            chunkDescriptor.ChunkSpecs.push_back(chunkSpec);
        }

        if (IsUnavailable(*chunkSpec, IsParityReplicasFetchEnabled())) {
            chunkDescriptor.State = EInputChunkState::Waiting;
        }

        if (visitedChunks.insert(chunkId).second) {
            chunkDescriptor.InputStripes.push_back(stripeDescriptor);
        }
    }
}

void TOperationControllerBase::RegisterIntermediate(
    TJobletPtr joblet,
    TCompletedJobPtr completedJob,
    TChunkStripePtr stripe)
{
    for (const auto& chunkSlice : stripe->ChunkSlices) {
        auto chunkId = FromProto<TChunkId>(chunkSlice->GetChunkSpec()->chunk_id());
        YCHECK(ChunkOriginMap.insert(std::make_pair(chunkId, completedJob)).second);

        if (IsIntermediateLivePreviewSupported()) {
            auto masterConnector = Host->GetMasterConnector();
            masterConnector->AttachToLivePreview(
                Operation,
                IntermediateTable.LivePreviewChunkListId,
                {chunkId});
        }
    }
}

bool TOperationControllerBase::HasEnoughChunkLists(bool intermediate)
{
    if (intermediate) {
        return ChunkListPool->HasEnough(IntermediateOutputCellTag, 1);
    } else {
        for (const auto& pair : CellTagToOutputTableCount) {
            if (!ChunkListPool->HasEnough(pair.first, pair.second)) {
                return false;
            }
        }
        return true;
    }
}

TChunkListId TOperationControllerBase::ExtractChunkList(TCellTag cellTag)
{
    return ChunkListPool->Extract(cellTag);
}

void TOperationControllerBase::ReleaseChunkLists(const std::vector<TChunkListId>& ids)
{
    ChunkListPool->Release(ids);
}

void TOperationControllerBase::RegisterJoblet(TJobletPtr joblet)
{
    YCHECK(JobletMap.insert(std::make_pair(joblet->JobId, joblet)).second);
}

TOperationControllerBase::TJobletPtr TOperationControllerBase::GetJoblet(const TJobId& jobId)
{
    auto it = JobletMap.find(jobId);
    YCHECK(it != JobletMap.end());
    return it->second;
}

void TOperationControllerBase::RemoveJoblet(const TJobId& jobId)
{
    YCHECK(JobletMap.erase(jobId) == 1);
}

void TOperationControllerBase::BuildProgress(IYsonConsumer* consumer) const
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    BuildYsonMapFluently(consumer)
        .Item("jobs").Value(JobCounter)
        .Item("ready_job_count").Value(GetPendingJobCount())
        .Item("job_statistics").Value(JobStatistics)
        .Item("estimated_input_statistics").BeginMap()
            .Item("chunk_count").Value(TotalEstimatedInputChunkCount)
            .Item("uncompressed_data_size").Value(TotalEstimatedInputDataSize)
            .Item("compressed_data_size").Value(TotalEstimatedCompressedDataSize)
            .Item("row_count").Value(TotalEstimatedInputRowCount)
            .Item("unavailable_chunk_count").Value(UnavailableInputChunkCount)
        .EndMap()
        .Item("live_preview").BeginMap()
            .Item("output_supported").Value(IsOutputLivePreviewSupported())
            .Item("intermediate_supported").Value(IsIntermediateLivePreviewSupported())
        .EndMap();
}

void TOperationControllerBase::BuildBriefProgress(IYsonConsumer* consumer) const
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    BuildYsonMapFluently(consumer)
        .Item("jobs").Value(JobCounter);
}

void TOperationControllerBase::BuildResult(IYsonConsumer* consumer) const
{
    // TODO(acid): Think about correct affinity here.
    VERIFY_THREAD_AFFINITY(ControlThread);

    auto error = FromProto<TError>(Operation->Result().error());
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("error").Value(error)
        .EndMap();
}

void TOperationControllerBase::UpdateJobStatistics(const TJobSummary& jobSummary)
{
    auto statistics = jobSummary.Statistics;
    LOG_DEBUG("Job data statistics (JobId: %v, Input: {%v}, Output: {%v})",
        jobSummary.Id,
        GetTotalInputDataStatistics(statistics),
        GetTotalOutputDataStatistics(statistics));

    statistics.AddSuffixToNames(jobSummary.StatisticsSuffix);
    JobStatistics.Update(statistics);
}

void TOperationControllerBase::BuildBriefSpec(IYsonConsumer* consumer) const
{
    VERIFY_THREAD_AFFINITY_ANY();

    BuildYsonMapFluently(consumer)
        .DoIf(Spec->Title.HasValue(), [&] (TFluentMap fluent) {
            fluent
                .Item("title").Value(*Spec->Title);
        })
        .Item("input_table_paths").ListLimited(GetInputTablePaths(), 1)
        .Item("output_table_paths").ListLimited(GetOutputTablePaths(), 1);
}

std::vector<TOperationControllerBase::TPathWithStage> TOperationControllerBase::GetFilePaths() const
{
    return std::vector<TPathWithStage>();
}

bool TOperationControllerBase::IsRowCountPreserved() const
{
    return false;
}

int TOperationControllerBase::SuggestJobCount(
    i64 totalDataSize,
    i64 dataSizePerJob,
    TNullable<int> configJobCount,
    int maxJobCount) const
{
    i64 suggestionBySize = (totalDataSize + dataSizePerJob - 1) / dataSizePerJob;
    i64 jobCount = configJobCount.Get(suggestionBySize);
    return static_cast<int>(Clamp(jobCount, 1, maxJobCount));
}

void TOperationControllerBase::InitUserJobSpecTemplate(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TUserJobSpecPtr config,
    const std::vector<TUserFile>& files)
{
    jobSpec->set_shell_command(config->Command);
    jobSpec->set_memory_limit(config->MemoryLimit);
    jobSpec->set_iops_threshold(config->IopsThreshold);
    jobSpec->set_use_yamr_descriptors(config->UseYamrDescriptors);
    jobSpec->set_check_input_fully_consumed(config->CheckInputFullyConsumed);
    jobSpec->set_max_stderr_size(config->MaxStderrSize);
    jobSpec->set_enable_core_dump(config->EnableCoreDump);
    jobSpec->set_custom_statistics_count_limit(config->CustomStatisticsCountLimit);
    
    if (Config->UserJobBlkioWeight) {
        jobSpec->set_blkio_weight(*Config->UserJobBlkioWeight);
    }

    {
        // Set input and output format.
        TFormat inputFormat(EFormatType::Yson);
        TFormat outputFormat(EFormatType::Yson);

        if (config->Format) {
            inputFormat = outputFormat = *config->Format;
        }

        if (config->InputFormat) {
            inputFormat = *config->InputFormat;
        }

        if (config->OutputFormat) {
            outputFormat = *config->OutputFormat;
        }

        jobSpec->set_input_format(ConvertToYsonString(inputFormat).Data());
        jobSpec->set_output_format(ConvertToYsonString(outputFormat).Data());
    }

    auto fillEnvironment = [&] (yhash_map<Stroka, Stroka>& env) {
        for (const auto& pair : env) {
            jobSpec->add_environment(Format("%v=%v", pair.first, pair.second));
        }
    };

    // Global environment.
    fillEnvironment(Config->Environment);

    // Local environment.
    fillEnvironment(config->Environment);

    jobSpec->add_environment(Format("YT_OPERATION_ID=%v", OperationId));

    for (const auto& file : files) {
        auto *descriptor = jobSpec->add_files();
        descriptor->set_type(static_cast<int>(file.Type));
        descriptor->set_file_name(file.FileName);
        ToProto(descriptor->mutable_chunks(), file.ChunkSpecs);
        switch (file.Type) {
            case EObjectType::File:
                descriptor->set_executable(file.Executable);
                break;
            case EObjectType::Table:
                descriptor->set_format(file.Format.Data());
                break;
            default:
                YUNREACHABLE();
        }
    }
}

void TOperationControllerBase::InitUserJobSpec(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TJobletPtr joblet,
    i64 memoryReserve)
{
    ToProto(jobSpec->mutable_async_scheduler_transaction_id(), AsyncSchedulerTransactionId);

    jobSpec->set_memory_reserve(memoryReserve);

    jobSpec->add_environment(Format("YT_JOB_INDEX=%v", joblet->JobIndex));
    jobSpec->add_environment(Format("YT_JOB_ID=%v", joblet->JobId));
    if (joblet->StartRowIndex >= 0) {
        jobSpec->add_environment(Format("YT_START_ROW_INDEX=%v", joblet->StartRowIndex));
    }
}

i64 TOperationControllerBase::GetFinalOutputIOMemorySize(TJobIOConfigPtr ioConfig) const
{
    i64 result = 0;
    for (const auto& outputTable : OutputTables) {
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
    return result;
}

i64 TOperationControllerBase::GetFinalIOMemorySize(
    TJobIOConfigPtr ioConfig,
    const TChunkStripeStatisticsVector& stripeStatistics) const
{
    i64 result = 0;
    for (const auto& stat : stripeStatistics) {
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
    config->TableWriter->MinUploadReplicationFactor = 1;

    // Cache blocks on nodes.
    config->TableWriter->PopulateCache = true;

    // Don't sync intermediate chunks.
    config->TableWriter->SyncOnClose = false;
}

void TOperationControllerBase::ValidateKey(const TOwningKey& key)
{
    for (int i = 0; i < key.GetCount(); ++i) {
        ValidateKeyValue(key[i]);
    }
}

void TOperationControllerBase::InitFinalOutputConfig(TJobIOConfigPtr /* config */)
{ }

IClientPtr TOperationControllerBase::CreateClient()
{
    TClientOptions options;
    options.User = Operation->GetAuthenticatedUser();
    return Host
        ->GetMasterClient()
        ->GetConnection()
        ->CreateClient(options);
}

const NProto::TUserJobResult* TOperationControllerBase::FindUserJobResult(const TRefCountedJobResultPtr& result)
{
    const auto& schedulerJobResultExt = result->GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    if (schedulerJobResultExt.has_user_job_result()) {
        return &schedulerJobResultExt.user_job_result();
    }
    return nullptr;
}

void TOperationControllerBase::ValidateUserFileCount(TUserJobSpecPtr spec, const Stroka& operation)
{
    if (spec && spec->FilePaths.size() > Config->MaxUserFileCount) {
        THROW_ERROR_EXCEPTION("Too many user files in %v: maximum allowed %v, actual %v",
            operation,
            Config->MaxUserFileCount,
            spec->FilePaths.size());
    }
}

void TOperationControllerBase::Persist(TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, TotalEstimatedInputChunkCount);
    Persist(context, TotalEstimatedInputDataSize);
    Persist(context, TotalEstimatedInputRowCount);
    Persist(context, TotalEstimatedInputValueCount);
    Persist(context, TotalEstimatedCompressedDataSize);

    Persist(context, UnavailableInputChunkCount);

    Persist(context, JobCounter);

    Persist(context, InputNodeDirectory);
    Persist(context, AuxNodeDirectory);

    Persist(context, InputTables);

    Persist(context, OutputTables);

    Persist(context, IntermediateOutputCellTag);

    Persist(context, IntermediateTable);

    Persist(context, Files);

    Persist(context, Tasks);

    Persist(context, TaskGroups);

    Persist(context, InputChunkMap);

    Persist(context, CellTagToOutputTableCount);

    Persist(context, CachedPendingJobCount);

    Persist(context, CachedNeededResources);

    Persist(context, ChunkOriginMap);

    Persist(context, JobletMap);

    Persist(context, JobIndexGenerator);

    Persist(context, JobStatistics);

    // NB: Scheduler snapshots need not be stable.
    Persist<
        TSetSerializer<
            TDefaultSerializer,
            TUnsortedTag
        >
    >(context, InputChunkSpecs);

    if (context.IsLoad()) {
        for (const auto& task : Tasks) {
            task->Initialize();
        }
    }
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

