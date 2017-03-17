#include "merge_controller.h"
#include "sorted_controller.h"
#include "private.h"
#include "chunk_list_pool.h"
#include "chunk_pool.h"
#include "helpers.h"
#include "job_memory.h"
#include "map_controller.h"
#include "operation_controller_detail.h"

#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_scraper.h>
#include <yt/ytlib/chunk_client/input_chunk_slice.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/chunk_slice_fetcher.h>
#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/core/concurrency/periodic_yielder.h>

#include <yt/core/misc/numeric_helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NYTree;
using namespace NYPath;
using namespace NYson;
using namespace NJobProxy;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NScheduler::NProto;
using namespace NChunkClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NConcurrency;
using namespace NTableClient;

using NChunkClient::TReadRange;
using NChunkClient::TReadLimit;
using NTableClient::TKey;

////////////////////////////////////////////////////////////////////

static const NProfiling::TProfiler Profiler("/operations/merge");

////////////////////////////////////////////////////////////////////

class TMergeControllerBase
    : public TOperationControllerBase
{
public:
    TMergeControllerBase(
        TSchedulerConfigPtr config,
        TSimpleOperationSpecBasePtr spec,
        TSimpleOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TOperationControllerBase(config, spec, options, host, operation)
        , Spec(spec)
        , Options(options)
        , TotalChunkCount(0)
        , TotalDataSize(0)
        , CurrentTaskDataSize(0)
        , CurrentTaskChunkCount(0)
        , CurrentPartitionIndex(0)
        , MaxDataSizePerJob(0)
        , ChunkSliceSize(0)
        , IsExplicitJobCount(false)
    { }

    // Persistence.

    virtual void Persist(const TPersistenceContext& context) override
    {
        TOperationControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, TotalChunkCount);
        Persist(context, TotalDataSize);
        Persist(context, JobIOConfig);
        Persist(context, JobSpecTemplate);
        Persist(context, MaxDataSizePerJob);
        Persist(context, ChunkSliceSize);
        Persist(context, IsExplicitJobCount);
        Persist(context, MergeTaskGroup);
    }

protected:
    TSimpleOperationSpecBasePtr Spec;
    TSimpleOperationOptionsPtr Options;

    //! The total number of chunks for processing (teleports excluded).
    int TotalChunkCount;

    //! The total data size for processing (teleports excluded).
    i64 TotalDataSize;

    //! For each input table, the corresponding entry holds the stripe
    //! containing the chunks collected so far.
    //! Not serialized.
    /*!
     *  Empty stripes are never stored explicitly and are denoted by |nullptr|.
     */
    std::vector<TChunkStripePtr> CurrentTaskStripes;

    //! The total data size accumulated in #CurrentTaskStripes.
    //! Not serialized.
    i64 CurrentTaskDataSize;

    //! The total number of chunks in #CurrentTaskStripes.
    //! Not serialized.
    int CurrentTaskChunkCount;

    //! The number of output partitions generated so far.
    //! Not serialized.
    /*!
     *  Each partition either corresponds to a merge task or to a teleport chunk.
     *  Partition index is used as a key when calling #TOperationControllerBase::RegisterOutputChunkTree.
     */
    int CurrentPartitionIndex;

    //! Customized job IO config.
    TJobIOConfigPtr JobIOConfig;

    //! The template for starting new jobs.
    TJobSpec JobSpecTemplate;

    //! Overrides the spec limit to satisfy global job count limit.
    i64 MaxDataSizePerJob;
    i64 ChunkSliceSize;

    //! Flag set when job count was explicitly specified.
    bool IsExplicitJobCount;

    //! Indicates if input table chunks can be teleported to output table.
    std::vector<bool> IsInputTableTeleportable;

    class TMergeTask
        : public TTask
    {
    public:
        //! For persistence only.
        TMergeTask()
            : Controller(nullptr)
            , TaskIndex(-1)
            , PartitionIndex(-1)
        { }

        TMergeTask(
            TMergeControllerBase* controller,
            int taskIndex,
            int partitionIndex = -1)
            : TTask(controller)
            , Controller(controller)
            , ChunkPool(CreateAtomicChunkPool())
            , TaskIndex(taskIndex)
            , PartitionIndex(partitionIndex)
        { }

        virtual Stroka GetId() const override
        {
            return
                PartitionIndex < 0
                ? Format("Merge(%v)", TaskIndex)
                : Format("Merge(%v,%v)", TaskIndex, PartitionIndex);
        }

        virtual TTaskGroupPtr GetGroup() const override
        {
            return Controller->MergeTaskGroup;
        }

        virtual TDuration GetLocalityTimeout() const override
        {
            return Controller->Spec->LocalityTimeout;
        }

        virtual TExtendedJobResources GetNeededResources(TJobletPtr joblet) const override
        {
            return GetMergeResources(joblet->InputStripeList->GetStatistics());
        }

        virtual IChunkPoolInput* GetChunkPoolInput() const override
        {
            return ChunkPool.get();
        }

        virtual IChunkPoolOutput* GetChunkPoolOutput() const override
        {
            return ChunkPool.get();
        }

        virtual void Persist(const TPersistenceContext& context) override
        {
            TTask::Persist(context);

            using NYT::Persist;
            Persist(context, Controller);
            Persist(context, ChunkPool);
            Persist(context, TaskIndex);
            Persist(context, PartitionIndex);
        }

        virtual TUserJobSpecPtr GetUserJobSpec() const override
        {
            return Controller->GetUserJobSpec();
        }

        virtual EJobType GetJobType() const override
        {
            return Controller->GetJobType();
        }

    protected:
        void BuildInputOutputJobSpec(TJobletPtr joblet, TJobSpec* jobSpec)
        {
            AddParallelInputSpec(jobSpec, joblet);
            AddFinalOutputSpecs(jobSpec, joblet);
        }

    private:
        DECLARE_DYNAMIC_PHOENIX_TYPE(TMergeTask, 0x72736bac);

        TMergeControllerBase* Controller;

        std::unique_ptr<IChunkPool> ChunkPool;

        //! The position in #TMergeControllerBase::Tasks.
        int TaskIndex;

        //! Key for #TOutputTable::OutputChunkTreeIds.
        int PartitionIndex;

        virtual TExtendedJobResources GetMinNeededResourcesHeavy() const override
        {
            return GetMergeResources(ChunkPool->GetApproximateStripeStatistics());
        }

        TExtendedJobResources GetMergeResources(
            const TChunkStripeStatisticsVector& statistics) const
        {
            TExtendedJobResources result;
            result.SetUserSlots(1);
            result.SetCpu(Controller->GetCpuLimit());
            result.SetJobProxyMemory(Controller->GetFinalIOMemorySize(
                    Controller->Spec->JobIO,
                    UpdateChunkStripeStatistics(statistics)));
            AddFootprintAndUserJobResources(result);
            return result;
        }

        TChunkStripeStatisticsVector UpdateChunkStripeStatistics(
            const TChunkStripeStatisticsVector& statistics) const
        {
            if (Controller->IsSingleStripeInput()) {
                return AggregateStatistics(statistics);
            } else {
                return statistics;
            }
        }

        virtual void BuildJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
        {
            jobSpec->CopyFrom(Controller->JobSpecTemplate);
            BuildInputOutputJobSpec(joblet, jobSpec);
        }

        virtual void OnJobCompleted(TJobletPtr joblet, const TCompletedJobSummary& jobSummary) override
        {
            TTask::OnJobCompleted(joblet, jobSummary);

            RegisterOutput(joblet, PartitionIndex, jobSummary);
        }

        virtual void OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& jobSummary) override
        {
            TTask::OnJobAborted(joblet, jobSummary);
        }
    };

    typedef TIntrusivePtr<TMergeTask> TMergeTaskPtr;

    TTaskGroupPtr MergeTaskGroup;

    virtual bool IsRowCountPreserved() const override
    {
        return true;
    }

    //! Resizes #CurrentTaskStripes appropriately and sets all its entries to |NULL|.
    void ResetCurrentTaskStripes()
    {
        CurrentTaskStripes.clear();
        CurrentTaskStripes.resize(InputTables.size());
        CurrentTaskDataSize = 0;
        CurrentTaskChunkCount = 0;
    }

    void EndTask(TMergeTaskPtr task, TKey breakpointKey = TKey())
    {
        YCHECK(HasActiveTask());

        std::vector<TChunkStripePtr> taskStripes;
        i64 taskDataSize =  0;
        int taskChunkCount = 0;

        if (!breakpointKey) {
            taskDataSize = CurrentTaskDataSize;
            taskChunkCount = CurrentTaskChunkCount;
            taskStripes = std::move(CurrentTaskStripes);
            ResetCurrentTaskStripes();
        } else {
            auto pendingStripes = std::move(CurrentTaskStripes);
            ResetCurrentTaskStripes();

            taskStripes.resize(pendingStripes.size(), nullptr);

            auto addSlice = [&] (const TInputDataSlicePtr& dataSlice) {
                ++taskChunkCount;
                taskDataSize += dataSlice->GetDataSize();
                AddSliceToStripe(dataSlice, taskStripes);
            };

            for (const auto& stripe : pendingStripes) {
                if (!stripe) {
                    continue;
                }

                for (const auto& dataSlice : stripe->DataSlices) {
                    if (dataSlice->UpperLimit().Key <= breakpointKey) {
                        addSlice(dataSlice);
                    } else if (dataSlice->LowerLimit().Key >= breakpointKey) {
                        AddPendingDataSlice(dataSlice);
                    } else {
                        auto lowerSlice = CreateInputDataSlice(dataSlice, TKey(), breakpointKey);
                        addSlice(lowerSlice);

                        auto upperSlice = CreateInputDataSlice(dataSlice, breakpointKey, TKey());
                        AddPendingDataSlice(upperSlice);
                    }
                }
            }
        }

        task->AddInput(taskStripes);
        task->FinishInput();

        if (task->IsCompleted()) {
            // This task is useless, e.g. all input stripes are from foreign tables.
            return;
        }

        RegisterTask(task);

        LOG_DEBUG("Task finished (Id: %v, TaskDataSize: %v, TaskChunkCount: %v, BreakpointKey: %v)",
            task->GetId(),
            taskDataSize,
            taskChunkCount,
            breakpointKey);

        TotalDataSize += taskDataSize;
        TotalChunkCount += taskChunkCount;

        // Don't validate this limit if operation is already running.
        if (!IsPrepared() && TotalChunkCount > Config->MaxTotalSliceCount) {
            THROW_ERROR_EXCEPTION("Total number of data slices in operation is too large. Consider reducing job count or reducing chunk count in input tables.")
                << TErrorAttribute("actual_total_slice_count", TotalChunkCount)
                << TErrorAttribute("max_total_slice_count", Config->MaxTotalSliceCount)
                << TErrorAttribute("current_job_count", CurrentPartitionIndex);
        }

        ++CurrentPartitionIndex;
    }

    void EndTaskAtKey(TKey breakpointKey)
    {
        YCHECK(HasActiveTask());

        auto task = New<TMergeTask>(
            this,
            static_cast<int>(Tasks.size()),
            CurrentPartitionIndex);
        task->Initialize();

        EndTask(task, breakpointKey);
    }

    //! Finishes the current task.
    virtual void EndTaskIfActive()
    {
        if (!HasActiveTask())
            return;

        EndTaskAtKey(TKey());
    }

    //! Finishes the current task if the size is large enough.
    void EndTaskIfLarge()
    {
        if (HasLargeActiveTask()) {
            EndTaskIfActive();
        }
    }

    //! Returns True if some stripes are currently queued.
    bool HasActiveTask()
    {
        return CurrentTaskDataSize > 0;
    }

    //! Returns True if the total data size of currently queued stripes exceeds the pre-configured limit
    //! or number of stripes greater than pre-configured limit.
    bool HasLargeActiveTask()
    {
        YCHECK(MaxDataSizePerJob > 0);
        return CurrentTaskDataSize >= MaxDataSizePerJob || CurrentTaskChunkCount >= Options->MaxDataSlicesPerJob;
    }

    void AddSliceToStripe(const TInputDataSlicePtr& dataSlice, std::vector<TChunkStripePtr>& stripes)
    {
        auto tableIndex = dataSlice->GetTableIndex();
        auto stripe = stripes[tableIndex];

        if (!stripe) {
            stripe = stripes[tableIndex] = New<TChunkStripe>(InputTables[tableIndex].IsForeign());
        }

        stripe->DataSlices.push_back(dataSlice);
    }

    //! Add chunk to the current task's pool.
    virtual void AddPendingDataSlice(const TInputDataSlicePtr& dataSlice)
    {
        AddSliceToStripe(dataSlice, CurrentTaskStripes);

        i64 sliceDataSize = dataSlice->GetDataSize();

        CurrentTaskDataSize += sliceDataSize;
        ++CurrentTaskChunkCount;
    }

    //! Add chunk directly to the output.
    void AddTeleportChunk(TInputChunkPtr chunkSpec)
    {
        auto tableIndex = GetTeleportTableIndex();
        if (tableIndex) {
            LOG_TRACE("Teleport chunk added (ChunkId: %v, Partition: %v)",
                chunkSpec->ChunkId(),
                CurrentPartitionIndex);

            // Place the chunk directly to the output table.
            RegisterOutput(chunkSpec, CurrentPartitionIndex, *tableIndex);
            ++CurrentPartitionIndex;
        }
    }

    //! Create new task from unread input data slices.
    void AddTaskForUnreadInputDataSlices(std::vector<TInputDataSlicePtr> inputDataSlices)
    {
        CurrentTaskDataSize = 0;
        CurrentTaskChunkCount = 0;
        ResetCurrentTaskStripes();

        for (auto& inputDataSlice : inputDataSlices) {
            AddPendingDataSlice(inputDataSlice);
        }
        EndTaskIfActive();
    }

    // Custom bits of preparation pipeline.

    virtual bool IsCompleted() const override
    {
        return Tasks.size() == JobCounter.GetCompleted();
    }

    virtual void DoInitialize() override
    {
        TOperationControllerBase::DoInitialize();

        MergeTaskGroup = New<TTaskGroup>();
        MergeTaskGroup->MinNeededResources.SetCpu(GetCpuLimit());

        RegisterTaskGroup(MergeTaskGroup);
    }

    virtual void CustomPrepare() override
    {
        CalculateSizes();
        ProcessInputs();
        EndInputChunks();
        FinishPreparation();
    }

    void CalculateSizes()
    {
        auto jobSizeConstraints = CreateSimpleJobSizeConstraints(
            Spec,
            Options,
            PrimaryInputDataSize);

        MaxDataSizePerJob = jobSizeConstraints->GetDataSizePerJob();
        ChunkSliceSize = jobSizeConstraints->GetInputSliceDataSize();
        IsExplicitJobCount = jobSizeConstraints->IsExplicitJobCount();

        LOG_INFO("Calculated operation parameters (JobCount: %v, MaxDataSizePerJob: %v, ChunkSliceSize: %v, IsExplicitJobCount: %v)",
            jobSizeConstraints->GetJobCount(),
            MaxDataSizePerJob,
            ChunkSliceSize,
            IsExplicitJobCount);
    }

    void ProcessInputs()
    {
        PROFILE_TIMING ("/input_processing_time") {
            LOG_INFO("Processing inputs");

            TPeriodicYielder yielder(PrepareYieldPeriod);

            InitTeleportableInputTables();

            ResetCurrentTaskStripes();

            for (const auto& chunk : CollectPrimaryUnversionedChunks()) {
                ProcessInputDataSlice(CreateUnversionedInputDataSlice(CreateInputChunkSlice(chunk)));
                yielder.TryYield();
            }
            for (const auto& slice : CollectPrimaryVersionedDataSlices(ChunkSliceSize)) {
                ProcessInputDataSlice(slice);
                yielder.TryYield();
            }
        }
    }

    void FinishPreparation()
    {
        InitJobIOConfig();
        InitJobSpecTemplate();

        LOG_INFO("Inputs processed (JobDataSize: %v, JobChunkCount: %v, JobCount: %v)",
            TotalDataSize,
            TotalChunkCount,
            Tasks.size());
    }

    //! Called for each input chunk.
    virtual void ProcessInputDataSlice(TInputDataSlicePtr dataSlice) = 0;

    //! Called at the end of input chunks scan.
    void EndInputChunks()
    {
        // Close the last task, if any.
        if (CurrentTaskDataSize > 0) {
            EndTaskIfActive();
        }
    }

    // Progress reporting.

    virtual Stroka GetLoggingProgress() const override
    {
        return Format(
            "Jobs = {T: %v, R: %v, C: %v, P: %v, F: %v, A: %v, I: %v}, "
            "UnavailableInputChunks: %v",
            JobCounter.GetTotal(),
            JobCounter.GetRunning(),
            JobCounter.GetCompleted(),
            GetPendingJobCount(),
            JobCounter.GetFailed(),
            JobCounter.GetAbortedTotal(),
            JobCounter.GetInterrupted(),
            UnavailableInputChunkCount);
    }


    // Unsorted helpers.
    virtual TCpuResource GetCpuLimit() const
    {
        return 1;
    }

    virtual bool IsSingleStripeInput() const
    {
        return true;
    }

    virtual TNullable<int> GetTeleportTableIndex() const
    {
        return MakeNullable(0);
    }

    //! Returns True if the chunk can be included into the output as-is.
    virtual bool IsTeleportChunk(const TInputChunkPtr& chunkSpec) const = 0;

    virtual i64 GetUserJobMemoryReserve() const
    {
        return 0;
    }

    //! A typical implementation of #IsTeleportChunk that depends on whether chunks must be combined or not.
    bool IsTeleportChunkImpl(const TInputChunkPtr& chunkSpec, bool combineChunks) const
    {
        if (chunkSpec->Channel() || !IsInputTableTeleportable[chunkSpec->GetTableIndex()]) {
            return false;
        }

        return combineChunks
            ? chunkSpec->IsLargeCompleteChunk(Spec->JobIO->TableWriter->DesiredChunkSize)
            : chunkSpec->IsCompleteChunk();
    }

    //! Initializes #JobIOConfig and #TableReaderOptions.
    void InitJobIOConfig()
    {
        JobIOConfig = CloneYsonSerializable(Spec->JobIO);
        InitFinalOutputConfig(JobIOConfig);
    }

    virtual TUserJobSpecPtr GetUserJobSpec() const
    {
        return nullptr;
    }

    virtual EJobType GetJobType() const = 0;

    //! Initializes #JobSpecTemplate.
    virtual void InitJobSpecTemplate() = 0;

    //! Initialize IsInputTableTeleportable
    virtual void InitTeleportableInputTables()
    {
        IsInputTableTeleportable.resize(InputTables.size());
        auto tableIndex = GetTeleportTableIndex();
        if (tableIndex) {
            for (int index = 0; index < InputTables.size(); ++index) {
                if (!InputTables[index].IsDynamic) {
                    IsInputTableTeleportable[index] = ValidateTableSchemaCompatibility(
                        InputTables[index].Schema,
                        OutputTables[*tableIndex].TableUploadOptions.TableSchema,
                        false).IsOK();
                }
            }
        }
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TMergeControllerBase::TMergeTask);

