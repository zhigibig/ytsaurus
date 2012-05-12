#include "stdafx.h"
#include "map_controller.h"
#include "private.h"
#include "operation_controller_detail.h"
#include "chunk_pool.h"
#include "chunk_list_pool.h"

#include <ytlib/ytree/fluent.h>
#include <ytlib/table_client/schema.h>
#include <ytlib/job_proxy/config.h>
#include <ytlib/chunk_holder/chunk_meta_extensions.h>

#include <cmath>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NChunkServer;
using namespace NScheduler::NProto;
using namespace NChunkHolder::NProto;
using namespace NTableClient::NProto;

////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger(OperationLogger);
static NProfiling::TProfiler Profiler("/operations/map");

////////////////////////////////////////////////////////////////////

class TMapController
    : public TOperationControllerBase
{
public:
    TMapController(
        TSchedulerConfigPtr config,
        TMapOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TOperationControllerBase(config, host, operation)
        , Config(config)
        , Spec(spec)
        , TotalJobCount(0)
        , TotalWeight(0)
        , PendingWeight(0)
        , CompletedWeight(0)
        , TotalChunkCount(0)
        , PendingChunkCount(0)
        , CompletedChunkCount(0)
    { }

private:
    typedef TMapController TThis;

    TSchedulerConfigPtr Config;
    TMapOperationSpecPtr Spec;

    // Counters.
    int TotalJobCount;
    i64 TotalWeight;
    i64 PendingWeight;
    i64 CompletedWeight;
    int TotalChunkCount;
    int PendingChunkCount;
    int CompletedChunkCount;
    
    TAutoPtr<IChunkPool> ChunkPool;

    // A template for starting new jobs.
    TJobSpec JobSpecTemplate;

    // Init/finish.

    virtual int GetPendingJobCount()
    {
        return PendingWeight == 0
            ? 0
            : TotalJobCount - CompletedJobCount;
    }


    // Job scheduling and outcome handling.

    struct TMapJobInProgress
        : public TJobInProgress
    {
        IChunkPool::TExtractResultPtr ExtractResult;
        std::vector<TChunkListId> ChunkListIds;
    };

    virtual TJobPtr DoScheduleJob(TExecNodePtr node)
    {
        // Check if we have enough chunk lists in the pool.
        if (!CheckChunkListsPoolSize(OutputTables.size())) {
            return NULL;
        }

        // We've got a job to do! :)

        // Allocate chunks for the job.
        auto jip = New<TMapJobInProgress>();
        i64 weightThreshold = GetJobWeightThreshold(GetPendingJobCount(), PendingWeight);
        jip->ExtractResult = ChunkPool->Extract(
            node->GetAddress(),
            weightThreshold,
            std::numeric_limits<int>::max(),
            false);
        YASSERT(jip->ExtractResult);

        LOG_DEBUG("Extracted %d chunks, %d local for node %s (ExtractedWeight: %" PRId64 ", WeightThreshold: %" PRId64 ")",
            static_cast<int>(jip->ExtractResult->Chunks.size()),
            jip->ExtractResult->LocalCount,
            ~node->GetAddress(),
            jip->ExtractResult->Weight,
            weightThreshold);

        // Make a copy of the generic spec and customize it.
        auto jobSpec = JobSpecTemplate;
        auto* mapJobSpec = jobSpec.MutableExtension(TMapJobSpec::map_job_spec);
        FOREACH (const auto& chunk, jip->ExtractResult->Chunks) {
            *mapJobSpec->mutable_input_spec()->add_chunks() = chunk->InputChunk;
        }
        FOREACH (auto& outputSpec, *mapJobSpec->mutable_output_specs()) {
            auto chunkListId = ChunkListPool->Extract();
            jip->ChunkListIds.push_back(chunkListId);
            *outputSpec.mutable_chunk_list_id() = chunkListId.ToProto();
        }

        // Update running counters.
        PendingChunkCount -= jip->ExtractResult->Chunks.size();
        PendingWeight -= jip->ExtractResult->Weight;

        return CreateJob(
            jip,
            node,
            jobSpec,
            BIND(&TThis::OnJobCompleted, MakeWeak(this)),
            BIND(&TThis::OnJobFailed, MakeWeak(this)));
    }

    void OnJobCompleted(TMapJobInProgress* jip)
    {
        CompletedChunkCount += jip->ExtractResult->Chunks.size();
        CompletedWeight += jip->ExtractResult->Weight;

        for (int index = 0; index < static_cast<int>(OutputTables.size()); ++index) {
            auto chunkListId = jip->ChunkListIds[index];
            OutputTables[index].PartitionTreeIds.push_back(chunkListId);
        }
    }

    void OnJobFailed(TMapJobInProgress* jip)
    {
        PendingChunkCount += jip->ExtractResult->Chunks.size();
        PendingWeight += jip->ExtractResult->Weight;

        LOG_DEBUG("Returned %d chunks into pool",
            static_cast<int>(jip->ExtractResult->Chunks.size()));
        ChunkPool->PutBack(jip->ExtractResult);

        ReleaseChunkLists(jip->ChunkListIds);
    }


    // Custom bits of preparation pipeline.

    virtual std::vector<TYPath> GetInputTablePaths()
    {
        return Spec->InputTablePaths;
    }

    virtual std::vector<TYPath> GetOutputTablePaths()
    {
        return Spec->OutputTablePaths;
    }

    virtual std::vector<TYPath> GetFilePaths()
    {
        return Spec->FilePaths;
    }

    virtual TAsyncPipeline<void>::TPtr CustomizePreparationPipeline(TAsyncPipeline<void>::TPtr pipeline)
    {
        return pipeline->Add(BIND(&TThis::ProcessInputs, MakeStrong(this)));
    }

    void ProcessInputs()
    {
        PROFILE_TIMING ("/input_processing_time") {
            LOG_INFO("Processing inputs");
            
            // Compute statistics and populate the pool.
            i64 totalRowCount = 0;
            i64 totalDataSize = 0;

            ChunkPool = CreateUnorderedChunkPool();

            for (int tableIndex = 0; tableIndex < static_cast<int>(InputTables.size()); ++tableIndex) {
                const auto& table = InputTables[tableIndex];

                TNullable<TYson> rowAttributes;
                if (InputTables.size() > 1) {
                    rowAttributes = BuildYsonFluently()
                        .BeginMap()
                            .Item("table_index").Scalar(tableIndex)
                        .EndMap();
                }

                FOREACH (auto& chunk, *table.FetchResponse->mutable_chunks()) {
                    // Currently fetch never returns row attributes.
                    YASSERT(!chunk.has_row_attributes());

                    if (rowAttributes) {
                        chunk.set_row_attributes(rowAttributes.Get());
                    }

                    auto miscExt = GetProtoExtension<NChunkHolder::NProto::TMiscExt>(chunk.extensions());

                    i64 rowCount = miscExt->row_count();
                    i64 dataSize = miscExt->uncompressed_size();

                    // TODO(babenko): make customizable
                    // Plus one is to ensure that weights are positive.
                    i64 weight = dataSize + 1;

                    totalRowCount += rowCount;
                    totalDataSize += dataSize;
                    ++TotalChunkCount;
                    TotalWeight += weight;

                    auto pooledChunk = New<TPooledChunk>(chunk, weight);
                    ChunkPool->Add(pooledChunk);
                }
            }

            // Check for empty inputs.
            if (totalRowCount == 0) {
                LOG_INFO("Empty input");
                FinalizeOperation();
                return;
            }

            // Init counters.
            ChooseJobCount();
            PendingWeight = TotalWeight;
            PendingChunkCount = TotalChunkCount;

            // Allocate some initial chunk lists.
            ChunkListPool->Allocate(OutputTables.size() * TotalJobCount + Config->SpareChunkListCount);

            InitJobSpecTemplate();

            LOG_INFO("Inputs processed (RowCount: %" PRId64 ", DataSize: %" PRId64 ", Weight: %" PRId64 ", ChunkCount: %d, JobCount: %d)",
                totalRowCount,
                totalDataSize,
                TotalWeight,
                TotalChunkCount,
                TotalJobCount);
        }
    }

    void ChooseJobCount()
    {
        TotalJobCount = GetJobCount(
            TotalWeight,
            Spec->JobIO->ChunkSequenceWriter->DesiredChunkSize,
            Spec->JobCount,
            TotalChunkCount);
    }

    // Progress reporting.

    virtual void LogProgress()
    {
        LOG_DEBUG("Progress: "
            "Jobs = {T: %d, R: %d, C: %d, P: %d, F: %d}, "
            "Chunks = {T: %d, C: %d, P: %d}, "
            "Weight = {T: %" PRId64 ", C: %" PRId64 ", P: %" PRId64 "}",
            TotalJobCount,
            RunningJobCount,
            CompletedJobCount,
            GetPendingJobCount(),
            FailedJobCount,
            TotalChunkCount,
            CompletedChunkCount,
            PendingChunkCount,
            TotalWeight,
            CompletedWeight,
            PendingWeight);
    }

    virtual void DoGetProgress(IYsonConsumer* consumer)
    {
        BuildYsonMapFluently(consumer)
            .Item("chunks").BeginMap()
                .Item("total").Scalar(TotalChunkCount)
                .Item("completed").Scalar(CompletedChunkCount)
                .Item("pending").Scalar(PendingChunkCount)
            .EndMap()
            .Item("weight").BeginMap()
                .Item("total").Scalar(TotalWeight)
                .Item("completed").Scalar(CompletedWeight)
                .Item("pending").Scalar(PendingWeight)
            .EndMap();
    }

    // Unsorted helpers.

    void InitJobSpecTemplate()
    {
        JobSpecTemplate.set_type(EJobType::Map);

        TUserJobSpec userJobSpec;
        userJobSpec.set_shell_command(Spec->Mapper);
        FOREACH (const auto& file, Files) {
            *userJobSpec.add_files() = *file.FetchResponse;
        }
        *JobSpecTemplate.MutableExtension(TUserJobSpec::user_job_spec) = userJobSpec;

        TMapJobSpec mapJobSpec;
        *mapJobSpec.mutable_output_transaction_id() = OutputTransaction->GetId().ToProto();
        FOREACH (const auto& table, OutputTables) {
            auto* outputSpec = mapJobSpec.add_output_specs();
            outputSpec->set_channels(table.Channels);
        }
        *JobSpecTemplate.MutableExtension(TMapJobSpec::map_job_spec) = mapJobSpec;

        JobSpecTemplate.set_io_config(SerializeToYson(Spec->JobIO));

        // TODO(babenko): stderr
    }
};

IOperationControllerPtr CreateMapController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = New<TMapOperationSpec>();
    try {
        spec->Load(~operation->GetSpec());
    } catch (const std::exception& ex) {
        ythrow yexception() << Sprintf("Error parsing operation spec\n%s", ex.what());
    }

    return New<TMapController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

