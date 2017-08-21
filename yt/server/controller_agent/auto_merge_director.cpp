#include "auto_merge_director.h"

namespace NYT {
namespace NControllerAgent {

using namespace NLogging;

////////////////////////////////////////////////////////////////////////////////

TLogger AutoMergeLogger("AutoMerge");

////////////////////////////////////////////////////////////////////////////////

TAutoMergeDirector::TAutoMergeDirector(
    int maxIntermediateChunkCount,
    int maxChunkCountPerMergeJob,
    TOperationId operationId)
    : MaxIntermediateChunkCount_(maxIntermediateChunkCount)
    , ChunkCountPerMergeJob_(maxChunkCountPerMergeJob)
    , OperationId_(operationId)
    , Logger(AutoMergeLogger)
{
    Logger.AddTag("OperationId: %v", OperationId_);
}

bool TAutoMergeDirector::TryScheduleTaskJob(int intermediateChunkCount)
{
    if (intermediateChunkCount + CurrentIntermediateChunkCount_ <= MaxIntermediateChunkCount_) {
        LOG_DEBUG("Allowing scheduling of task job "
            "(IntermediateChunkCountEstimate: %v, CurrentIntermediateChunkCount: %v, MaxIntermediateChunkCount: %v)",
            intermediateChunkCount,
            CurrentIntermediateChunkCount_,
            MaxIntermediateChunkCount_);
        return true;
    } else {
        // First, check for marginal case. If the job produces more than `MaxIntermediateChunkCount_`
        // chunks itself, there is no way we can stay under limit, so just let it go.
        if (intermediateChunkCount > MaxIntermediateChunkCount_) {
            LOG_DEBUG("Allowing scheduling of a marginally large task job "
                "(IntermediateChunkCountEstimate: %v, MaxIntermediateChunkCount: %v",
                intermediateChunkCount,
                MaxIntermediateChunkCount_);
            return true;
        }

        LOG_DEBUG("Disallowing scheduling of a task job "
            "(IntermediateChunkCountEstimate: %v, CurrentIntermediateChunkCount: %v, MaxIntermediateChunkCount: %v, "
            "RunningMergeJobCount: %v",
            intermediateChunkCount,
            CurrentIntermediateChunkCount_,
            MaxIntermediateChunkCount_,
            RunningMergeJobCount_);

        // If there are already some auto-merge jobs running, we should just wait for them.
        // Otherwise, we enable force-flush mode.
        if (RunningMergeJobCount_ == 0 && RunningTaskJobCount_ == 0 && !ForceFlush_) {
            LOG_DEBUG("Force flush mode enabled");
            ForceFlush_ = true;
            StateChanged_.Fire();
        }
        return false;
    }
}

bool TAutoMergeDirector::TryScheduleMergeJob(int intermediateChunkCount) const
{
    if (intermediateChunkCount >= ChunkCountPerMergeJob_ || ForceFlush_ || TaskCompleted_) {
        LOG_DEBUG("Allowing scheduling of a merge job "
            "(IntermediateChunkCount: %v, ChunkCountPerMergeJob: %v, ForceFlush: %v, TaskCompleted: %v",
            intermediateChunkCount,
            ChunkCountPerMergeJob_,
            ForceFlush_,
            TaskCompleted_);
        return true;
    } else {
        LOG_DEBUG("Disallowing scheduling of a merge job "
            "(IntermediateChunkCount: %v, ChunkCountPerMergeJob: %v, ForceFlush: %v, TaskCompleted: %v",
            intermediateChunkCount,
            ChunkCountPerMergeJob_,
            ForceFlush_,
            TaskCompleted_);
        return false;
    }
}

void TAutoMergeDirector::OnTaskJobStarted(int intermediateChunkCountEstimate)
{
    ++RunningTaskJobCount_;
    CurrentIntermediateChunkCount_ += intermediateChunkCountEstimate;
    StateChanged_.Fire();
}

void TAutoMergeDirector::OnTaskJobFinished(int intermediateChunkCountEstimate)
{
    --RunningTaskJobCount_;
    CurrentIntermediateChunkCount_ -= intermediateChunkCountEstimate;
    YCHECK(CurrentIntermediateChunkCount_ >= 0);
    StateChanged_.Fire();
}

void TAutoMergeDirector::OnMergeInputProcessed(int intermediateChunkCount)
{
    CurrentIntermediateChunkCount_ += intermediateChunkCount;
    StateChanged_.Fire();
}

void TAutoMergeDirector::OnMergeJobStarted()
{
    ++RunningMergeJobCount_;

    if (ForceFlush_) {
        LOG_DEBUG("Force flush mode disabled");
        ForceFlush_ = false;
    }

    StateChanged_.Fire();
}

void TAutoMergeDirector::OnMergeJobFinished(int unregisteredOutputChunkCount)
{
    --RunningMergeJobCount_;
    YCHECK(RunningMergeJobCount_ >= 0);
    CurrentIntermediateChunkCount_ -= unregisteredOutputChunkCount;
    YCHECK(CurrentIntermediateChunkCount_ >= 0);

    StateChanged_.Fire();
}

void TAutoMergeDirector::OnTaskCompleted()
{
    TaskCompleted_ = true;
    StateChanged_.Fire();
}

void TAutoMergeDirector::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, MaxIntermediateChunkCount_);
    Persist(context, ChunkCountPerMergeJob_);
    Persist(context, OperationId_);
    Persist(context, CurrentIntermediateChunkCount_);
    Persist(context, RunningMergeJobCount_);
    Persist(context, ForceFlush_);
    Persist(context, TaskCompleted_);
    Persist(context, RunningTaskJobCount_);

    if (context.IsLoad()) {
        Logger = AutoMergeLogger;
        Logger.AddTag("OperationId: %v", OperationId_);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT