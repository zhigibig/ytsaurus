#include "stdafx.h"
#include "chunk_replicator.h"
#include "node_lease_tracker.h"
#include "chunk_placement.h"
#include "node.h"
#include "job.h"
#include "chunk.h"
#include "chunk_list.h"
#include "job_list.h"
#include "chunk_tree_traversing.h"
#include "private.h"

#include <ytlib/misc/foreach.h>
#include <ytlib/misc/serialize.h>
#include <ytlib/misc/string.h>
#include <ytlib/misc/small_vector.h>

#include <ytlib/profiling/profiler.h>

#include <server/cell_master/bootstrap.h>
#include <server/cell_master/config.h>
#include <server/cell_master/meta_state_facade.h>

#include <server/chunk_server/chunk_manager.h>

namespace NYT {
namespace NChunkServer {

using namespace NCellMaster;
using namespace NObjectClient;
using namespace NProfiling;
using namespace NChunkServer::NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkServerLogger;
static NProfiling::TProfiler& Profiler = ChunkServerProfiler;

////////////////////////////////////////////////////////////////////////////////

TChunkReplicator::TChunkReplicator(
    TChunkManagerConfigPtr config,
    TBootstrap* bootstrap,
    TChunkPlacementPtr chunkPlacement,
    TNodeLeaseTrackerPtr nodeLeaseTracker)
    : Config(config)
    , Bootstrap(bootstrap)
    , ChunkPlacement(chunkPlacement)
    , NodeLeaseTracker(nodeLeaseTracker)
    , ChunkRefreshDelay(DurationToCpuDuration(config->ChunkRefreshDelay))
{
    YCHECK(config);
    YCHECK(bootstrap);
    YCHECK(chunkPlacement);
    YCHECK(nodeLeaseTracker);

    RefreshInvoker = New<TPeriodicInvoker>(
        Bootstrap->GetMetaStateFacade()->GetEpochInvoker(EStateThreadQueue::ChunkMaintenance),
        BIND(&TChunkReplicator::OnRefresh, MakeWeak(this)),
        Config->ChunkRefreshPeriod);
    RefreshInvoker->Start();

    RFUpdateInvoker = New<TPeriodicInvoker>(
        Bootstrap->GetMetaStateFacade()->GetEpochInvoker(EStateThreadQueue::ChunkMaintenance),
        BIND(&TChunkReplicator::OnRFUpdate, MakeWeak(this)),
        Config->ChunkRFUpdatePeriod);
    RFUpdateInvoker->Start();
}

void TChunkReplicator::ScheduleJobs(
    TDataNode* node,
    const std::vector<TJobInfo>& runningJobs,
    std::vector<TJobStartInfo>* jobsToStart,
    std::vector<TJobStopInfo>* jobsToStop)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    int replicationJobCount;
    int removalJobCount;
    ProcessExistingJobs(
        node,
        runningJobs,
        jobsToStop,
        &replicationJobCount,
        &removalJobCount);

    if (IsEnabled()) {
        ScheduleNewJobs(
            node,
            std::max(0, Config->ChunkReplicator->MaxReplicationFanOut - replicationJobCount),
            std::max(0, Config->ChunkReplicator->MaxRemovalJobsPerNode - removalJobCount),
            jobsToStart);
    }
}

void TChunkReplicator::OnNodeRegistered(TDataNode* node)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    node->ChunksToRemove().clear();

    FOREACH (auto& chunksToReplicate, node->ChunksToReplicate()) {
        chunksToReplicate.clear();
    }

    FOREACH (auto* chunk, node->StoredChunks()) {
        ScheduleChunkRefresh(chunk);
    }
}

void TChunkReplicator::OnNodeUnregistered(TDataNode* node)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    UNUSED(node);
}

void TChunkReplicator::OnChunkRemoved(TChunk* chunk)
{
    LostChunks_.erase(chunk);
    LostVitalChunks_.erase(chunk);
    UnderreplicatedChunks_.erase(chunk);
    OverreplicatedChunks_.erase(chunk);
}

void TChunkReplicator::ScheduleChunkRemoval(TDataNode* node, const TChunkId& chunkId)
{
    node->ChunksToRemove().insert(chunkId);
    FOREACH (auto& chunksToReplicate, node->ChunksToReplicate()) {
        chunksToReplicate.erase(chunkId);
    }
}

void TChunkReplicator::ScheduleChunkRemoval(TDataNode* node, TChunk* chunk)
{
    ScheduleChunkRemoval(node, chunk->GetId());
}

void TChunkReplicator::ProcessExistingJobs(
    TDataNode* node,
    const std::vector<TJobInfo>& runningJobs,
    std::vector<TJobStopInfo>* jobsToStop,
    int* replicationJobCount,
    int* removalJobCount)
{
    using ::ToString;

    *replicationJobCount = 0;
    *removalJobCount = 0;

    yhash_set<TJobId> runningJobIds;

    auto chunkManager = Bootstrap->GetChunkManager();
    FOREACH (const auto& jobInfo, runningJobs) {
        auto jobId = TJobId::FromProto(jobInfo.job_id());
        runningJobIds.insert(jobId);
        const auto* job = chunkManager->FindJob(jobId);

        if (!job) {
            LOG_WARNING("Stopping unknown or obsolete job (JobId: %s, Address: %s)",
                ~jobId.ToString(),
                ~node->GetAddress());
            TJobStopInfo stopInfo;
            *stopInfo.mutable_job_id() = jobId.ToProto();
            jobsToStop->push_back(stopInfo);
            continue;
        }

        auto* chunk = chunkManager->FindChunk(job->GetChunkId());

        auto jobState = EJobState(jobInfo.state());
        switch (jobState) {
            case EJobState::Running:
                switch (job->GetType()) {
                    case EJobType::Replicate:
                        ++*replicationJobCount;
                        break;

                    case EJobType::Remove:
                        ++*removalJobCount;
                        break;

                    default:
                        YUNREACHABLE();
                }

                LOG_INFO("Job is running (JobId: %s, Address: %s)",
                    ~jobId.ToString(),
                    ~node->GetAddress());

                if (TInstant::Now() - job->GetStartTime() > Config->ChunkReplicator->JobTimeout) {
                    TJobStopInfo stopInfo;
                    *stopInfo.mutable_job_id() = jobId.ToProto();
                    jobsToStop->push_back(stopInfo);

                    LOG_WARNING("Job timed out (JobId: %s, Address: %s, Duration: %s)",
                        ~jobId.ToString(),
                        ~node->GetAddress(),
                        ~ToString(TInstant::Now() - job->GetStartTime()));
                }
                break;

            case EJobState::Completed:
            case EJobState::Failed: {
                TJobStopInfo stopInfo;
                *stopInfo.mutable_job_id() = jobId.ToProto();
                jobsToStop->push_back(stopInfo);

                if (chunk) {
                    ScheduleChunkRefresh(chunk);
                }

                switch (jobState) {
                    case EJobState::Completed:
                        LOG_INFO("Job completed (JobId: %s, Address: %s)",
                            ~jobId.ToString(),
                            ~node->GetAddress());
                        break;

                    case EJobState::Failed:
                        LOG_WARNING(
                            FromProto(jobInfo.error()),
                            "Job failed (JobId: %s, Address: %s)",
                            ~jobId.ToString(),
                            ~node->GetAddress());
                        break;

                    default:
                        YUNREACHABLE();
                }
                break;
            }

            default:
                YUNREACHABLE();
        }
    }

    // Check for missing jobs
    FOREACH (auto* job, node->Jobs()) {
        auto jobId = job->GetId();
        if (runningJobIds.find(jobId) == runningJobIds.end()) {
            TJobStopInfo stopInfo;
            *stopInfo.mutable_job_id() = jobId.ToProto();
            jobsToStop->push_back(stopInfo);

            LOG_WARNING("Job is missing (JobId: %s, Address: %s)",
                ~jobId.ToString(),
                ~node->GetAddress());
        }
    }
}

TChunkReplicator::EScheduleFlags TChunkReplicator::ScheduleReplicationJob(
    TDataNode* sourceNode,
    const TChunkId& chunkId,
    std::vector<TJobStartInfo>* jobsToStart)
{
    auto chunkManager = Bootstrap->GetChunkManager();
    auto* chunk = chunkManager->FindChunk(chunkId);
    if (!IsObjectAlive(chunk)) {
        return EScheduleFlags::Purged;
    }

    if (chunk->GetRefreshScheduled()) {
        LOG_TRACE("Chunk %s we're about to replicate is scheduled for another refresh",
            ~chunkId.ToString());
        return EScheduleFlags::Purged;
    }

    auto statistics = GetReplicaStatistics(*chunk);

    int replicasNeeded = statistics.ReplicationFactor - (statistics.StoredCount + statistics.PlusCount);
    if (replicasNeeded <= 0) {
        LOG_TRACE("Chunk %s we're about to replicate has enough replicas",
            ~chunkId.ToString());
        return EScheduleFlags::Purged;
    }

    auto targets = ChunkPlacement->GetReplicationTargets(chunk, replicasNeeded);
    if (targets.empty()) {
        LOG_TRACE("No suitable target nodes to replicate chunk %s",
            ~chunkId.ToString());
        return EScheduleFlags::None;
    }

    std::vector<Stroka> targetAddresses;
    FOREACH (auto* node, targets) {
        targetAddresses.push_back(node->GetAddress());
        ChunkPlacement->OnSessionHinted(node);
    }

    auto jobId = TJobId::Create();
    TJobStartInfo startInfo;
    *startInfo.mutable_job_id() = jobId.ToProto();
    startInfo.set_type(EJobType::Replicate);
    *startInfo.mutable_chunk_id() = chunkId.ToProto();
    ToProto(startInfo.mutable_target_addresses(), targetAddresses);
    jobsToStart->push_back(startInfo);

    LOG_DEBUG("Job %s is scheduled on %s: replicate chunk %s to [%s]",
        ~jobId.ToString(),
        ~sourceNode->GetAddress(),
        ~chunkId.ToString(),
        ~JoinToString(targetAddresses));

    return
        targetAddresses.size() == replicasNeeded
        ? EScheduleFlags(EScheduleFlags::Purged | EScheduleFlags::Scheduled)
        : EScheduleFlags(EScheduleFlags::Scheduled);
}

TChunkReplicator::EScheduleFlags TChunkReplicator::ScheduleBalancingJob(
    TDataNode* sourceNode,
    TChunk* chunk,
    double maxFillCoeff,
    std::vector<TJobStartInfo>* jobsToStart)
{
    auto chunkId = chunk->GetId();

    if (chunk->GetRefreshScheduled()) {
        LOG_TRACE("Chunk %s we're about to balance is scheduled for another refresh",
            ~chunkId.ToString());
        return EScheduleFlags::None;
    }

    auto* targetNode = ChunkPlacement->GetBalancingTarget(chunk, maxFillCoeff);
    if (!targetNode) {
        LOG_DEBUG("No suitable target nodes to balance chunk %s",
            ~chunkId.ToString());
        return EScheduleFlags::None;
    }

    ChunkPlacement->OnSessionHinted(targetNode);

    auto jobId = TJobId::Create();
    TJobStartInfo startInfo;
    *startInfo.mutable_job_id() = jobId.ToProto();
    startInfo.set_type(EJobType::Replicate);
    *startInfo.mutable_chunk_id() = chunkId.ToProto();
    startInfo.add_target_addresses(targetNode->GetAddress());
    jobsToStart->push_back(startInfo);

    LOG_DEBUG("Job %s is scheduled on %s: balance chunk %s to [%s]",
        ~jobId.ToString(),
        ~sourceNode->GetAddress(),
        ~chunkId.ToString(),
        ~targetNode->GetAddress());

    return EScheduleFlags(EScheduleFlags::Purged | EScheduleFlags::Scheduled);
}

TChunkReplicator::EScheduleFlags TChunkReplicator::ScheduleRemovalJob(
    TDataNode* node,
    const TChunkId& chunkId,
    std::vector<TJobStartInfo>* jobsToStart)
{
    auto chunkManager = Bootstrap->GetChunkManager();
    auto* chunk = chunkManager->FindChunk(chunkId);
    if (chunk && chunk->GetRefreshScheduled()) {
        LOG_TRACE("Chunk %s we're about to remove is scheduled for another refresh",
            ~chunkId.ToString());
        return EScheduleFlags::None;
    }

    auto jobId = TJobId::Create();

    TJobStartInfo startInfo;
    *startInfo.mutable_job_id() = jobId.ToProto();
    startInfo.set_type(EJobType::Remove);
    *startInfo.mutable_chunk_id() = chunkId.ToProto();
    jobsToStart->push_back(startInfo);

    LOG_DEBUG("Job %s is scheduled on %s: chunk %s will be removed",
        ~jobId.ToString(),
        ~node->GetAddress(),
        ~chunkId.ToString());

    return EScheduleFlags(EScheduleFlags::Purged | EScheduleFlags::Scheduled);
}

void TChunkReplicator::ScheduleNewJobs(
    TDataNode* node,
    int maxReplicationJobsToStart,
    int maxRemovalJobsToStart,
    std::vector<TJobStartInfo>* jobsToStart)
{
    // Schedule replication jobs.
    if (maxReplicationJobsToStart > 0) {
        FOREACH (auto& chunksToReplicate, node->ChunksToReplicate()) {
            auto it = chunksToReplicate.begin();
            while (it != chunksToReplicate.end()) {
                auto jt = it++;
                const auto& chunkId = *jt;
                if (maxReplicationJobsToStart == 0) {
                    break;
                }
                auto flags = ScheduleReplicationJob(node, chunkId, jobsToStart);
                if (flags & EScheduleFlags::Scheduled) {
                    --maxReplicationJobsToStart;
                }
                if (flags & EScheduleFlags::Purged) {
                    chunksToReplicate.erase(jt);
                }
            }
        }
    }

    // Schedule balancing jobs.
    double sourceFillCoeff = ChunkPlacement->GetFillCoeff(node);
    double targetFillCoeff = sourceFillCoeff - Config->ChunkReplicator->MinBalancingFillCoeffDiff;
    if (maxReplicationJobsToStart > 0 &&
        sourceFillCoeff > Config->ChunkReplicator->MinBalancingFillCoeff &&
        ChunkPlacement->HasBalancingTargets(targetFillCoeff))
    {
        auto chunksToBalance = ChunkPlacement->GetBalancingChunks(node, maxReplicationJobsToStart);
        FOREACH (auto* chunk, chunksToBalance) {
            if (maxReplicationJobsToStart == 0) {
                break;
            }
            auto flags = ScheduleBalancingJob(node, chunk, targetFillCoeff, jobsToStart);
            if (flags & EScheduleFlags::Scheduled) {
                --maxReplicationJobsToStart;
            }
        }
    }

    // Schedule removal jobs.
    if (maxRemovalJobsToStart > 0) {
        auto& chunksToRemove = node->ChunksToRemove();
        auto it = chunksToRemove.begin();
        while (it != chunksToRemove.end()) {
            auto jt = it++;
            const auto& chunkId = *jt;
            if (maxRemovalJobsToStart == 0) {
                break;
            }
            auto flags = ScheduleRemovalJob(node, chunkId, jobsToStart);
            if (flags & EScheduleFlags::Scheduled) {
                --maxRemovalJobsToStart;
            }
            if (flags & EScheduleFlags::Purged) {
                chunksToRemove.erase(jt);
            }
        }
    }
}

