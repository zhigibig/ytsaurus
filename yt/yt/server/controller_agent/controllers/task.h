#pragma once

#include "private.h"

#include "competitive_job_manager.h"
#include "data_flow_graph.h"
#include "job_splitter.h"

#include <yt/server/controller_agent/tentative_tree_eligibility.h>

#include <yt/server/lib/chunk_pools/chunk_stripe_key.h>
#include <yt/server/lib/chunk_pools/chunk_pool.h>
#include <yt/server/lib/chunk_pools/input_chunk_mapping.h>

#include <yt/server/lib/controller_agent/progress_counter.h>
#include <yt/server/lib/controller_agent/serialize.h>

#include <yt/ytlib/scheduler/job_resources.h>
#include <yt/ytlib/scheduler/public.h>

#include <yt/ytlib/table_client/helpers.h>

#include <yt/core/misc/digest.h>
#include <yt/core/misc/histogram.h>

namespace NYT::NControllerAgent::NControllers {

////////////////////////////////////////////////////////////////////////////////

class TTask
    : public TRefCounted
    , public IPersistent
{
public:
    DEFINE_BYVAL_RW_PROPERTY(std::optional<TInstant>, DelayedTime);
    DEFINE_BYVAL_RW_PROPERTY(TDataFlowGraph::TVertexDescriptor, InputVertex, TDataFlowGraph::TVertexDescriptor());

public:
    //! For persistence only.
    TTask();
    TTask(ITaskHostPtr taskHost, std::vector<TStreamDescriptor> streamDescriptors);
    explicit TTask(ITaskHostPtr taskHost);

    //! This method is called on task object creation (both at clean creation and at revival).
    //! It may be used when calling virtual method is needed, but not allowed.
    virtual void Initialize();

    //! This method is called on task object creation (at clean creation only).
    //! It may be used when calling virtual method is needed, but not allowed.
    virtual void Prepare();

    //! Title of a data flow graph vertex that appears in a web interface and coincides with the job type
    //! for builtin tasks. For example, "SortedReduce" or "PartitionMap".
    virtual TDataFlowGraph::TVertexDescriptor GetVertexDescriptor() const;
    //! Human-readable title of a particular task that appears in logging. For builtin tasks it coincides
    //! with the vertex descriptor and a partition index in brackets (if applicable).
    virtual TString GetTitle() const;

    //! Human-readable name of a particular task that appears in archive. Supported for vanilla tasks only for now.
    virtual TString GetName() const;

    virtual int GetPendingJobCount() const;
    int GetPendingJobCountDelta();

    virtual int GetTotalJobCount() const;
    int GetTotalJobCountDelta();

    const TProgressCounterPtr& GetJobCounter() const;

    virtual TJobResources GetTotalNeededResources() const;
    TJobResources GetTotalNeededResourcesDelta();

    bool IsStderrTableEnabled() const;

    bool IsCoreTableEnabled() const;

    virtual TDuration GetLocalityTimeout() const;
    virtual i64 GetLocality(NNodeTrackerClient::TNodeId nodeId) const;
    virtual bool HasInputLocality() const;

    NScheduler::TJobResourcesWithQuota GetMinNeededResources() const;

    void ResetCachedMinNeededResources();

    void AddInput(NChunkPools::TChunkStripePtr stripe);
    void AddInput(const std::vector<NChunkPools::TChunkStripePtr>& stripes);

    virtual void FinishInput();

    void UpdateTask();

    // NB: This works well until there is no more than one input data flow vertex for any task.
    void RegisterInGraph();
    void RegisterInGraph(TDataFlowGraph::TVertexDescriptor inputVertex);

    void CheckCompleted();
    void ForceComplete();

    virtual bool ValidateChunkCount(int chunkCount);

    void ScheduleJob(
        ISchedulingContext* context,
        const NScheduler::TJobResourcesWithQuota& jobLimits,
        const TString& treeId,
        bool treeIsTentative,
        NScheduler::TControllerScheduleJobResult* scheduleJobResult);

    bool TryRegisterSpeculativeJob(const TJobletPtr& joblet);
    std::optional<EAbortReason> ShouldAbortJob(const TJobletPtr& joblet);

    virtual TJobFinishedResult OnJobCompleted(TJobletPtr joblet, TCompletedJobSummary& jobSummary);
    virtual TJobFinishedResult OnJobFailed(TJobletPtr joblet, const TFailedJobSummary& jobSummary);
    virtual TJobFinishedResult OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& jobSummary);
    virtual void OnJobRunning(TJobletPtr joblet, const TRunningJobSummary& jobSummary);
    virtual void OnJobLost(TCompletedJobPtr completedJob);