////////////////////////////////////////////////////////////////////

//! Handles ordered merge and (sic!) erase operations.
class TOrderedMergeControllerBase
    : public TMergeControllerBase
{
public:
    TOrderedMergeControllerBase(
        TSchedulerConfigPtr config,
        TSimpleOperationSpecBasePtr spec,
        TSimpleOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TMergeControllerBase(config, spec, options, host, operation)
    { }

private:
    virtual void ProcessInputDataSlice(TInputDataSlicePtr slice) override
    {
        if (slice->Type == EDataSourceType::UnversionedTable) {
            const auto& chunkSpec = slice->GetSingleUnversionedChunkOrThrow();
            if (IsTeleportChunk(chunkSpec)) {
                // Merge is not needed. Copy the chunk directly to the output.
                EndTaskIfActive();
                AddTeleportChunk(chunkSpec);
                return;
            }

            // NB: During ordered merge all chunks go to a single chunk stripe.
            for (const auto& chunkSlice : SliceChunkByRowIndexes(chunkSpec, ChunkSliceSize, std::numeric_limits<i64>::max())) {
                AddPendingDataSlice(CreateUnversionedInputDataSlice(chunkSlice));
                EndTaskIfLarge();
            }
        } else {
            AddPendingDataSlice(slice);
            EndTaskIfLarge();
        }
    }
};

////////////////////////////////////////////////////////////////////

class TOrderedMapController
    : public TOrderedMergeControllerBase
{
public:
    TOrderedMapController(
        TSchedulerConfigPtr config,
        TMapOperationSpecPtr spec,
        TMapOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TOrderedMergeControllerBase(config, spec, options, host, operation)
        , Spec(spec)
    {
        RegisterJobProxyMemoryDigest(EJobType::OrderedMap, spec->JobProxyMemoryDigest);
        RegisterUserJobMemoryDigest(EJobType::OrderedMap, spec->Mapper->MemoryReserveFactor);
    }

    virtual void BuildBriefSpec(IYsonConsumer* consumer) const override
    {
        TOrderedMergeControllerBase::BuildBriefSpec(consumer);
        BuildYsonMapFluently(consumer)
            .Item("mapper").BeginMap()
                .Item("command").Value(TrimCommandForBriefSpec(Spec->Mapper->Command))
            .EndMap();
    }

    // Persistence.
    virtual void Persist(const TPersistenceContext& context) override
    {
        TOrderedMergeControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, StartRowIndex);
    }

protected:
    virtual TStringBuf GetDataSizeParameterNameForJob(EJobType jobType) const override
    {
        return STRINGBUF("data_size_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::OrderedMap};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TOrderedMapController, 0x1e5a7e32);

    TMapOperationSpecPtr Spec;

    i64 StartRowIndex = 0;

    virtual TUserJobSpecPtr GetUserJobSpec() const override
    {
        return Spec->Mapper;
    }

    virtual bool IsRowCountPreserved() const override
    {
        return false;
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return Spec->OutputTablePaths;
    }

    virtual TNullable<TRichYPath> GetStderrTablePath() const override
    {
        return Spec->StderrTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetStderrTableWriterConfig() const override
    {
        return Spec->StderrTableWriterConfig;
    }

    virtual TNullable<TRichYPath> GetCoreTablePath() const override
    {
        return Spec->CoreTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetCoreTableWriterConfig() const override
    {
        return Spec->CoreTableWriterConfig;
    }

    virtual TNullable<int> GetTeleportTableIndex() const override
    {
        Y_UNREACHABLE();
    }

    virtual bool IsTeleportChunk(const TInputChunkPtr& chunkSpec) const override
    {
        return false;
    }

    virtual void InitTeleportableInputTables() override
    { }

    virtual std::vector<TPathWithStage> GetFilePaths() const override
    {
        std::vector<TPathWithStage> result;
        for (const auto& path : Spec->Mapper->FilePaths) {
            result.push_back(std::make_pair(path, EOperationStage::Map));
        }
        return result;
    }

    virtual void DoInitialize() override
    {
        TOrderedMergeControllerBase::DoInitialize();

        ValidateUserFileCount(Spec->Mapper, "mapper");
    }

    virtual bool IsOutputLivePreviewSupported() const override
    {
        return true;
    }

    virtual void ReinstallUnreadInputDataSlices(const std::vector<TInputDataSlicePtr>& inputDataSlices) override
    {
        AddTaskForUnreadInputDataSlices(inputDataSlices);
    }

    // Unsorted helpers.
    virtual bool IsJobInterruptible() const override
    {
        return !IsExplicitJobCount;
    }

    virtual TCpuResource GetCpuLimit() const override
    {
        return Spec->Mapper->CpuLimit;
    }

    virtual i64 GetUserJobMemoryReserve() const override
    {
        return ComputeUserJobMemoryReserve(EJobType::OrderedMap, Spec->Mapper);
    }

    virtual void InitJobSpecTemplate() override
    {
        JobSpecTemplate.set_type(static_cast<int>(EJobType::OrderedMap));
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec->JobIO)).GetData());

        ToProto(schedulerJobSpecExt->mutable_data_source_directory(), MakeInputDataSources());

        if (Spec->InputQuery) {
            InitQuerySpec(schedulerJobSpecExt, Spec->InputQuery.Get(), Spec->InputSchema.Get());
        }

        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig).GetData());

        InitUserJobSpecTemplate(
            schedulerJobSpecExt->mutable_user_job_spec(),
            Spec->Mapper,
            Files,
            Spec->JobNodeAccount);
    }

    virtual void CustomizeJoblet(TJobletPtr joblet) override
    {
        joblet->StartRowIndex = StartRowIndex;
        StartRowIndex += joblet->InputStripeList->TotalRowCount;
    }

    virtual void CustomizeJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
    {
        auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        InitUserJobSpec(
            schedulerJobSpecExt->mutable_user_job_spec(),
            joblet);
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::OrderedMap;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TOrderedMapController);

////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateOrderedMapController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TMapOperationSpec>(operation->GetSpec());
    return New<TOrderedMapController>(config, spec, config->MapOperationOptions, host, operation);
}