TChunkReplicator::TReplicaStatistics TChunkReplicator::GetReplicaStatistics(const TChunk& chunk)
{
    TReplicaStatistics result;

    result.ReplicationFactor = chunk.GetReplicationFactor();
    result.StoredCount = static_cast<int>(chunk.StoredReplicas().size());
    result.CachedCount = ~chunk.CachedReplicas() ? static_cast<int>(chunk.CachedReplicas()->size()) : 0;
    result.PlusCount = 0;
    result.MinusCount = 0;

    if (result.StoredCount == 0) {
        return result;
    }

    auto chunkManager = Bootstrap->GetChunkManager();
    const auto* jobList = chunkManager->FindJobList(chunk.GetId());
    if (!jobList) {
        return result;
    }

    TSmallSet<Stroka, TypicalReplicationFactor> storedAddresses;
    FOREACH (auto replica, chunk.StoredReplicas()) {
        storedAddresses.insert(replica.GetNode()->GetAddress());
    }

    FOREACH (auto* job, jobList->Jobs()) {
        switch (job->GetType()) {
            case EJobType::Replicate: {
                FOREACH (const auto& address, job->TargetAddresses()) {
                    if (!storedAddresses.count(address)) {
                        ++result.PlusCount;
                    }
                }
                break;
            }

            case EJobType::Remove:
                if (storedAddresses.count(job->GetAddress())) {
                    ++result.MinusCount;
                }
                break;

            default:
                YUNREACHABLE();
        }
    }

    return result;
}

Stroka TChunkReplicator::ToString(const TReplicaStatistics& statistics)
{
    return Sprintf("%d+%d+%d-%d",
        statistics.StoredCount,
        statistics.CachedCount,
        statistics.PlusCount,
        statistics.MinusCount);
}

void TChunkReplicator::Refresh(TChunk* chunk)
{
    if (!chunk->IsConfirmed())
        return;

    const auto& chunkId = chunk->GetId();
    auto chunkManager = Bootstrap->GetChunkManager();
    FOREACH (auto replica, chunk->StoredReplicas()) {
        auto* node = replica.GetNode();
        if (node) {
            FOREACH (auto& chunksToReplicate, node->ChunksToReplicate()) {
                chunksToReplicate.erase(chunkId);
            }
            node->ChunksToRemove().erase(chunkId);
        }
    }

    LostChunks_.erase(chunk);
    LostVitalChunks_.erase(chunk);
    OverreplicatedChunks_.erase(chunk);
    UnderreplicatedChunks_.erase(chunk);

    auto statistics = GetReplicaStatistics(*chunk);
    if (statistics.StoredCount == 0) {
        LostChunks_.insert(chunk);

        if (chunk->GetVital()) {
            LostVitalChunks_.insert(chunk);
        }

        LOG_TRACE("Chunk %s is lost: %d replicas needed but only %s exist",
            ~chunkId.ToString(),
            statistics.ReplicationFactor,
            ~ToString(statistics));
    } else if (statistics.StoredCount - statistics.MinusCount > statistics.ReplicationFactor) {
        OverreplicatedChunks_.insert(chunk);

        // NB: Never start removal jobs if new replicas are on the way, hence the check plusCount > 0.
        if (statistics.PlusCount > 0) {
            LOG_WARNING("Chunk %s is over-replicated: %s replicas exist but only %d needed, waiting for pending replications to complete",
                ~chunkId.ToString(),
                ~ToString(statistics),
                statistics.ReplicationFactor);
            return;
        }

        int redundantCount = statistics.StoredCount - statistics.MinusCount - statistics.ReplicationFactor;
        auto nodes = ChunkPlacement->GetRemovalTargets(chunk, redundantCount);
        FOREACH (auto* node, nodes) {
            node->ChunksToRemove().insert(chunkId);
        }

        std::vector<Stroka> addresses;
        FOREACH (auto node, nodes) {
            addresses.push_back(node->GetAddress());
        }

        LOG_DEBUG("Chunk %s is over-replicated: %s replicas exist but only %d needed, removal is scheduled on [%s]",
            ~chunkId.ToString(),
            ~ToString(statistics),
            statistics.ReplicationFactor,
            ~JoinToString(addresses));
    } else if (statistics.StoredCount + statistics.PlusCount < statistics.ReplicationFactor) {
        UnderreplicatedChunks_.insert(chunk);

        // NB: Never start replication jobs when removal jobs are in progress, hence the check minusCount > 0.
        if (statistics.MinusCount > 0) {
            LOG_DEBUG("Chunk %s is under-replicated: %s replicas exist but %d needed, waiting for pending removals to complete",
                ~chunkId.ToString(),
                ~ToString(statistics),
                statistics.ReplicationFactor);
            return;
        }

        auto* node = ChunkPlacement->GetReplicationSource(chunk);

        int priority = ComputeReplicationPriority(statistics);
        node->ChunksToReplicate()[priority].insert(chunkId);

        LOG_DEBUG("Chunk %s is under-replicated: %s replicas exist but %d needed, replication is scheduled on %s",
            ~chunkId.ToString(),
            ~ToString(statistics),
            statistics.ReplicationFactor,
            ~node->GetAddress());
    } else {
        LOG_TRACE("Chunk %s is OK: %s replicas exist and %d needed",
            ~chunkId.ToString(),
            ~ToString(statistics),
            statistics.ReplicationFactor);
    }
 }