    virtual void OnStripeRegistrationFailed(
        TError error,
        NChunkPools::IChunkPoolInput::TCookie cookie,
        const NChunkPools::TChunkStripePtr& stripe,
        const TStreamDescriptor& streamDescriptor);

    // First checks against a given node, then against all nodes if needed.
    void CheckResourceDemandSanity(
        const NScheduler::TJobResourcesWithQuota& nodeResourceLimits,
        const NScheduler::TJobResourcesWithQuota& neededResources);

    void DoCheckResourceDemandSanity(const NScheduler::TJobResourcesWithQuota& neededResources);

    virtual bool IsCompleted() const;

    virtual bool IsActive() const;

    i64 GetTotalDataWeight() const;
    i64 GetCompletedDataWeight() const;
    i64 GetPendingDataWeight() const;

    i64 GetInputDataSliceCount() const;

    std::vector<std::optional<i64>> GetMaximumUsedTmpfsSizes() const;

    virtual void Persist(const TPersistenceContext& context) override;

    virtual NScheduler::TUserJobSpecPtr GetUserJobSpec() const;
    bool HasUserJob() const;

    // TODO(max42): eliminate necessity for this method (YT-10528).
    virtual bool IsSimpleTask() const;

    ITaskHost* GetTaskHost();

    IDigest* GetUserJobMemoryDigest() const;
    IDigest* GetJobProxyMemoryDigest() const;

    virtual void SetupCallbacks();

    virtual NScheduler::TExtendedJobResources GetNeededResources(const TJobletPtr& joblet) const = 0;

    virtual NChunkPools::IChunkPoolInputPtr GetChunkPoolInput() const = 0;
    virtual NChunkPools::IChunkPoolOutputPtr GetChunkPoolOutput() const = 0;

    virtual EJobType GetJobType() const = 0;

    //! Return a chunk mapping that is used to substitute input chunks when job spec is built.
    //! Base implementation returns task's own mapping.
    virtual NChunkPools::TInputChunkMappingPtr GetChunkMapping() const;

    std::vector<TString> FindAndBanSlowTentativeTrees();

    void LogTentativeTreeStatistics() const;

    TSharedRef BuildJobSpecProto(TJobletPtr joblet);

    virtual bool IsJobInterruptible() const = 0;

    void BuildTaskYson(NYTree::TFluentMap fluent) const;

    virtual void PropagatePartitions(
        const std::vector<TStreamDescriptor>& streamDescriptors,
        const NChunkPools::TChunkStripeListPtr& inputStripeList,
        std::vector<NChunkPools::TChunkStripePtr>* outputStripes);

    virtual NChunkPools::IChunkPoolOutput::TCookie ExtractCookie(NNodeTrackerClient::TNodeId nodeId);

protected:
    NLogging::TLogger Logger;

    //! Raw pointer here avoids cyclic reference; task cannot live longer than its host.
    ITaskHost* TaskHost_;

    //! Outgoing data stream descriptors.
    std::vector<TStreamDescriptor> StreamDescriptors_;

    //! Increments each time a new job in this task is scheduled.
    TIdGenerator TaskJobIndexGenerator_;

    TTentativeTreeEligibility TentativeTreeEligibility_;

    mutable std::unique_ptr<IDigest> JobProxyMemoryDigest_;
    mutable std::unique_ptr<IDigest> UserJobMemoryDigest_;

    std::unique_ptr<IJobSplitter> JobSplitter_;

    virtual std::optional<EScheduleJobFailReason> GetScheduleFailReason(ISchedulingContext* context);

    virtual void OnTaskCompleted();

    virtual void OnJobStarted(TJobletPtr joblet);

    //! True if task supports lost jobs.
    virtual bool CanLoseJobs() const;

    virtual void OnChunkTeleported(NChunkClient::TInputChunkPtr chunk, std::any tag);

    void ReinstallJob(std::function<void()> releaseOutputCookie);

    void ReleaseJobletResources(TJobletPtr joblet, bool waitForSnapshot);

    std::unique_ptr<NNodeTrackerClient::TNodeDirectoryBuilder> MakeNodeDirectoryBuilder(
        NScheduler::NProto::TSchedulerJobSpecExt* schedulerJobSpec);
    void AddSequentialInputSpec(
        NJobTrackerClient::NProto::TJobSpec* jobSpec,
        TJobletPtr joblet);
    void AddParallelInputSpec(
        NJobTrackerClient::NProto::TJobSpec* jobSpec,
        TJobletPtr joblet);
    void AddChunksToInputSpec(
        NNodeTrackerClient::TNodeDirectoryBuilder* directoryBuilder,
        NScheduler::NProto::TTableInputSpec* inputSpec,
        NChunkPools::TChunkStripePtr stripe);

    void AddOutputTableSpecs(NJobTrackerClient::NProto::TJobSpec* jobSpec, TJobletPtr joblet);