////////////////////////////////////////////////////////////////////

class TOrderedMergeController
    : public TOrderedMergeControllerBase
{
public:
    TOrderedMergeController(
        TSchedulerConfigPtr config,
        TOrderedMergeOperationSpecPtr spec,
        TOrderedMergeOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TOrderedMergeControllerBase(config, spec, options, host, operation)
        , Spec(spec)
    {
        RegisterJobProxyMemoryDigest(EJobType::OrderedMerge, spec->JobProxyMemoryDigest);
    }

protected:
    virtual TStringBuf GetDataSizeParameterNameForJob(EJobType jobType) const override
    {
        return STRINGBUF("data_size_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::OrderedMerge};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TOrderedMergeController, 0x1f748c56);

    TOrderedMergeOperationSpecPtr Spec;

    virtual void PrepareOutputTables() override
    {
        auto& table = OutputTables[0];

        switch (Spec->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    InferSchemaFromInputOrdered();
                } else {
                    ValidateOutputSchemaOrdered();
                    for (const auto& inputTable : InputTables) {
                        if (inputTable.SchemaMode == ETableSchemaMode::Strong) {
                            ValidateTableSchemaCompatibility(
                                inputTable.Schema,
                                table.TableUploadOptions.TableSchema,
                                /* ignoreSortOrder */ true)
                                .ThrowOnError();
                        }
                    }
                }
                break;

            case ESchemaInferenceMode::FromInput:
                InferSchemaFromInputOrdered();
                break;

            case ESchemaInferenceMode::FromOutput:
                break;

            default:
                Y_UNREACHABLE();
        }
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        std::vector<TRichYPath> result;
        result.push_back(Spec->OutputTablePath);
        return result;
    }

    virtual bool IsTeleportChunk(const TInputChunkPtr& chunkSpec) const override
    {
        if (Spec->ForceTransform) {
            return false;
        }

        return IsTeleportChunkImpl(chunkSpec, Spec->CombineChunks);
    }

    virtual bool IsBoundaryKeysFetchEnabled() const override
    {
        // Required for chunk teleporting in case of sorted output.
        return OutputTables[0].TableUploadOptions.TableSchema.IsSorted();
    }

    virtual bool IsRowCountPreserved() const override
    {
        return Spec->InputQuery ? false : TMergeControllerBase::IsRowCountPreserved();
    }

    virtual void InitJobSpecTemplate() override
    {
        JobSpecTemplate.set_type(static_cast<int>(EJobType::OrderedMerge));
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec->JobIO)).GetData());

        ToProto(schedulerJobSpecExt->mutable_data_source_directory(), MakeInputDataSources());

        if (Spec->InputQuery) {
            InitQuerySpec(schedulerJobSpecExt, Spec->InputQuery.Get(), Spec->InputSchema.Get());
        }

        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig).GetData());
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::OrderedMerge;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TOrderedMergeController);

////////////////////////////////////////////////////////////////////

class TEraseController
    : public TOrderedMergeControllerBase
{
public:
    TEraseController(
        TSchedulerConfigPtr config,
        TEraseOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TOrderedMergeControllerBase(config, spec, config->EraseOperationOptions, host, operation)
        , Spec(spec)
    {
        RegisterJobProxyMemoryDigest(EJobType::OrderedMerge, spec->JobProxyMemoryDigest);
    }

    virtual void BuildBriefSpec(IYsonConsumer* consumer) const override
    {
        TOrderedMergeControllerBase::BuildBriefSpec(consumer);
        BuildYsonMapFluently(consumer)
            // In addition to "input_table_paths" and "output_table_paths".
            // Quite messy, only needed for consistency with the regular spec.
            .Item("table_path").Value(Spec->TablePath);
    }

protected:
    virtual TStringBuf GetDataSizeParameterNameForJob(EJobType jobType) const override
    {
        Y_UNREACHABLE();
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TEraseController, 0x1cc6ba39);

    TEraseOperationSpecPtr Spec;

    virtual bool IsRowCountPreserved() const override
    {
        return false;
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return {Spec->TablePath};
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return {Spec->TablePath};
    }

    virtual bool IsTeleportChunk(const TInputChunkPtr& chunkSpec) const override
    {
        return IsTeleportChunkImpl(chunkSpec, Spec->CombineChunks);
    }

    virtual bool IsBoundaryKeysFetchEnabled() const override
    {
        // Required for chunk teleporting in case of sorted output.
        return OutputTables[0].TableUploadOptions.TableSchema.IsSorted();
    }

    virtual void DoInitialize() override
    {
        TOrderedMergeControllerBase::DoInitialize();

        // For erase operation the rowset specified by the user must actually be negated.
        {
            auto& path = InputTables[0].Path;
            auto ranges = path.GetRanges();
            if (ranges.size() > 1) {
                THROW_ERROR_EXCEPTION("Erase operation does not support tables with multiple ranges");
            }

            if (ranges.size() == 1) {
                std::vector<TReadRange> complementaryRanges;
                const auto& range = ranges[0];
                if (!range.LowerLimit().IsTrivial()) {
                    complementaryRanges.push_back(TReadRange(TReadLimit(), range.LowerLimit()));
                }
                if (!range.UpperLimit().IsTrivial()) {
                    complementaryRanges.push_back(TReadRange(range.UpperLimit(), TReadLimit()));
                }
                path.SetRanges(complementaryRanges);
            } else {
                path.SetRanges(std::vector<TReadRange>());
            }
        }
    }

    virtual void PrepareOutputTables() override
    {
        auto& table = OutputTables[0];
        table.TableUploadOptions.UpdateMode = EUpdateMode::Overwrite;
        table.TableUploadOptions.LockMode = ELockMode::Exclusive;

        // Sorted merge output MUST be sorted.
        table.Options->ExplodeOnValidationError = true;

        switch (Spec->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    InferSchemaFromInputOrdered();
                } else {
                    if (InputTables[0].SchemaMode == ETableSchemaMode::Strong) {
                        ValidateTableSchemaCompatibility(
                            InputTables[0].Schema,
                            table.TableUploadOptions.TableSchema,
                            /* ignoreSortOrder */ false)
                            .ThrowOnError();
                    }
                }
                break;

            case ESchemaInferenceMode::FromInput:
                InferSchemaFromInputOrdered();
                break;

            case ESchemaInferenceMode::FromOutput:
                break;

            default:
                Y_UNREACHABLE();
        }
    }

    virtual void InitJobSpecTemplate() override
    {
        JobSpecTemplate.set_type(static_cast<int>(EJobType::OrderedMerge));
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec->JobIO)).GetData());

        ToProto(schedulerJobSpecExt->mutable_data_source_directory(), MakeInputDataSources());

        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig).GetData());

        auto* jobSpecExt = JobSpecTemplate.MutableExtension(TMergeJobSpecExt::merge_job_spec_ext);
        // If the input is sorted then the output must also be sorted.
        // To produce sorted output a job needs key columns.
        const auto& table = InputTables[0];
        if (table.Schema.IsSorted()) {
            ToProto(jobSpecExt->mutable_key_columns(), table.Schema.GetKeyColumns());
        }
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::OrderedMerge;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TEraseController);