int TChunkReplicator::ComputeReplicationPriority(const TReplicaStatistics& statistics)
{
    YASSERT(statistics.StoredCount > 0);
    return std::min(statistics.StoredCount, ReplicationPriorityCount) - 1;
}

void TChunkReplicator::ScheduleChunkRefresh(const TChunkId& chunkId)
{
    auto chunkManager = Bootstrap->GetChunkManager();
    auto* chunk = chunkManager->FindChunk(chunkId);
    if (IsObjectAlive(chunk)) {
        ScheduleChunkRefresh(chunk);
    }
}

void TChunkReplicator::ScheduleChunkRefresh(TChunk* chunk)
{
    if (!IsObjectAlive(chunk) || chunk->GetRefreshScheduled())
        return;

    TRefreshEntry entry;
    entry.Chunk = chunk;
    entry.When = GetCpuInstant() + ChunkRefreshDelay;
    RefreshList.push_back(entry);
    chunk->SetRefreshScheduled(true);

    auto objectManager = Bootstrap->GetObjectManager();
    objectManager->LockObject(chunk);
}

void TChunkReplicator::OnRefresh()
{
    VERIFY_THREAD_AFFINITY(StateThread);

    if (RefreshList.empty()) {
        RefreshInvoker->ScheduleNext();
        return;
    }

    auto objectManager = Bootstrap->GetObjectManager();

    int count = 0;
    PROFILE_TIMING ("/incremental_refresh_time") {
        auto chunkManager = Bootstrap->GetChunkManager();
        auto now = GetCpuInstant();
        for (int i = 0; i < Config->MaxChunksPerRefresh; ++i) {
            if (RefreshList.empty())
                break;

            const auto& entry = RefreshList.front();
            if (entry.When > now)
                break;

            auto* chunk = entry.Chunk;
            RefreshList.pop_front();
            chunk->SetRefreshScheduled(false);
            ++count;

            if (IsObjectAlive(chunk)) {
                Refresh(chunk);
            }

            objectManager->UnlockObject(chunk);        
        }
    }

    LOG_DEBUG("Incremental chunk refresh completed, %d chunks processed",
        count);

    RefreshInvoker->ScheduleNext();
}

