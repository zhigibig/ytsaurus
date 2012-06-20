#include "stdafx.h"
#include "merge_controller.h"
#include "private.h"
#include "operation_controller.h"
#include "operation_controller_detail.h"
#include "chunk_pool.h"
#include "chunk_list_pool.h"

#include <ytlib/ytree/fluent.h>
#include <ytlib/transaction_client/transaction.h>
#include <ytlib/table_client/key.h>
#include <ytlib/table_client/chunk_meta_extensions.h>
#include <ytlib/chunk_holder/chunk_meta_extensions.h>

#include <cmath>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NObjectServer;
using namespace NChunkServer;
using namespace NTableClient;
using namespace NTableServer;
using namespace NScheduler::NProto;
using namespace NChunkHolder::NProto;
using namespace NTableClient::NProto;

////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger(OperationLogger);
static NProfiling::TProfiler Profiler("/operations/merge");

////////////////////////////////////////////////////////////////////

class TMergeControllerBase
    : public TOperationControllerBase
{
public:
    TMergeControllerBase(
        TSchedulerConfigPtr config,
        TMergeOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TOperationControllerBase(config, host, operation)
        , Spec(spec)
        , TotalJobCount(0)
        , CurrentTaskWeight(0)
    { }

protected:
    TMergeOperationSpecPtr Spec;

    // Counters.
    int TotalJobCount;
    TProgressCounter WeightCounter;
    TProgressCounter ChunkCounter;

    //! For each input table, the corresponding entry holds the stripe
    //! containing the chunks collected so far. Empty stripes are never stored explicitly
    //! and are denoted by |NULL|.
    std::vector<TChunkStripePtr> CurrentTaskStripes;

    //! The total weight accumulated in #CurrentTaskStripes.
    i64 CurrentTaskWeight;

    //! Key columns to pass in the job spec. Only makes sense for sorted merge.
    std::vector<Stroka> KeyColumns;

    //! The template for starting new jobs.
    TJobSpec JobSpecTemplate;

    // Merge task.

    class TMergeTask
        : public TTask
    {
    public:
        explicit TMergeTask(
            TMergeControllerBase* controller,
            int taskIndex,
            int partitionIndex = -1)
            : TTask(controller)
            , Controller(controller)
            , TaskIndex(taskIndex)
            , PartitionIndex(partitionIndex)
        {
            ChunkPool = CreateAtomicChunkPool();
        }

        Stroka GetId() const OVERRIDE
        {
            return
                PartitionIndex < 0
                ? Sprintf("Merge(%d)", TaskIndex)
                : Sprintf("Merge(%d,%d)", TaskIndex, PartitionIndex);
        }

        int GetPendingJobCount() const OVERRIDE
        {
            return ChunkPool->IsPending() ? 1 : 0;
        }

        TDuration GetMaxLocalityDelay() const OVERRIDE
        {
            // TODO(babenko): make configurable
            return TDuration::Seconds(5);
        }


    private:
        TMergeControllerBase* Controller;

        //! The position in |MergeTasks|. 
        int TaskIndex;

        //! The position in |TOutputTable::PartitionIds| where the 
        //! output of this task must be placed.
        int PartitionIndex;


        int GetChunkListCountPerJob() const OVERRIDE
        {
            return 1;
        }

        TNullable<i64> GetJobWeightThreshold() const OVERRIDE
        {
            return Null;
        }

        TJobSpec GetJobSpec(TJobInProgress* jip) OVERRIDE
        {
            auto jobSpec = Controller->JobSpecTemplate;
            AddParallelInputSpec(&jobSpec, jip);
            AddTabularOutputSpec(&jobSpec, jip, Controller->OutputTables[0]);
            return jobSpec;
        }

        void OnJobStarted(TJobInProgress* jip) OVERRIDE
        {
            TTask::OnJobStarted(jip);

            Controller->ChunkCounter.Start(jip->PoolResult->TotalChunkCount);
            Controller->WeightCounter.Start(jip->PoolResult->TotalChunkWeight);
        }

        void OnJobCompleted(TJobInProgress* jip) OVERRIDE
        {
            TTask::OnJobCompleted(jip);

            Controller->ChunkCounter.Completed(jip->PoolResult->TotalChunkCount);
            Controller->WeightCounter.Completed(jip->PoolResult->TotalChunkWeight);

            auto& table = Controller->OutputTables[0];
            table.PartitionTreeIds[PartitionIndex] = jip->ChunkListIds[0];
        }

        void OnJobFailed(TJobInProgress* jip)
        {
            TTask::OnJobFailed(jip);

            Controller->ChunkCounter.Failed(jip->PoolResult->TotalChunkCount);
            Controller->WeightCounter.Failed(jip->PoolResult->TotalChunkWeight);
        }

    };

    typedef TIntrusivePtr<TMergeTask> TMergeTaskPtr;

    std::vector<TMergeTaskPtr> MergeTasks;

    //! Resizes #CurrentTaskStripes appropriately and sets all its entries to |NULL|.
    void ClearCurrentTaskStripes()
    {
        CurrentTaskStripes.clear();
        CurrentTaskStripes.resize(InputTables.size());
    }
    
    //! Finishes the current task.
    void EndTask()
    {
        YCHECK(HasActiveTask());

        auto& table = OutputTables[0];
        auto task = New<TMergeTask>(
            this,
            static_cast<int>(MergeTasks.size()),
            static_cast<int>(table.PartitionTreeIds.size()));

        FOREACH (auto stripe, CurrentTaskStripes) {
            if (stripe) {
                task->AddStripe(stripe);
            }
        }

        // Reserve a place for this task among partitions.
        table.PartitionTreeIds.push_back(NullChunkListId);

        MergeTasks.push_back(task);

        LOG_DEBUG("Finished task (Task: %d, Weight: %" PRId64 ")",
            static_cast<int>(MergeTasks.size()) - 1,
            CurrentTaskWeight);

        CurrentTaskWeight = 0;
        ClearCurrentTaskStripes();
    }

    //! Finishes the current task if the size is large enough.
    void EndTaskIfLarge()
    {
        if (HasLargeActiveTask()) {
            EndTask();
        }
    }

    //! Returns True if some stripes are currently queued.
    bool HasActiveTask()
    {
        return CurrentTaskWeight > 0;
    }

    //! Returns True if the total weight of currently queued stripes exceeds the pre-configured limit.
    bool HasLargeActiveTask()
    {
        return CurrentTaskWeight >= Spec->MaxMergeJobWeight;
    }

    //! Add chunk to the current task's pool.
    void AddPendingChunk(const TInputChunk& chunk, int tableIndex)
    {
        // Merge is IO-bound, use data size as weight.
        auto misc = GetProtoExtension<TMiscExt>(chunk.extensions());
        i64 weight = misc->data_weight();

        auto stripe = CurrentTaskStripes[tableIndex];
        if (!stripe) {
            stripe = CurrentTaskStripes[tableIndex] = New<TChunkStripe>();
        }

        WeightCounter.Increment(weight);
        ChunkCounter.Increment(1);
        CurrentTaskWeight += weight;
        stripe->AddChunk(chunk, weight);

        auto& table = OutputTables[0];
        auto chunkId = TChunkId::FromProto(chunk.slice().chunk_id());
        LOG_DEBUG("Added pending chunk (ChunkId: %s, Partition: %d, Task: %d, TableIndex: %d)",
            ~chunkId.ToString(),
            static_cast<int>(table.PartitionTreeIds.size()),
            static_cast<int>(MergeTasks.size()),
            tableIndex);
    }

    //! Add chunk directly to the output.
    void AddPassthroughChunk(const TInputChunk& chunk)
    {
        auto& table = OutputTables[0];
        auto chunkId = TChunkId::FromProto(chunk.slice().chunk_id());
        LOG_DEBUG("Added passthrough chunk (ChunkId: %s, Partition: %d)",
            ~chunkId.ToString(),
            static_cast<int>(table.PartitionTreeIds.size()));
        // Place the chunk directly to the output table.
        table.PartitionTreeIds.push_back(chunkId);
    }


    // Init/finish.

    void CustomInitialize() OVERRIDE
    {
        if (InputTables.empty()) {
            // At least one table is needed for sorted merge to figure out the key columns.
            // To be consistent, we don't allow empty set of input tables in for any merge type.
            ythrow yexception() << "At least more input table must be given";
        }
    }

    // Custom bits of preparation pipeline.

    std::vector<TYPath> GetInputTablePaths() OVERRIDE
    {
        return Spec->InputTablePaths;
    }

    std::vector<TYPath> GetOutputTablePaths() OVERRIDE
    {
        std::vector<TYPath> result;
        result.push_back(Spec->OutputTablePath);
        return result;
    }

    TAsyncPipeline<void>::TPtr CustomizePreparationPipeline(TAsyncPipeline<void>::TPtr pipeline) OVERRIDE
    {
        return pipeline->Add(BIND(&TMergeControllerBase::ProcessInputs, MakeStrong(this)));
    }

    void ProcessInputs()
    {
        PROFILE_TIMING ("/input_processing_time") {
            LOG_INFO("Processing inputs");

            ClearCurrentTaskStripes();
            BeginInputChunks();

            for (int tableIndex = 0; tableIndex < static_cast<int>(InputTables.size()); ++tableIndex) {
                const auto& table = InputTables[tableIndex];
                FOREACH (auto& chunk, *table.FetchResponse->mutable_chunks()) {
                    auto chunkId = TChunkId::FromProto(chunk.slice().chunk_id());
                    auto miscExt = GetProtoExtension<TMiscExt>(chunk.extensions());
                    i64 weight = miscExt->data_weight();
                    i64 rowCount = miscExt->row_count();
                    LOG_DEBUG("Processing chunk (ChunkId: %s, DataWeight: %" PRId64 ", RowCount: %" PRId64 ")",
                        ~chunkId.ToString(),
                        weight,
                        rowCount);
                    ProcessInputChunk(chunk, tableIndex);
                }
            }

            EndInputChunks();

            // Check for trivial inputs.
            if (ChunkCounter.GetTotal() == 0) {
                LOG_INFO("Trivial merge");
                OnOperationCompleted();
                return;
            }

            // Init counters.
            TotalJobCount = static_cast<int>(MergeTasks.size());

            // Allocate some initial chunk lists.
            ChunkListPool->Allocate(TotalJobCount + Config->SpareChunkListCount);

            InitJobSpecTemplate();

            LOG_INFO("Inputs processed (Weight: %" PRId64 ", ChunkCount: %" PRId64 ", JobCount: %d)",
                WeightCounter.GetTotal(),
                ChunkCounter.GetTotal(),
                TotalJobCount);

            // Kick-start the tasks.
            FOREACH (const auto task, MergeTasks) {
                AddTaskPendingHint(task);
            }
        }
    }


    //! Called at the beginning of input chunks scan.
    virtual void BeginInputChunks()
    { }

    //! Called for each input chunk.
    virtual void ProcessInputChunk(const TInputChunk& chunk, int tableIndex) = 0;

    //! Called at the end of input chunks scan.
    virtual void EndInputChunks()
    {
        // Close the last task, if any.
        if (CurrentTaskWeight > 0) {
            EndTask();
        }
    }


    // Progress reporting.

    void LogProgress() OVERRIDE
    {
        LOG_DEBUG("Progress: "
            "Jobs = {T: %d, R: %d, C: %d, P: %d, F: %d}, "
            "Chunks = {%s}, "
            "Weight = {%s}",
            TotalJobCount,
            RunningJobCount,
            CompletedJobCount,
            GetPendingJobCount(),
            FailedJobCount,
            ~ToString(ChunkCounter),
            ~ToString(WeightCounter));
    }

    void DoGetProgress(IYsonConsumer* consumer) OVERRIDE
    {
        BuildYsonMapFluently(consumer)
            .Item("chunks").Do(BIND(&TProgressCounter::ToYson, &ChunkCounter))
            .Item("weight").Do(BIND(&TProgressCounter::ToYson, &WeightCounter));
    }


    // Unsorted helpers.

    //! Returns True iff the chunk has nontrivial limits.
    //! Such chunks are always pooled.
    static bool IsCompleteChunk(const TInputChunk& chunk)
    {
        return
            !chunk.slice().start_limit().has_row_index() &&
            !chunk.slice().start_limit().has_key () &&
            !chunk.slice().end_limit().has_row_index() &&
            !chunk.slice().end_limit().has_key ();
    }

    //! Returns True iff the chunk is complete and is large enough to be included to the output as-is.
    //! When |CombineChunks| is off, all complete chunks are considered large.
    bool IsLargeCompleteChunk(const TInputChunk& chunk)
    {
        if (!IsCompleteChunk(chunk)) {
            return false;
        }

        auto miscExt = GetProtoExtension<TMiscExt>(chunk.extensions());
        // ChunkSequenceWriter may actually produce a chunk a bit smaller than DesiredChunkSize,
        // so we have to be more flexible here.
        if (0.9 * miscExt->compressed_data_size() >= Config->MergeJobIO->ChunkSequenceWriter->DesiredChunkSize) {
            return true;
        }

        if (!Spec->CombineChunks) {
            return true;
        }

        return false;
    }

    virtual void InitJobSpecTemplate()
    {
        JobSpecTemplate.set_type(
            Spec->Mode == EMergeMode::Sorted
            ? EJobType::SortedMerge
            : EJobType::OrderedMerge);

        *JobSpecTemplate.mutable_output_transaction_id() = OutputTransaction->GetId().ToProto();

        auto* jobSpecExt = JobSpecTemplate.MutableExtension(NScheduler::NProto::TMergeJobSpecExt::merge_job_spec_ext);
        ToProto(jobSpecExt->mutable_key_columns(), KeyColumns);

        // ToDo(psushin): set larger PrefetchWindow for unordered merge.
        JobSpecTemplate.set_io_config(SerializeToYson(Config->MergeJobIO));
    }

};

////////////////////////////////////////////////////////////////////

//! Handles unordered merge operation.
class TUnorderedMergeController
    : public TMergeControllerBase
{
public:
    TUnorderedMergeController(
        TSchedulerConfigPtr config,
        TMergeOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TMergeControllerBase(config, spec, host, operation)
    { }

private:
    void ProcessInputChunk(const TInputChunk& chunk, int tableIndex) OVERRIDE
    {
        UNUSED(tableIndex);

        auto chunkId = TChunkId::FromProto(chunk.slice().chunk_id());
        auto& table = OutputTables[0];

        if (IsLargeCompleteChunk(chunk)) {
            // Chunks not requiring merge go directly to the output chunk list.
            AddPassthroughChunk(chunk);
            return;
        }

        // All chunks go to a single chunk stripe.
        AddPendingChunk(chunk, 0);
        EndTaskIfLarge();
    }

};

////////////////////////////////////////////////////////////////////

//! Handles ordered merge and (sic!) erase operations.
class TOrderedMergeController
    : public TMergeControllerBase
{
public:
    TOrderedMergeController(
        TSchedulerConfigPtr config,
        TMergeOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TMergeControllerBase(config, spec, host, operation)
    { }

private:
    void ProcessInputChunk(const TInputChunk& chunk, int tableIndex) OVERRIDE
    {
        UNUSED(tableIndex);

        auto chunkId = TChunkId::FromProto(chunk.slice().chunk_id());
        auto& table = OutputTables[0];

        if (IsLargeCompleteChunk(chunk) && !HasActiveTask()) {
            // Merge is not required and no current task is active.
            // Copy the chunk directly to the output.
            LOG_DEBUG("Chunk %s is large and complete, using as-is in partition %d",
                ~chunkId.ToString(),
                static_cast<int>(table.PartitionTreeIds.size()));
            table.PartitionTreeIds.push_back(chunkId);
            return;
        }

        // All chunks go to a single chunk stripe.
        AddPendingChunk(chunk, 0);
        EndTaskIfLarge();
    }
};

////////////////////////////////////////////////////////////////////

class TEraseController
    : public TOrderedMergeController
{
public:
    TEraseController(
        TSchedulerConfigPtr config,
        TEraseOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TOrderedMergeController(
            config,
            BuildFakeMergeSpec(spec),
            host,
            operation)
    { }


private:
    void CustomInitialize() OVERRIDE
    {
        // For erase operation the rowset specified by the user must actually be removed...
        InputTables[0].NegateFetch = true;
        // ...and the output table must be cleared.
        OutputTables[0].Clear = true;
    }

    void OnCustomInputsRecieved(TObjectServiceProxy::TRspExecuteBatchPtr batchRsp) OVERRIDE
    {
        UNUSED(batchRsp);

        // If the input is sorted then the output is marked as sorted as well
        if (InputTables[0].Sorted) {
            SetOutputTablesSorted(InputTables[0].KeyColumns);
        }
    }

    static TMergeOperationSpecPtr BuildFakeMergeSpec(TEraseOperationSpecPtr eraseSpec)
    {
        auto mergeSpec = New<TMergeOperationSpec>();
        mergeSpec->InputTablePaths.push_back(eraseSpec->TablePath);
        mergeSpec->OutputTablePath = eraseSpec->TablePath;
        mergeSpec->Mode = EMergeMode::Ordered;
        mergeSpec->CombineChunks = eraseSpec->CombineChunks;
        return mergeSpec;
    }
};

////////////////////////////////////////////////////////////////////

//! Handles sorted merge operation.
class TSortedMergeController
    : public TMergeControllerBase
{
public:
    TSortedMergeController(
        TSchedulerConfigPtr config,
        TMergeOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TMergeControllerBase(config, spec, host, operation)
    { }

private:
    //! Either the left or the right endpoint of a chunk.
    struct TKeyEndpoint
    {
        bool Left;
        int TableIndex;
        NTableClient::NProto::TKey Key;
        const TInputChunk* InputChunk;
    };

    std::vector<TKeyEndpoint> Endpoints;

    void ProcessInputChunk(const TInputChunk& chunk, int tableIndex) OVERRIDE
    {
        auto miscExt = GetProtoExtension<TMiscExt>(chunk.extensions());
        YCHECK(miscExt->sorted());

        // Construct endpoints and place them into the list.
        auto boundaryKeysExt = GetProtoExtension<NTableClient::NProto::TBoundaryKeysExt>(chunk.extensions());
        {
            TKeyEndpoint endpoint;
            endpoint.Left = true;
            endpoint.TableIndex = tableIndex;
            endpoint.Key = boundaryKeysExt->start();
            endpoint.InputChunk = &chunk;
            Endpoints.push_back(endpoint);
        }
        {
            TKeyEndpoint endpoint;
            endpoint.Left = false;
            endpoint.TableIndex = tableIndex;
            endpoint.Key = boundaryKeysExt->end();
            endpoint.InputChunk = &chunk;
            Endpoints.push_back(endpoint);
        }
    }

    void EndInputChunks() OVERRIDE
    {
        // Sort earlier collected endpoints to figure out overlapping chunks.
        // Sort endpoints by keys, in case of a tie left endpoints go first.
        LOG_INFO("Sorting chunks");

        int prefixLength = static_cast<int>(KeyColumns.size());
        std::sort(
            Endpoints.begin(),
            Endpoints.end(),
            [=] (const TKeyEndpoint& lhs, const TKeyEndpoint& rhs) -> bool {
                auto keysResult = CompareKeys(lhs.Key, rhs.Key, prefixLength);
                if (keysResult != 0) {
                    return keysResult < 0;
                }
                return lhs.Left && !rhs.Left;
            });

        // Compute components consisting of overlapping chunks.
        // Combine small tasks, if requested so.
        LOG_INFO("Building tasks");
        int depth = 0;
        int startIndex = 0;
        for (
            int currentIndex = startIndex; 
            currentIndex < static_cast<int>(Endpoints.size()); 
            ++currentIndex) 
        {
            auto& endpoint = Endpoints[currentIndex];
            if (endpoint.Left) {
                ++depth;
            } else {
                --depth;
                if (depth == 0) {
                    ProcessOverlap(startIndex, currentIndex + 1);
                    startIndex = currentIndex + 1;
                }
            }
        }

        SetOutputTablesSorted(KeyColumns);

        TMergeControllerBase::EndInputChunks();
    }

    void ProcessOverlap(int startIndex, int endIndex)
    {
        // Must be an even number of endpoints.
        YCHECK((endIndex - startIndex) % 2 == 0);

        int chunkCount = (endIndex - startIndex) / 2;
        LOG_DEBUG("Found overlap of %d chunks", chunkCount);

        // Check for trivial components.
        {
            const auto& chunk = *Endpoints[startIndex].InputChunk;
            if (chunkCount == 1 && IsLargeCompleteChunk(chunk) && !HasActiveTask()) {
                auto chunkId = TChunkId::FromProto(chunk.slice().chunk_id());
                // Merge is not required and no current task is active.
                // Copy the chunk directly to the output.
                AddPassthroughChunk(chunk);
                return;
            }
        }

        TNullable<NTableClient::NProto::TKey> lastBreakpoint;
        yhash_map<const TInputChunk*, TKeyEndpoint*> openedChunks;

        for (int index = startIndex; index < endIndex; ++index) {
            auto& endpoint = Endpoints[index];
            auto chunkId = TChunkId::FromProto(endpoint.InputChunk->slice().chunk_id());
            if (endpoint.Left) {
                YCHECK(openedChunks.insert(std::make_pair(endpoint.InputChunk, &endpoint)).second);
                LOG_DEBUG("Chunk interval opened (ChunkId: %s)", ~chunkId.ToString());
            } else {
                AddPendingChunk(
                    SliceChunk(*endpoint.InputChunk, lastBreakpoint, Null),
                    endpoint.TableIndex);
                YCHECK(openedChunks.erase(endpoint.InputChunk) == 1);
                LOG_DEBUG("Chunk interval closed (ChunkId: %s)", ~chunkId.ToString());
                if (HasLargeActiveTask()) {
                    auto nextBreakpoint = GetKeySuccessor(endpoint.Key);
                    LOG_DEBUG("Task is too large, flushing %" PRISZT " chunks at key {%s}",
                        openedChunks.size(),
                        ~nextBreakpoint.DebugString());
                    FOREACH (const auto& pair, openedChunks) {
                        AddPendingChunk(
                            SliceChunk(*pair.second->InputChunk, lastBreakpoint, nextBreakpoint),
                            pair.second->TableIndex);
                    }
                    EndTask();
                    LOG_DEBUG("Finished flushing opened chunks");
                    lastBreakpoint = nextBreakpoint;
                }
            }
        }

        YCHECK(openedChunks.empty());
        EndTaskIfLarge();
    }


    void OnCustomInputsRecieved(NObjectServer::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp) OVERRIDE
    {
        UNUSED(batchRsp);

        KeyColumns = CheckInputTablesSorted(Spec->KeyColumns);
        LOG_INFO("Key columns are %s", ~SerializeToYson(KeyColumns, EYsonFormat::Text));

        CheckOutputTablesEmpty();
    }
};

////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateMergeController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = New<TMergeOperationSpec>();
    try {
        spec->Load(operation->GetSpec());
    } catch (const std::exception& ex) {
        ythrow yexception() << Sprintf("Error parsing operation spec\n%s", ex.what());
    }

    switch (spec->Mode) {
        case EMergeMode::Unordered:
            return New<TUnorderedMergeController>(config, spec, host, operation);
        case EMergeMode::Ordered:
            return New<TOrderedMergeController>(config, spec, host, operation);
        case EMergeMode::Sorted:
            return New<TSortedMergeController>(config, spec, host, operation);
        default:
            YUNREACHABLE();
    }
}

IOperationControllerPtr CreateEraseController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = New<TEraseOperationSpec>();
    try {
        spec->Load(operation->GetSpec());
    } catch (const std::exception& ex) {
        ythrow yexception() << Sprintf("Error parsing operation spec\n%s", ex.what());
    }

    return New<TEraseController>(config, spec, host, operation);
}

IOperationControllerPtr CreateReduceController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto reduceSpec = New<TReduceOperationSpec>();
    try {
        reduceSpec->Load(operation->GetSpec());
    } catch (const std::exception& ex) {
        ythrow yexception() << Sprintf("Error parsing operation spec\n%s", ex.what());
    }

    // TODO(babenko): fixme
    return NULL;
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