IOperationControllerPtr CreateEraseController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TEraseOperationSpec>(operation->GetSpec());
    return New<TEraseController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EEndpointType,
    (Left)
    (Right)
);

//! Handles sorted merge and reduce operations.
class TLegacySortedMergeControllerBase
    : public TMergeControllerBase
{
public:
    TLegacySortedMergeControllerBase(
        TSchedulerConfigPtr config,
        TSimpleOperationSpecBasePtr spec,
        TSortedMergeOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TMergeControllerBase(config, spec, options, host, operation)
    { }

    // Persistence.
    virtual void Persist(const TPersistenceContext& context) override
    {
        TMergeControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, Endpoints);
        Persist(context, SortKeyColumns);
        Persist(context, ManiacJobSpecTemplate);
    }

protected:
    class TManiacTask
        : public TMergeTask
    {
    public:
        //! For persistence only.
        TManiacTask()
            : Controller(nullptr)
        { }

        TManiacTask(
            TLegacySortedMergeControllerBase* controller,
            int taskIndex,
            int partitionIndex)
            : TMergeTask(controller, taskIndex, partitionIndex)
            , Controller(controller)
        { }

        virtual void Persist(const TPersistenceContext& context) override
        {
            TMergeTask::Persist(context);

            using NYT::Persist;
            Persist(context, Controller);
        }

    private:
        DECLARE_DYNAMIC_PHOENIX_TYPE(TManiacTask, 0xb3ed19a2);

        TLegacySortedMergeControllerBase* Controller;

        virtual void BuildJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
        {
            jobSpec->CopyFrom(Controller->ManiacJobSpecTemplate);
            BuildInputOutputJobSpec(joblet, jobSpec);
        }

    };

    struct TKeyEndpoint
    {
        EEndpointType Type;
        TInputDataSlicePtr DataSlice;
        TKey MinBoundaryKey;
        TKey MaxBoundaryKey;
        bool Teleport;

        void Persist(const TPersistenceContext& context)
        {
            using NYT::Persist;
            Persist(context, Type);
            Persist(context, DataSlice);
            Persist(context, MinBoundaryKey);
            Persist(context, MaxBoundaryKey);
            Persist(context, Teleport);
        }

        TKey GetKey() const
        {
            return Type == EEndpointType::Left
                ? MinBoundaryKey
                : MaxBoundaryKey;
        }
    };

    std::vector<TKeyEndpoint> Endpoints;

    //! The actual (adjusted) key columns.
    std::vector<Stroka> SortKeyColumns;

    IChunkSliceFetcherPtr ChunkSliceFetcher;

    TJobSpec ManiacJobSpecTemplate;

    std::vector<TInputDataSlicePtr> VersionedDataSlices;

    virtual bool ShouldSlicePrimaryTableByKeys()
    {
        return true;
    }

    virtual bool IsTeleportChunk(const TInputChunkPtr& chunkSpec) const override
    {
        Y_UNREACHABLE();
    }

    virtual bool IsSingleStripeInput() const override
    {
        return false;
    }

    virtual void PrepareOutputTables() override
    {
        // NB: we need to do this after locking input tables but before preparing ouput tables.
        AdjustKeyColumns();
    }

    virtual void CustomPrepare() override
    {
        // NB: Base member is not called intentionally.

        CalculateSizes();

        TScrapeChunksCallback scraperCallback;
        if (Spec->UnavailableChunkStrategy == EUnavailableChunkAction::Wait) {
            scraperCallback = CreateScrapeChunksSessionCallback(
                Config->ChunkScraper,
                GetCancelableInvoker(),
                Host->GetChunkLocationThrottlerManager(),
                AuthenticatedInputMasterClient,
                InputNodeDirectory,
                Logger);
        }

        ChunkSliceFetcher = CreateChunkSliceFetcher(
            Config->Fetcher,
            ChunkSliceSize,
            SortKeyColumns,
            ShouldSlicePrimaryTableByKeys(),
            InputNodeDirectory,
            GetCancelableInvoker(),
            scraperCallback,
            Host->GetMasterClient(),
            RowBuffer,
            Logger);

        ProcessInputs();

        WaitFor(ChunkSliceFetcher->Fetch())
            .ThrowOnError();

        if (ShouldSlicePrimaryTableByKeys()) {
            CollectEndpoints();

            LOG_INFO("Sorting %v endpoints", static_cast<int>(Endpoints.size()));
            SortEndpoints();

            if (GetTeleportTableIndex()) {
                FindTeleportChunks();
            }
        }
        ProcessForeignInputTables();
        BuildTasks();

        FinishPreparation();

        LOG_INFO("Tasks prepared (TaskCount: %v, EndpointCount: %v, TotalSliceCount: %v)",
            Tasks.size(),
            Endpoints.size(),
            TotalChunkCount);

        // Clear unused data, especially keys, to minimize memory footprint.
        decltype(Endpoints)().swap(Endpoints);
        ClearInputChunkBoundaryKeys();
    }

    virtual void ProcessInputDataSlice(TInputDataSlicePtr slice) override
    {
        if (slice->Type == EDataSourceType::UnversionedTable) {
            const auto& chunk = slice->GetSingleUnversionedChunkOrThrow();
            ChunkSliceFetcher->AddChunk(chunk);
        } else {
            VersionedDataSlices.push_back(slice);
        }
    }

    virtual void AdjustKeyColumns() = 0;
    virtual void SortEndpoints() = 0;
    virtual void FindTeleportChunks() = 0;
    virtual void BuildTasks() = 0;

    void CollectEndpoints()
    {
        auto processSlice = [&] (const TInputDataSlicePtr& slice) {
            if (slice->LowerLimit().Key >= slice->UpperLimit().Key) {
                // This can happen if ranges were specified.
                // Chunk slice fetcher can produce empty slices.
                return;
            }

            TKeyEndpoint leftEndpoint;
            leftEndpoint.Type = EEndpointType::Left;
            leftEndpoint.DataSlice = slice;
            leftEndpoint.MinBoundaryKey = slice->LowerLimit().Key;
            leftEndpoint.MaxBoundaryKey = slice->UpperLimit().Key;

            try {
                ValidateClientKey(leftEndpoint.MinBoundaryKey);
                ValidateClientKey(leftEndpoint.MaxBoundaryKey);
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION(
                    "Error validating sample key in input table %v",
                    GetInputTablePaths()[slice->GetTableIndex()])
                    << ex;
            }

            leftEndpoint.Teleport = false;
            Endpoints.push_back(leftEndpoint);

            TKeyEndpoint rightEndpoint = leftEndpoint;
            rightEndpoint.Type = EEndpointType::Right;
            Endpoints.push_back(rightEndpoint);
        };

        for (const auto& chunkSlice : ChunkSliceFetcher->GetChunkSlices()) {
            processSlice(CreateUnversionedInputDataSlice(chunkSlice));
        }

        for (const auto& slice : VersionedDataSlices) {
            processSlice(slice);
        }
    }

    virtual bool IsTeleportCandidate(TInputChunkPtr chunkSpec) const
    {
        return
            !(chunkSpec->LowerLimit() && chunkSpec->LowerLimit()->HasRowIndex()) &&
            !(chunkSpec->UpperLimit() && chunkSpec->UpperLimit()->HasRowIndex()) &&
            !chunkSpec->Channel();
    }

    virtual bool IsBoundaryKeysFetchEnabled() const override
    {
        return true;
    }

    virtual void ProcessForeignInputTables()
    { }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TLegacySortedMergeControllerBase::TManiacTask);

////////////////////////////////////////////////////////////////////