bool TChunkReplicator::IsEnabled()
{
    // This method also logs state changes.

    auto config = Config->ChunkReplicator;
    if (config->MinOnlineNodeCount) {
        int needOnline = config->MinOnlineNodeCount.Get();
        int gotOnline = NodeLeaseTracker->GetOnlineNodeCount();
        if (gotOnline < needOnline) {
            if (!LastEnabled || LastEnabled.Get()) {
                LOG_INFO("Chunk replicator disabled: too few online nodes, needed >= %d but got %d",
                    needOnline,
                    gotOnline);
                LastEnabled = false;
            }
            return false;
        }
    }

    auto chunkManager = Bootstrap->GetChunkManager();
    int chunkCount = chunkManager->GetChunkCount();
    int lostChunkCount = chunkManager->LostChunks().size();
    if (config->MaxLostChunkFraction && chunkCount > 0) {
        double needFraction = config->MaxLostChunkFraction.Get();
        double gotFraction = (double) lostChunkCount / chunkCount;
        if (gotFraction > needFraction) {
            if (!LastEnabled || LastEnabled.Get()) {
                LOG_INFO("Chunk replicator disabled: too many lost chunks, needed <= %lf but got %lf",
                    needFraction,
                    gotFraction);
                LastEnabled = false;
            }
            return false;
        }
    }

    if (!LastEnabled || !LastEnabled.Get()) {
        LOG_INFO("Chunk replicator enabled");
        LastEnabled = true;
    }

    return true;
}

int TChunkReplicator::GetRefreshListSize() const
{
    return static_cast<int>(RefreshList.size());
}

int TChunkReplicator::GetRFUpdateListSize() const
{
    return static_cast<int>(RFUpdateList.size());
}

void TChunkReplicator::ScheduleRFUpdate(TChunkTree* chunkTree)
{
    switch (chunkTree->GetType()) {
        case EObjectType::Chunk:
            ScheduleRFUpdate(chunkTree->AsChunk());
            break;
        case EObjectType::ChunkList:
            ScheduleRFUpdate(chunkTree->AsChunkList());
            break;
        default:
            YUNREACHABLE();
    }
}

void TChunkReplicator::ScheduleRFUpdate(TChunkList* chunkList)
{
    VERIFY_THREAD_AFFINITY(StateThread);

    class TVisitor
        : public IChunkVisitor
    {
    public:
        TVisitor(
            NCellMaster::TBootstrap* bootstrap,
            TChunkReplicatorPtr replicator,
            TChunkList* root)
            : Bootstrap(bootstrap)
            , Replicator(std::move(replicator))
            , Root(root)
        { }

        void Run()
        {
            TraverseChunkTree(Bootstrap, this, Root);
        }

    private:
        TBootstrap* Bootstrap;
        TChunkReplicatorPtr Replicator;
        TChunkList* Root;

        virtual bool OnChunk(
            TChunk* chunk,
            const NTableClient::NProto::TReadLimit& startLimit,
            const NTableClient::NProto::TReadLimit& endLimit) override
        {
            UNUSED(startLimit);
            UNUSED(endLimit);

            Replicator->ScheduleRFUpdate(chunk);
            return true;
        }

        virtual void OnError(const TError& error) override
        {
            LOG_ERROR(error, "Error traversing chunk tree for RF update");
        }

        virtual void OnFinish() override
        { }

    };

    New<TVisitor>(Bootstrap, this, chunkList)->Run();
}

void TChunkReplicator::ScheduleRFUpdate(TChunk* chunk)
{
    if (!IsObjectAlive(chunk) || chunk->GetRefreshScheduled())
        return;

    RFUpdateList.push_back(chunk);
    chunk->SetRFUpdateScheduled(true);

    auto objectManager = Bootstrap->GetObjectManager();
    objectManager->LockObject(chunk);
}