    static void UpdateInputSpecTotals(
        NJobTrackerClient::NProto::TJobSpec* jobSpec,
        TJobletPtr joblet);

    // Send stripe to the next chunk pool.
    void RegisterStripe(
        NChunkPools::TChunkStripePtr chunkStripe,
        const TStreamDescriptor& streamDescriptor,
        TJobletPtr joblet,
        NChunkPools::TChunkStripeKey key = NChunkPools::TChunkStripeKey());

    static std::vector<NChunkPools::TChunkStripePtr> BuildChunkStripes(
        google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>* chunkSpecs,
        int tableCount);

    static NChunkPools::TChunkStripePtr BuildIntermediateChunkStripe(
        google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>* chunkSpecs);

    std::vector<NChunkPools::TChunkStripePtr> BuildOutputChunkStripes(
        NScheduler::NProto::TSchedulerJobResultExt* schedulerJobResultExt,
        const std::vector<NChunkClient::TChunkTreeId>& chunkTreeIds,
        google::protobuf::RepeatedPtrField<NScheduler::NProto::TOutputResult> boundaryKeys);

    void AddFootprintAndUserJobResources(NScheduler::TExtendedJobResources& jobResources) const;

    //! This method processes `chunkListIds`, forming the chunk stripes (maybe with boundary
    //! keys taken from `jobResult` if they are present) and sends them to the destination pools
    //! depending on the table index.
    //!
    //! If destination pool requires the recovery info, `joblet` should be non-null since it is used
    //! in the recovery info, otherwise it is not used.
    //!
    //! This method steals output chunk specs for `jobResult`.
    void RegisterOutput(
        NJobTrackerClient::NProto::TJobResult* jobResult,
        const std::vector<NChunkClient::TChunkListId>& chunkListIds,
        TJobletPtr joblet,
        const NChunkPools::TChunkStripeKey& key = NChunkPools::TChunkStripeKey());

    //! A convenience method for calling task->Finish() and
    //! task->SetInputVertex(this->GetJobType());
    void FinishTaskInput(const TTaskPtr& task);

    virtual NScheduler::TExtendedJobResources GetMinNeededResourcesHeavy() const = 0;
    virtual void BuildJobSpec(TJobletPtr joblet, NJobTrackerClient::NProto::TJobSpec* jobSpec) = 0;

    virtual void SetStreamDescriptors(TJobletPtr joblet) const;

    virtual bool IsInputDataWeightHistogramSupported() const;

    virtual TJobSplitterConfigPtr GetJobSplitterConfig() const = 0;

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TTask, 0x81ab3cd3);

    int CachedPendingJobCount_;
    int CachedTotalJobCount_;

    std::vector<std::optional<i64>> MaximumUsedTmpfsSizes_;

    TJobResources CachedTotalNeededResources_;
    mutable std::optional<NScheduler::TExtendedJobResources> CachedMinNeededResources_;

    bool CompletedFired_ = false;

    using TCookieAndPool = std::pair<NChunkPools::IChunkPoolInput::TCookie, NChunkPools::IChunkPoolInputPtr>;

    //! For each lost job currently being replayed and destination pool, maps output cookie to corresponding input cookie.
    std::map<TCookieAndPool, NChunkPools::IChunkPoolInput::TCookie> LostJobCookieMap;

    NChunkPools::TInputChunkMappingPtr InputChunkMapping_;

    TCompetitiveJobManager CompetitiveJobManager_;

    //! Time of first job scheduling.
    std::optional<TInstant> StartTime_;

    //! Time of task completion.
    std::optional<TInstant> CompletionTime_;

    //! Caches results of SerializeToWireProto serializations.
    // NB: This field is transient intentionally.
    THashMap<NTableClient::TTableSchemaPtr, TString> TableSchemaToProtobufTableSchema_;

    std::unique_ptr<IHistogram> EstimatedInputDataWeightHistogram_;
    std::unique_ptr<IHistogram> InputDataWeightHistogram_;

    NScheduler::TJobResources ApplyMemoryReserve(const NScheduler::TExtendedJobResources& jobResources) const;

    void UpdateMaximumUsedTmpfsSizes(const TStatistics& statistics);

    void AbortJobViaScheduler(TJobId jobId, EAbortReason reason);

    void OnSpeculativeJobScheduled(const TJobletPtr& joblet);

    double GetJobProxyMemoryReserveFactor() const;
    double GetUserJobMemoryReserveFactor() const;

    int EstimateSplitJobCount(const TCompletedJobSummary& jobSummary, const TJobletPtr& joblet);

    TString GetOrCacheSerializedSchema(const NTableClient::TTableSchemaPtr& schema);
};

DEFINE_REFCOUNTED_TYPE(TTask)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent::NControllers