class TLegacySortedMergeController
    : public TLegacySortedMergeControllerBase
{
public:
    TLegacySortedMergeController(
        TSchedulerConfigPtr config,
        TSortedMergeOperationSpecPtr spec,
        TSortedMergeOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TLegacySortedMergeControllerBase(config, spec, options, host, operation)
        , Spec(spec)
    {
        RegisterJobProxyMemoryDigest(EJobType::SortedMerge, spec->JobProxyMemoryDigest);
    }

public:
    virtual TStringBuf GetDataSizeParameterNameForJob(EJobType jobType) const override
    {
        return STRINGBUF("data_size_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::SortedMerge};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TLegacySortedMergeController, 0xbc6daa18);

    TSortedMergeOperationSpecPtr Spec;

    bool IsLargeEnoughToTeleport(const TInputChunkPtr& chunkSpec)
    {
        if (!Spec->CombineChunks)
            return true;

        return chunkSpec->IsLargeCompleteChunk(Spec->JobIO->TableWriter->DesiredChunkSize);
    }

    virtual void AdjustKeyColumns() override
    {
        const auto& specKeyColumns = Spec->MergeBy;
        LOG_INFO("Spec key columns are %v", specKeyColumns);

        SortKeyColumns = CheckInputTablesSorted(specKeyColumns);
        LOG_INFO("Adjusted key columns are %v", SortKeyColumns);
    }

    virtual void SortEndpoints() override
    {
        int prefixLength = static_cast<int>(SortKeyColumns.size());
        std::sort(
            Endpoints.begin(),
            Endpoints.end(),
            [=] (const TKeyEndpoint& lhs, const TKeyEndpoint& rhs) -> bool {
                {
                    auto cmpResult = CompareRows(lhs.GetKey(), rhs.GetKey(), prefixLength);
                    if (cmpResult != 0) {
                        return cmpResult < 0;
                    }
                }

                {
                    auto cmpResult = CompareRows(lhs.MinBoundaryKey, rhs.MinBoundaryKey, prefixLength);
                    if (cmpResult != 0) {
                        return cmpResult < 0;
                    }
                }

                {
                    auto cmpResult = CompareRows(lhs.MaxBoundaryKey, rhs.MaxBoundaryKey, prefixLength);
                    if (cmpResult != 0) {
                        return cmpResult < 0;
                    }
                }

                {
                    // DataSlice address is used to identify the slices of one chunk.
                    auto cmpPtr = reinterpret_cast<intptr_t>(lhs.DataSlice.Get())
                        - reinterpret_cast<intptr_t>(rhs.DataSlice.Get());
                    if (cmpPtr != 0) {
                        return cmpPtr < 0;
                    }
                }

                return lhs.Type < rhs.Type;
            });
    }

    virtual void FindTeleportChunks() override
    {
        if (Spec->ForceTransform) {
            return;
        }

        TPeriodicYielder yielder(PrepareYieldPeriod);

        int openedSlicesCount = 0;
        TInputChunkPtr currentChunkSpec;
        int startTeleportIndex = -1;
        for (int i = 0; i < static_cast<int>(Endpoints.size()); ++i) {
            yielder.TryYield();
            const auto& endpoint = Endpoints[i];
            const auto& dataSlice = endpoint.DataSlice;

            if (dataSlice->Type == EDataSourceType::VersionedTable) {
                currentChunkSpec.Reset();
                continue;
            }

            // NB: Only unversioned tables can be teleported.
            YCHECK(dataSlice->IsTrivial());
            const auto& chunkSpec = dataSlice->GetSingleUnversionedChunkOrThrow();

            openedSlicesCount += endpoint.Type == EEndpointType::Left ? 1 : -1;

            YCHECK(chunkSpec->BoundaryKeys());
            const auto& minKey = chunkSpec->BoundaryKeys()->MinKey;
            const auto& maxKey = chunkSpec->BoundaryKeys()->MaxKey;

            if (currentChunkSpec) {
                if (chunkSpec == currentChunkSpec) {
                    if (endpoint.Type == EEndpointType::Right &&
                        CompareRows(maxKey, endpoint.MaxBoundaryKey, SortKeyColumns.size()) == 0)
                    {
                        // The last slice of a full chunk.
                        currentChunkSpec.Reset();

                        bool isManiacTeleport = CompareRows(
                            Endpoints[startTeleportIndex].GetKey(),
                            endpoint.GetKey(),
                            SortKeyColumns.size()) == 0;

                        if (IsLargeEnoughToTeleport(chunkSpec) &&
                            (openedSlicesCount == 0 || isManiacTeleport))
                        {
                            for (int j = startTeleportIndex; j <= i; ++j) {
                                Endpoints[j].Teleport = true;
                            }
                        }
                    }

                    continue;
                } else {
                    currentChunkSpec.Reset();
                }
            }


            // No current Teleport candidate.
            if (IsInputTableTeleportable[chunkSpec->GetTableIndex()] &&
                endpoint.Type == EEndpointType::Left &&
                CompareRows(minKey, endpoint.MinBoundaryKey, SortKeyColumns.size()) == 0 &&
                IsTeleportCandidate(chunkSpec))
            {
                // The first slice of a full chunk.
                currentChunkSpec = chunkSpec;
                startTeleportIndex = i;
            }
        }
    }

    virtual void BuildTasks() override
    {
        TPeriodicYielder yielder(PrepareYieldPeriod);

        const int prefixLength = static_cast<int>(SortKeyColumns.size());

        yhash_set<TInputDataSlicePtr> globalOpenedSlices;
        TKey lastBreakpoint;

        int startIndex = 0;
        while (startIndex < static_cast<int>(Endpoints.size())) {
            yielder.TryYield();
            auto key = Endpoints[startIndex].GetKey();

            std::vector<TInputChunkPtr> teleportChunks;
            yhash_set<TInputDataSlicePtr> localOpenedSlices;

            // Slices with equal left and right boundaries.
            std::vector<TInputDataSlicePtr> maniacs;

            int currentIndex = startIndex;
            while (currentIndex < static_cast<int>(Endpoints.size())) {
                // Iterate over endpoints with equal keys.
                const auto& endpoint = Endpoints[currentIndex];
                auto currentKey = endpoint.GetKey();

                if (CompareRows(key, currentKey, prefixLength) != 0) {
                    // This key is over.
                    break;
                }

                if (endpoint.Teleport) {
                    auto chunkSpec = endpoint.DataSlice->GetSingleUnversionedChunkOrThrow();
                    teleportChunks.push_back(chunkSpec);
                    while (currentIndex < static_cast<int>(Endpoints.size()) &&
                        Endpoints[currentIndex].Teleport &&
                        Endpoints[currentIndex].DataSlice->GetSingleUnversionedChunkOrThrow() == chunkSpec)
                    {
                        ++currentIndex;
                    }
                    continue;
                }

                if (endpoint.Type == EEndpointType::Left) {
                    YCHECK(localOpenedSlices.insert(endpoint.DataSlice).second);
                    ++currentIndex;
                    continue;
                }

                // Right non-Teleport endpoint.
                {
                    auto it = globalOpenedSlices.find(endpoint.DataSlice);
                    if (it != globalOpenedSlices.end()) {
                        AddPendingDataSlice(CreateInputDataSlice(*it, lastBreakpoint));
                        globalOpenedSlices.erase(it);
                        ++currentIndex;
                        continue;
                    }
                }
                {
                    auto it = localOpenedSlices.find(endpoint.DataSlice);
                    YCHECK(it != localOpenedSlices.end());
                    maniacs.push_back(*it);
                    localOpenedSlices.erase(it);
                    ++currentIndex;
                    continue;
                }

                Y_UNREACHABLE();
            }

            globalOpenedSlices.insert(localOpenedSlices.begin(), localOpenedSlices.end());

            auto endTask = [&] () {
                if (lastBreakpoint && CompareRows(key, lastBreakpoint) == 0) {
                    // Already flushed at this key.
                    return;
                }

                auto nextBreakpoint = GetKeyPrefixSuccessor(key, prefixLength, RowBuffer);
                LOG_TRACE("Finish current task, flushing %v chunks at key %v",
                    globalOpenedSlices.size(),
                    nextBreakpoint);

                for (const auto& dataSlice : globalOpenedSlices) {
                    AddPendingDataSlice(CreateInputDataSlice(dataSlice, lastBreakpoint, nextBreakpoint));
                }
                lastBreakpoint = nextBreakpoint;

                EndTaskIfActive();
            };

            auto hasLargeActiveTask = [&] () {
                return HasLargeActiveTask() ||
                    CurrentTaskChunkCount + globalOpenedSlices.size() >= Options->MaxDataSlicesPerJob;
            };

            while (!hasLargeActiveTask() && !maniacs.empty()) {
                AddPendingDataSlice(maniacs.back());
                maniacs.pop_back();
            }

            if (!maniacs.empty()) {
                endTask();

                for (auto& dataSlice : maniacs) {
                    AddPendingDataSlice(dataSlice);
                    if (HasLargeActiveTask()) {
                        EndManiacTask();
                    }
                }
                EndManiacTask();
            }

            if (!teleportChunks.empty()) {
                endTask();

                TOwningKey previousMaxKey;
                for (const auto& chunkSpec : teleportChunks) {
                    // Ensure sorted order of teleported chunks.
                    YCHECK(chunkSpec->BoundaryKeys());
                    const auto& minKey = chunkSpec->BoundaryKeys()->MinKey;
                    const auto& maxKey = chunkSpec->BoundaryKeys()->MaxKey;
                    YCHECK(CompareRows(previousMaxKey, minKey, prefixLength) <= 0);
                    previousMaxKey = maxKey;

                    AddTeleportChunk(chunkSpec);
                }
            }

            if (hasLargeActiveTask()) {
                endTask();
            }

            startIndex = currentIndex;
        }

        YCHECK(globalOpenedSlices.empty());
        EndTaskIfActive();
    }

    void EndManiacTask()
    {
        if (!HasActiveTask())
            return;

        auto task = New<TManiacTask>(
            this,
            static_cast<int>(Tasks.size()),
            CurrentPartitionIndex);
        task->Initialize();

        EndTask(task);
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        std::vector<TRichYPath> result;
        result.push_back(Spec->OutputTablePath);
        return result;
    }

    virtual void PrepareOutputTables() override
    {
        // Check that all input tables are sorted by the same key columns.
        TLegacySortedMergeControllerBase::PrepareOutputTables();

        auto& table = OutputTables[0];
        table.TableUploadOptions.LockMode = ELockMode::Exclusive;

        auto prepareOutputKeyColumns = [&] () {
            if (table.TableUploadOptions.TableSchema.IsSorted()) {
                if (table.TableUploadOptions.TableSchema.GetKeyColumns() != SortKeyColumns) {
                    THROW_ERROR_EXCEPTION("Merge key columns do not match output table schema in \"strong\" schema mode")
                            << TErrorAttribute("output_schema", table.TableUploadOptions.TableSchema)
                            << TErrorAttribute("merge_by", SortKeyColumns)
                            << TErrorAttribute("schema_inference_mode", Spec->SchemaInferenceMode);
                }
            } else {
                table.TableUploadOptions.TableSchema =
                    table.TableUploadOptions.TableSchema.ToSorted(SortKeyColumns);
            }
        };

        switch (Spec->SchemaInferenceMode) {
            case ESchemaInferenceMode::Auto:
                if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    InferSchemaFromInput(SortKeyColumns);
                } else {
                    prepareOutputKeyColumns();

                    for (const auto& inputTable : InputTables) {
                        if (inputTable.SchemaMode == ETableSchemaMode::Strong) {
                            ValidateTableSchemaCompatibility(
                                inputTable.Schema,
                                table.TableUploadOptions.TableSchema,
                                /* ignoreSortOrder */ true)
                                .ThrowOnError();
                        }
                    }
                }
                break;

            case ESchemaInferenceMode::FromInput:
                InferSchemaFromInput(SortKeyColumns);
                break;

            case ESchemaInferenceMode::FromOutput:
                if (table.TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
                    table.TableUploadOptions.TableSchema = TTableSchema::FromKeyColumns(SortKeyColumns);
                } else {
                    prepareOutputKeyColumns();
                }
                break;

            default:
                Y_UNREACHABLE();
        }
    }

    virtual void InitJobSpecTemplate() override
    {
        JobSpecTemplate.set_type(static_cast<int>(EJobType::SortedMerge));
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        auto* mergeJobSpecExt = JobSpecTemplate.MutableExtension(TMergeJobSpecExt::merge_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec->JobIO)).GetData());

        ToProto(schedulerJobSpecExt->mutable_data_source_directory(), MakeInputDataSources());
        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig).GetData());

        ToProto(mergeJobSpecExt->mutable_key_columns(), SortKeyColumns);

        ManiacJobSpecTemplate.CopyFrom(JobSpecTemplate);
        ManiacJobSpecTemplate.set_type(static_cast<int>(EJobType::UnorderedMerge));
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::SortedMerge;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TLegacySortedMergeController);