void TChunkReplicator::OnRFUpdate()
{
    if (RFUpdateList.empty() ||
        !Bootstrap->GetMetaStateFacade()->GetManager()->HasActiveQuorum())
    {
        RFUpdateInvoker->ScheduleNext();
        return;
    }

    // Extract up to GCObjectsPerMutation objects and post a mutation.
    auto chunkManager = Bootstrap->GetChunkManager();
    auto objectManager = Bootstrap->GetObjectManager();
    NProto::TMetaReqUpdateChunkReplicationFactor request;

    PROFILE_TIMING ("/rf_update_time") {
        for (int i = 0; i < Config->MaxChunksPerRFUpdate; ++i) {
            if (RFUpdateList.empty())
                break;

            auto* chunk = RFUpdateList.front();
            RFUpdateList.pop_front();
            chunk->SetRFUpdateScheduled(false);

            if (IsObjectAlive(chunk)) {
                int replicationFactor = ComputeReplicationFactor(*chunk);
                if (chunk->GetReplicationFactor() != replicationFactor) {
                    auto* update = request.add_updates();
                    *update->mutable_chunk_id() = chunk->GetId().ToProto();
                    update->set_replication_factor(replicationFactor);
                }
            }

            objectManager->UnlockObject(chunk);
        }
    }

    if (request.updates_size() > 0) {
        LOG_DEBUG("Starting RF update for %d chunks", request.updates_size());

        auto invoker = Bootstrap->GetMetaStateFacade()->GetEpochInvoker();
        chunkManager
            ->CreateUpdateChunkReplicationFactorMutation(request)
            ->OnSuccess(BIND(&TChunkReplicator::OnRFUpdateCommitSucceeded, MakeWeak(this)).Via(invoker))
            ->OnError(BIND(&TChunkReplicator::OnRFUpdateCommitFailed, MakeWeak(this)).Via(invoker))
            ->PostCommit();
    }
}

void TChunkReplicator::OnRFUpdateCommitSucceeded()
{
    LOG_DEBUG("RF update commit succeeded");

    RFUpdateInvoker->ScheduleOutOfBand();
    RFUpdateInvoker->ScheduleNext();
}

void TChunkReplicator::OnRFUpdateCommitFailed(const TError& error)
{
    LOG_WARNING(error, "RF update commit failed");

    RFUpdateInvoker->ScheduleNext();
}

int TChunkReplicator::ComputeReplicationFactor(const TChunk& chunk)
{
    int result = chunk.GetReplicationFactor();

    // Unique number used to distinguish already visited chunk lists.
    auto mark = TChunkList::GenerateVisitMark();

    // BFS queue. Try to avoid allocations.
    TSmallVector<TChunkList*, 64> queue;
    size_t frontIndex = 0;

    auto enqueue = [&] (TChunkList* chunkList) {
        if (chunkList->GetVisitMark() != mark) {
            chunkList->SetVisitMark(mark);
            queue.push_back(chunkList);
        }
    };

    // Put seeds into the queue.
    FOREACH (auto* parent, chunk.Parents()) {
        auto* adjustedParent = FollowParentLinks(parent);
        if (adjustedParent) {
            enqueue(adjustedParent);
        }
    }

    // The main BFS loop.
    while (frontIndex < queue.size()) {
        auto* chunkList = queue[frontIndex++];

        // Examine owners, if any.
        FOREACH (const auto* owningNode, chunkList->OwningNodes()) {
            result = std::max(result, owningNode->GetOwningReplicationFactor());
        }

        // Proceed to parents.
        FOREACH (auto* parent, chunkList->Parents()) {
            auto* adjustedParent = FollowParentLinks(parent);
            if (adjustedParent) {
                enqueue(adjustedParent);
            }
        }
    }

    return result;
}

TChunkList* TChunkReplicator::FollowParentLinks(TChunkList* chunkList)
{
    while (chunkList->OwningNodes().empty()) {
        const auto& parents = chunkList->Parents();
        size_t parentCount = parents.size();
        if (parentCount == 0) {
            return NULL;
        }
        if (parentCount > 1) {
            break;
        }
        chunkList = *parents.begin();
    }
    return chunkList;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