////////////////////////////////////////////////////////////////////

IOperationControllerPtr CreateMergeController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = operation->GetSpec();
    auto baseSpec = ParseOperationSpec<TMergeOperationSpec>(spec);
    switch (baseSpec->Mode) {
        case EMergeMode::Unordered: {
            return CreateUnorderedMergeController(config, host, operation);
        }
        case EMergeMode::Ordered: {
            return New<TOrderedMergeController>(
                config,
                ParseOperationSpec<TOrderedMergeOperationSpec>(spec),
                config->OrderedMergeOperationOptions,
                host,
                operation);
        }
        case EMergeMode::Sorted: {
            auto legacySpec = ParseOperationSpec<TOperationWithLegacyControllerSpec>(spec);
            if (legacySpec->UseLegacyController) {
                return New<TLegacySortedMergeController>(
                    config,
                    ParseOperationSpec<TSortedMergeOperationSpec>(spec),
                    config->SortedMergeOperationOptions,
                    host,
                    operation);
            } else {
                return CreateSortedMergeController(config, host, operation);
            }
        }
        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////

class TLegacyReduceControllerBase
    : public TLegacySortedMergeControllerBase
{
public:
    TLegacyReduceControllerBase(
        TSchedulerConfigPtr config,
        TReduceOperationSpecBasePtr spec,
        TReduceOperationOptionsPtr options,
        IOperationHost* host,
        TOperation* operation)
        : TLegacySortedMergeControllerBase(config, spec, options, host, operation)
        , Spec(spec)
    { }

    virtual void BuildBriefSpec(IYsonConsumer* consumer) const override
    {
        TLegacySortedMergeControllerBase::BuildBriefSpec(consumer);
        BuildYsonMapFluently(consumer)
            .Item("reducer").BeginMap()
                .Item("command").Value(TrimCommandForBriefSpec(Spec->Reducer->Command))
            .EndMap();
    }

    // Persistence.
    virtual void Persist(const TPersistenceContext& context) override
    {
        TLegacySortedMergeControllerBase::Persist(context);

        using NYT::Persist;
        Persist(context, StartRowIndex);
        Persist(context, ForeignKeyColumnCount);
        Persist(context, ReduceKeyColumnCount);
        Persist(context, ForeignInputDataSlices);
    }

protected:
    TReduceOperationSpecBasePtr Spec;

    i64 StartRowIndex = 0;

    //! Number of key columns for foreign tables.
    int ForeignKeyColumnCount = 0;

    //! Not serialized.
    int ReduceKeyColumnCount;

    //! Not serialized.
    TNullable<int> TeleportOutputTable;
    //! Not serialized.
    std::vector<std::deque<TInputDataSlicePtr>> ForeignInputDataSlices;

    //! Not serialized.
    TKey CurrentTaskMinForeignKey;
    //! Not serialized.
    TKey CurrentTaskMaxForeignKey;

    virtual void DoInitialize() override
    {
        TLegacySortedMergeControllerBase::DoInitialize();

        int teleportOutputCount = 0;
        for (int i = 0; i < static_cast<int>(OutputTables.size()); ++i) {
            if (OutputTables[i].Path.GetTeleport()) {
                ++teleportOutputCount;
                TeleportOutputTable = i;
            }
        }

        if (teleportOutputCount > 1) {
            THROW_ERROR_EXCEPTION("Too many teleport output tables: maximum allowed 1, actual %v",
                teleportOutputCount);
        }

        ValidateUserFileCount(Spec->Reducer, "reducer");
    }

    virtual bool IsRowCountPreserved() const override
    {
        return false;
    }

    virtual void ProcessForeignInputTables() override
    {
        ForeignInputDataSlices = CollectForeignInputDataSlices(ForeignKeyColumnCount);
    }

    void AddForeignTablesToTask(TKey foreignMinKey, TKey foreignMaxKey)
    {
        YCHECK(ForeignKeyColumnCount > 0);
        YCHECK(ForeignKeyColumnCount <= static_cast<int>(SortKeyColumns.size()));
        YCHECK(foreignMinKey.GetCount() <= ForeignKeyColumnCount);

        for (const auto& tableDataSlices : ForeignInputDataSlices) {
            for (const auto& dataSlice : tableDataSlices) {
                const auto& minKey = dataSlice->LowerLimit().Key;
                const auto& maxKey = dataSlice->UpperLimit().Key;
                if (CompareRows(foreignMinKey, maxKey, ForeignKeyColumnCount) > 0) {
                    continue;
                }

                if (CompareRows(foreignMaxKey, minKey, ForeignKeyColumnCount) < 0) {
                    break;
                }

                auto lowerKey = GetKeyPrefix(minKey, ForeignKeyColumnCount, RowBuffer);
                auto upperKey = GetKeyPrefixSuccessor(maxKey, ForeignKeyColumnCount, RowBuffer);

                if (lowerKey < foreignMinKey) {
                    lowerKey = foreignMinKey;
                }

                if (upperKey > foreignMaxKey) {
                    upperKey = foreignMaxKey;
                }

                AddPendingDataSlice(CreateInputDataSlice(
                    dataSlice,
                    lowerKey,
                    upperKey));
            }
        }
    }

    virtual void AddPendingDataSlice(const TInputDataSlicePtr& dataSlice) override
    {
        if (ForeignKeyColumnCount > 0) {
            if (!CurrentTaskMinForeignKey ||
                CompareRows(CurrentTaskMinForeignKey, dataSlice->LowerLimit().Key, ForeignKeyColumnCount) > 0)
            {
                CurrentTaskMinForeignKey = GetKeyPrefix(dataSlice->LowerLimit().Key, ForeignKeyColumnCount, RowBuffer);
            }
            if (!CurrentTaskMaxForeignKey ||
                CompareRows(CurrentTaskMaxForeignKey, dataSlice->UpperLimit().Key, ForeignKeyColumnCount) < 0)
            {
                CurrentTaskMaxForeignKey = GetKeyPrefixSuccessor(dataSlice->UpperLimit().Key, ForeignKeyColumnCount, RowBuffer);
            }
        }

        TLegacySortedMergeControllerBase::AddPendingDataSlice(dataSlice);
    }

    virtual void EndTaskIfActive() override
    {
        if (!HasActiveTask())
            return;

        if (ForeignKeyColumnCount != 0) {
            YCHECK(CurrentTaskMinForeignKey && CurrentTaskMaxForeignKey);

            AddForeignTablesToTask(CurrentTaskMinForeignKey, CurrentTaskMaxForeignKey);

            if (CurrentTaskDataSize > 2 * MaxDataSizePerJob) {
                // Task looks to large, let's try to split it further by foreign key.
                std::vector<std::pair<TKey, i64>> sliceWeights;
                for (const auto& stripe : CurrentTaskStripes) {
                    if (!stripe) {
                        continue;
                    }

                    for (const auto& dataSlice : stripe->DataSlices) {
                        sliceWeights.push_back(std::make_pair(dataSlice->UpperLimit().Key, dataSlice->GetDataSize()));
                    }
                }

                std::sort(sliceWeights.begin(), sliceWeights.end());

                i64 currentDataSize = 0;
                TKey breakpointKey;
                TPeriodicYielder yielder(PrepareYieldPeriod);
                for (const auto& sliceWeight : sliceWeights) {
                    yielder.TryYield();
                    if (CompareRows(breakpointKey, sliceWeight.first, ForeignKeyColumnCount) == 0) {
                        continue;
                    }

                    currentDataSize += sliceWeight.second;

                    if (currentDataSize > 2 * MaxDataSizePerJob && HasActiveTask()) {
                        breakpointKey = GetKeyPrefixSuccessor(sliceWeight.first, ForeignKeyColumnCount, RowBuffer);
                        currentDataSize = 0;
                        EndTaskAtKey(breakpointKey);
                    }
                }
            }
        }

        CurrentTaskMinForeignKey = TKey();
        CurrentTaskMaxForeignKey = TKey();

        TLegacySortedMergeControllerBase::EndTaskIfActive();
    }

    virtual std::vector<TRichYPath> GetInputTablePaths() const override
    {
        return Spec->InputTablePaths;
    }

    virtual std::vector<TRichYPath> GetOutputTablePaths() const override
    {
        return Spec->OutputTablePaths;
    }

    virtual TNullable<TRichYPath> GetStderrTablePath() const override
    {
        return Spec->StderrTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetStderrTableWriterConfig() const override
    {
        return Spec->StderrTableWriterConfig;
    }

    virtual TNullable<TRichYPath> GetCoreTablePath() const override
    {
        return Spec->CoreTablePath;
    }

    virtual TBlobTableWriterConfigPtr GetCoreTableWriterConfig() const override
    {
        return Spec->CoreTableWriterConfig;
    }

    virtual TNullable<int> GetTeleportTableIndex() const override
    {
        return TeleportOutputTable;
    }

    virtual std::vector<TPathWithStage> GetFilePaths() const override
    {
        std::vector<TPathWithStage> result;
        for (const auto& path : Spec->Reducer->FilePaths) {
            result.push_back(std::make_pair(path, EOperationStage::Reduce));
        }
        return result;
    }

    virtual void ReinstallUnreadInputDataSlices(const std::vector<TInputDataSlicePtr>& inputDataSlices) override
    {
        AddTaskForUnreadInputDataSlices(inputDataSlices);
    }

    // Unsorted helpers.
    virtual bool IsJobInterruptible() const override
    {
        return !IsExplicitJobCount;
    }

    virtual TCpuResource GetCpuLimit() const override
    {
        return Spec->Reducer->CpuLimit;
    }

    virtual TUserJobSpecPtr GetUserJobSpec() const override
    {
        return Spec->Reducer;
    }

    virtual i64 GetUserJobMemoryReserve() const override
    {
        return ComputeUserJobMemoryReserve(GetJobType(), Spec->Reducer);
    }

    virtual void InitJobSpecTemplate() override
    {
        YCHECK(!SortKeyColumns.empty());

        JobSpecTemplate.set_type(static_cast<int>(GetJobType()));
        auto* schedulerJobSpecExt = JobSpecTemplate.MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(ConvertToYsonString(CreateTableReaderOptions(Spec->JobIO)).GetData());

        ToProto(schedulerJobSpecExt->mutable_data_source_directory(), MakeInputDataSources());

        schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(JobIOConfig).GetData());

        InitUserJobSpecTemplate(
            schedulerJobSpecExt->mutable_user_job_spec(),
            Spec->Reducer,
            Files,
            Spec->JobNodeAccount);

        auto* reduceJobSpecExt = JobSpecTemplate.MutableExtension(TReduceJobSpecExt::reduce_job_spec_ext);
        ToProto(reduceJobSpecExt->mutable_key_columns(), SortKeyColumns);
        reduceJobSpecExt->set_reduce_key_column_count(ReduceKeyColumnCount);
        reduceJobSpecExt->set_join_key_column_count(ForeignKeyColumnCount);

        ManiacJobSpecTemplate.CopyFrom(JobSpecTemplate);
    }

    virtual void CustomizeJoblet(TJobletPtr joblet) override
    {
        joblet->StartRowIndex = StartRowIndex;
        StartRowIndex += joblet->InputStripeList->TotalRowCount;
    }

    virtual void CustomizeJobSpec(TJobletPtr joblet, TJobSpec* jobSpec) override
    {
        auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        InitUserJobSpec(
            schedulerJobSpecExt->mutable_user_job_spec(),
            joblet);
    }

    virtual bool IsOutputLivePreviewSupported() const override
    {
        for (const auto& inputTable : InputTables) {
            if (inputTable.Path.GetTeleport()) {
                return false;
            }
        }
        return true;
    }

    virtual bool IsInputDataSizeHistogramSupported() const override
    {
        return true;
    }
};

////////////////////////////////////////////////////////////////////

class TLegacyReduceController
    : public TLegacyReduceControllerBase
{
public:
    TLegacyReduceController(
        TSchedulerConfigPtr config,
        TReduceOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TLegacyReduceControllerBase(config, spec, config->ReduceOperationOptions, host, operation)
        , Spec(spec)
    {
        RegisterJobProxyMemoryDigest(EJobType::SortedReduce, spec->JobProxyMemoryDigest);
        RegisterUserJobMemoryDigest(EJobType::SortedReduce, spec->Reducer->MemoryReserveFactor);
    }

    // Persistence.
    virtual void Persist(const TPersistenceContext& context) override
    {
        TLegacyReduceControllerBase::Persist(context);
    }

protected:
    virtual TStringBuf GetDataSizeParameterNameForJob(EJobType jobType) const override
    {
        return STRINGBUF("data_size_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::SortedReduce};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TLegacyReduceController, 0xacd16dbc);

    TReduceOperationSpecPtr Spec;

    virtual void DoInitialize() override
    {
        TLegacyReduceControllerBase::DoInitialize();

        int foreignInputCount = 0;
        for (auto& table : InputTables) {
            if (table.Path.GetForeign()) {
                if (table.Path.GetTeleport()) {
                    THROW_ERROR_EXCEPTION("Foreign table can not be specified as teleport");
                }
                if (table.Path.GetRanges().size() > 1) {
                    THROW_ERROR_EXCEPTION("Reduce operation does not support foreign tables with multiple ranges");
                }
                ++foreignInputCount;
            }
        }

        if (foreignInputCount == InputTables.size()) {
            THROW_ERROR_EXCEPTION("At least one non-foreign input table is required");
        }

        if (foreignInputCount == 0 && !Spec->JoinBy.empty()) {
            THROW_ERROR_EXCEPTION("At least one foreign input table is required");
        }

        if (foreignInputCount != 0 && Spec->JoinBy.empty()) {
            THROW_ERROR_EXCEPTION("Join key columns are required");
        }
    }

    virtual void AdjustKeyColumns() override
    {
        auto sortBy = Spec->SortBy.empty() ? Spec->ReduceBy : Spec->SortBy;
        LOG_INFO("Spec key columns are %v", sortBy);

        SortKeyColumns = CheckInputTablesSorted(sortBy, &TInputTable::IsPrimary);

        if (SortKeyColumns.size() < Spec->ReduceBy.size() ||
            !CheckKeyColumnsCompatible(SortKeyColumns, Spec->ReduceBy))
        {
            THROW_ERROR_EXCEPTION("Reduce key columns %v are not compatible with sort key columns %v",
                Spec->ReduceBy,
                SortKeyColumns);
        }
        ReduceKeyColumnCount = Spec->ReduceBy.size();

        const auto& specForeignKeyColumns = Spec->JoinBy;
        ForeignKeyColumnCount = specForeignKeyColumns.size();
        if (ForeignKeyColumnCount != 0) {
            LOG_INFO("Foreign key columns are %v", specForeignKeyColumns);

            CheckInputTablesSorted(specForeignKeyColumns, &TInputTable::IsForeign);

            if (Spec->ReduceBy.size() < specForeignKeyColumns.size() ||
                !CheckKeyColumnsCompatible(Spec->ReduceBy, specForeignKeyColumns))
            {
                    THROW_ERROR_EXCEPTION("Join key columns %v are not compatible with reduce key columns %v",
                        specForeignKeyColumns,
                        Spec->ReduceBy);
            }
        }
    }

    virtual bool IsTeleportCandidate(TInputChunkPtr chunkSpec) const override
    {
        return
            TLegacySortedMergeControllerBase::IsTeleportCandidate(chunkSpec) &&
            InputTables[chunkSpec->GetTableIndex()].Path.GetTeleport();
    }

    virtual bool AreForeignTablesSupported() const override
    {
        return true;
    }

    virtual void SortEndpoints() override
    {
        std::sort(
            Endpoints.begin(),
            Endpoints.end(),
            [=] (const TKeyEndpoint& lhs, const TKeyEndpoint& rhs) -> bool {
                {
                    auto cmpResult = CompareRows(lhs.GetKey(), rhs.GetKey());
                    if (cmpResult != 0) {
                        return cmpResult < 0;
                    }
                }

                {
                    auto cmpResult = static_cast<int>(lhs.Type) - static_cast<int>(rhs.Type);
                    if (cmpResult != 0) {
                        return cmpResult < 0;
                    }
                }

                if (lhs.DataSlice->Type == EDataSourceType::UnversionedTable) {
                    // If keys (trimmed to key columns) are equal, we put slices in
                    // the same order they are in the original table.
                    const auto& lhsChunk = lhs.DataSlice->GetSingleUnversionedChunkOrThrow();
                    const auto& rhsChunk = rhs.DataSlice->GetSingleUnversionedChunkOrThrow();

                    auto cmpResult = lhsChunk->GetTableRowIndex() -
                        rhsChunk->GetTableRowIndex();
                    if (cmpResult != 0) {
                        return cmpResult < 0;
                    }
                }

                {
                    auto cmpPtr = reinterpret_cast<intptr_t>(lhs.DataSlice.Get())
                        - reinterpret_cast<intptr_t>(rhs.DataSlice.Get());
                    return cmpPtr < 0;
                }
            });
    }

    virtual void FindTeleportChunks() override
    {
        TPeriodicYielder yielder(PrepareYieldPeriod);

        const int prefixLength = ReduceKeyColumnCount;

        TInputChunkPtr currentChunkSpec;
        int startTeleportIndex = -1;

        int openedSlicesCount = 0;
        auto previousKey = EmptyKey().Get();

        for (int i = 0; i < static_cast<int>(Endpoints.size()); ++i) {
            yielder.TryYield();
            const auto& endpoint = Endpoints[i];
            auto key = endpoint.GetKey();
            const auto& dataSlice = endpoint.DataSlice;

            if (dataSlice->Type == EDataSourceType::VersionedTable) {
                currentChunkSpec.Reset();
                continue;
            }

            openedSlicesCount += endpoint.Type == EEndpointType::Left ? 1 : -1;

            if (currentChunkSpec &&
                dataSlice->GetSingleUnversionedChunkOrThrow() == currentChunkSpec)
            {
                previousKey = key;
                continue;
            }

            if (CompareRows(key, previousKey, prefixLength) == 0) {
                currentChunkSpec.Reset();
                // Don't update previous key - it's equal to current.
                continue;
            }

            if (currentChunkSpec) {
                const auto& previousEndpoint = Endpoints[i - 1];
                const auto& chunkSpec = previousEndpoint.DataSlice->GetSingleUnversionedChunkOrThrow();

                YCHECK(chunkSpec->BoundaryKeys());
                const auto& maxKey = chunkSpec->BoundaryKeys()->MaxKey;
                if (previousEndpoint.Type == EEndpointType::Right &&
                    CompareRows(maxKey, previousEndpoint.GetKey(), prefixLength) == 0)
                {
                    for (int j = startTeleportIndex; j < i; ++j) {
                        Endpoints[j].Teleport = true;
                    }
                }
            }

            currentChunkSpec.Reset();
            previousKey = key;

            // No current teleport candidate.
            const auto& chunkSpec = endpoint.DataSlice->GetSingleUnversionedChunkOrThrow();
            YCHECK(chunkSpec->BoundaryKeys());
            const auto& minKey = chunkSpec->BoundaryKeys()->MinKey;
            if (IsInputTableTeleportable[chunkSpec->GetTableIndex()] &&
                endpoint.Type == EEndpointType::Left &&
                CompareRows(minKey, endpoint.GetKey(), prefixLength) == 0 &&
                IsTeleportCandidate(chunkSpec) &&
                openedSlicesCount == 1)
            {
                currentChunkSpec = chunkSpec;
                startTeleportIndex = i;
            }
        }

        if (currentChunkSpec) {
            // Last Teleport candidate.
            auto& previousEndpoint = Endpoints.back();
            const auto& chunkSpec = previousEndpoint.DataSlice->GetSingleUnversionedChunkOrThrow();
            YCHECK(previousEndpoint.Type == EEndpointType::Right);
            YCHECK(chunkSpec->BoundaryKeys());
            const auto& maxKey = chunkSpec->BoundaryKeys()->MaxKey;
            if (CompareRows(maxKey, previousEndpoint.GetKey(), prefixLength) == 0) {
                for (int j = startTeleportIndex; j < static_cast<int>(Endpoints.size()); ++j) {
                    Endpoints[j].Teleport = true;
                }
            }
        }
    }

    virtual void BuildTasks() override
    {
        TPeriodicYielder yielder(PrepareYieldPeriod);
        const int prefixLength = ReduceKeyColumnCount;

        yhash_set<TInputDataSlicePtr> openedSlices;
        TKey lastBreakpoint;

        auto hasLargeActiveTask = [&] () {
            return HasLargeActiveTask() ||
                CurrentTaskChunkCount + openedSlices.size() >= Options->MaxDataSlicesPerJob;
        };

        int startIndex = 0;
        while (startIndex < static_cast<int>(Endpoints.size())) {
            yielder.TryYield();
            auto key = Endpoints[startIndex].GetKey();

            int currentIndex = startIndex;
            while (currentIndex < static_cast<int>(Endpoints.size())) {
                // Iterate over endpoints with equal keys.
                const auto& endpoint = Endpoints[currentIndex];
                auto currentKey = endpoint.GetKey();

                if (CompareRows(key, currentKey, prefixLength) != 0) {
                    // This key is over.
                    break;
                }

                if (endpoint.Teleport) {
                    YCHECK(openedSlices.empty());
                    EndTaskIfActive();

                    auto chunkSpec = endpoint.DataSlice->GetSingleUnversionedChunkOrThrow();
                    AddTeleportChunk(chunkSpec);

                    while (currentIndex < static_cast<int>(Endpoints.size()) &&
                        Endpoints[currentIndex].Teleport &&
                        Endpoints[currentIndex].DataSlice->GetSingleUnversionedChunkOrThrow() == chunkSpec)
                    {
                        ++currentIndex;
                    }
                    continue;
                }

                if (endpoint.Type == EEndpointType::Left) {
                    YCHECK(openedSlices.insert(endpoint.DataSlice).second);
                    ++currentIndex;
                    continue;
                }

                // Right non-Teleport endpoint.
                YCHECK(endpoint.Type == EEndpointType::Right);

                auto it = openedSlices.find(endpoint.DataSlice);
                YCHECK(it != openedSlices.end());
                AddPendingDataSlice(CreateInputDataSlice(*it, lastBreakpoint));
                openedSlices.erase(it);
                ++currentIndex;
            }

            if (hasLargeActiveTask()) {
                YCHECK(!lastBreakpoint || CompareRows(key, lastBreakpoint, prefixLength) != 0);

                auto nextBreakpoint = GetKeyPrefixSuccessor(key, prefixLength, RowBuffer);

                LOG_TRACE("Current task finished, flushing %v chunks at key %v",
                    openedSlices.size(),
                    nextBreakpoint);

                for (const auto& dataSlice : openedSlices) {
                    AddPendingDataSlice(CreateInputDataSlice(dataSlice, lastBreakpoint, nextBreakpoint));
                }

                lastBreakpoint = nextBreakpoint;

                EndTaskIfActive();
            }

            startIndex = currentIndex;
        }

        YCHECK(openedSlices.empty());
        EndTaskIfActive();
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::SortedReduce;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TLegacyReduceController);

IOperationControllerPtr CreateLegacyReduceController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TReduceOperationSpec>(operation->GetSpec());
    return New<TLegacyReduceController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

class TLegacyJoinReduceController
    : public TLegacyReduceControllerBase
{
public:
    TLegacyJoinReduceController(
        TSchedulerConfigPtr config,
        TJoinReduceOperationSpecPtr spec,
        IOperationHost* host,
        TOperation* operation)
        : TLegacyReduceControllerBase(config, spec, config->JoinReduceOperationOptions, host, operation)
        , Spec(spec)
    {
        RegisterJobProxyMemoryDigest(EJobType::JoinReduce, spec->JobProxyMemoryDigest);
        RegisterUserJobMemoryDigest(EJobType::JoinReduce, spec->Reducer->MemoryReserveFactor);
    }

    // Persistence.
    virtual void Persist(const TPersistenceContext& context) override
    {
        TLegacyReduceControllerBase::Persist(context);
    }

protected:
    virtual TStringBuf GetDataSizeParameterNameForJob(EJobType jobType) const override
    {
        return STRINGBUF("data_size_per_job");
    }

    virtual std::vector<EJobType> GetSupportedJobTypesForJobsDurationAnalyzer() const override
    {
        return {EJobType::JoinReduce};
    }

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TLegacyJoinReduceController, 0xc0fd3095);

    TJoinReduceOperationSpecPtr Spec;

    virtual void DoInitialize() override
    {
        TLegacyReduceControllerBase::DoInitialize();

        if (InputTables.size() < 2) {
            THROW_ERROR_EXCEPTION("At least two input tables are required");
        }

        int primaryInputCount = 0;
        for (int i = 0; i < static_cast<int>(InputTables.size()); ++i) {
            if (!InputTables[i].Path.GetForeign()) {
                ++primaryInputCount;
            }
            if (InputTables[i].Path.GetTeleport()) {
                THROW_ERROR_EXCEPTION("Teleport tables are not supported in join-reduce");
            }
        }

        if (primaryInputCount != 1) {
            THROW_ERROR_EXCEPTION("You must specify exactly one non-foreign (primary) input table (%v specified)",
                primaryInputCount);
        }

        // For join reduce tables with multiple ranges are not supported.
        for (int i = 0; i < static_cast<int>(InputTables.size()); ++i) {
            auto& path = InputTables[i].Path;
            auto ranges = path.GetRanges();
            if (ranges.size() > 1) {
                THROW_ERROR_EXCEPTION("Join reduce operation does not support tables with multiple ranges");
            }
        }

        // Forbid teleport attribute for output tables.
        if (GetTeleportTableIndex()) {
            THROW_ERROR_EXCEPTION("Teleport tables are not supported in join-reduce");
        }
    }

    virtual void AdjustKeyColumns() override
    {
        // NB: Base member is not called intentionally.

        LOG_INFO("Spec key columns are %v", Spec->JoinBy);
        SortKeyColumns = CheckInputTablesSorted(Spec->JoinBy);

        ReduceKeyColumnCount = SortKeyColumns.size();
        ForeignKeyColumnCount = SortKeyColumns.size();
    }

    virtual void SortEndpoints() override
    {
        Y_UNREACHABLE();
    }

    virtual void FindTeleportChunks() override
    {
        Y_UNREACHABLE();
    }

    virtual void BuildTasks() override
    {
        TPeriodicYielder yielder(PrepareYieldPeriod);

        auto processSlice = [&] (const TInputDataSlicePtr& slice) {
            yielder.TryYield();

            try {
                ValidateClientKey(slice->LowerLimit().Key);
                ValidateClientKey(slice->UpperLimit().Key);
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION(
                    "Error validating sample key in input table %v",
                    GetInputTablePaths()[slice->GetTableIndex()])
                    << ex;
            }

            AddPendingDataSlice(slice);

            EndTaskIfLarge();
        };

        for (const auto& chunkSlice : ChunkSliceFetcher->GetChunkSlices()) {
            processSlice(CreateUnversionedInputDataSlice(chunkSlice));
        }

        for (const auto& dataSlice : VersionedDataSlices) {
            processSlice(dataSlice);
        }

        EndTaskIfActive();
    }

    virtual bool ShouldSlicePrimaryTableByKeys() override
    {
        // JoinReduce slices by row indexes.
        return false;
    }

    virtual bool AreForeignTablesSupported() const override
    {
        return true;
    }

    virtual EJobType GetJobType() const override
    {
        return EJobType::JoinReduce;
    }
};

DEFINE_DYNAMIC_PHOENIX_TYPE(TLegacyJoinReduceController);

IOperationControllerPtr CreateLegacyJoinReduceController(
    TSchedulerConfigPtr config,
    IOperationHost* host,
    TOperation* operation)
{
    auto spec = ParseOperationSpec<TJoinReduceOperationSpec>(operation->GetSpec());
    return New<TLegacyJoinReduceController>(config, spec, host, operation);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
