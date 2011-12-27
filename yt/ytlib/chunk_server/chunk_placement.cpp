#include "stdafx.h"
#include "chunk_placement.h"

#include "../misc/foreach.h"

#include <util/random/random.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

using NChunkClient::TChunkId;
using NChunkHolder::EJobType;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkServerLogger;

////////////////////////////////////////////////////////////////////////////////

TChunkPlacement::TChunkPlacement(TChunkManager* chunkManager)
    : ChunkManager(chunkManager)
{
    YASSERT(chunkManager);
}

void TChunkPlacement::OnHolderRegistered(const THolder& holder)
{
    double loadFactor = GetLoadFactor(holder);
    auto it = LoadFactorMap.insert(MakePair(loadFactor, holder.GetId()));
    YVERIFY(IteratorMap.insert(MakePair(holder.GetId(), it)).Second());
    YVERIFY(HintedSessionsMap.insert(MakePair(holder.GetId(), 0)).Second());
}

void TChunkPlacement::OnHolderUnregistered(const THolder& holder)
{
    auto iteratorIt = IteratorMap.find(holder.GetId());
    YASSERT(iteratorIt != IteratorMap.end());
    auto preferenceIt = iteratorIt->Second();
    LoadFactorMap.erase(preferenceIt);
    IteratorMap.erase(iteratorIt);
    YVERIFY(HintedSessionsMap.erase(holder.GetId()) == 1);
}

void TChunkPlacement::OnHolderUpdated(const THolder& holder)
{
    OnHolderUnregistered(holder);
    OnHolderRegistered(holder);
}

void TChunkPlacement::OnSessionHinted(const THolder& holder)
{
    ++HintedSessionsMap[holder.GetId()];
}

yvector<THolderId> TChunkPlacement::GetUploadTargets(int count)
{
    return GetUploadTargets(count, yhash_set<Stroka>());
}

yvector<THolderId> TChunkPlacement::GetUploadTargets(int count, const yhash_set<Stroka>& forbiddenAddresses)
{
    // TODO: check replication fan-in in case this is a replication job
    yvector<const THolder*> holders;
    holders.reserve(LoadFactorMap.size());

    FOREACH(const auto& pair, LoadFactorMap) {
        const auto& holder = ChunkManager->GetHolder(pair.second);
        if (IsValidUploadTarget(holder) &&
            forbiddenAddresses.find(holder.GetAddress()) == forbiddenAddresses.end()) {
            holders.push_back(&holder);
        }
    }

    std::sort(holders.begin(), holders.end(),
        [&] (const THolder* lhs, const THolder* rhs) {
            return GetSessionCount(*lhs) < GetSessionCount(*rhs);
        });

    yvector<const THolder*> holdersSample;
    holdersSample.reserve(count);

    auto beginGroupIt = holders.begin();
    while (beginGroupIt != holders.end() && count > 0) {
        auto endGroupIt = beginGroupIt;
        int groupSize = 0;
        while (endGroupIt != holders.end() && GetSessionCount(*(*beginGroupIt)) == GetSessionCount(*(*endGroupIt))) {
            ++endGroupIt;
            ++groupSize;
        }

        int sampleCount = Min(count, groupSize);
        std::random_sample_n(
            beginGroupIt,
            endGroupIt,
            std::back_inserter(holdersSample),
            sampleCount);

        beginGroupIt = endGroupIt;
        count -= sampleCount;
    }

    yvector<THolderId> holderIdsSample(holdersSample.ysize());
    for (int i = 0; i < holdersSample.ysize(); ++i) {
        holderIdsSample[i] = holdersSample[i]->GetId();
    }

    return holderIdsSample;
}

yvector<THolderId> TChunkPlacement::GetReplicationTargets(const TChunk& chunk, int count)
{
    yhash_set<Stroka> forbiddenAddresses;

    FOREACH(auto holderId, chunk.StoredLocations()) {
        const auto& holder = ChunkManager->GetHolder(holderId);
        forbiddenAddresses.insert(holder.GetAddress());
    }

    const auto* jobList = ChunkManager->FindJobList(chunk.GetId());
    if (jobList) {
        FOREACH(const auto& jobId, jobList->JobIds()) {
            const auto& job = ChunkManager->GetJob(jobId);
            if (job.GetType() == EJobType::Replicate && job.GetChunkId() == chunk.GetId()) {
                forbiddenAddresses.insert(job.TargetAddresses().begin(), job.TargetAddresses().end());
            }
        }
    }

    return GetUploadTargets(count, forbiddenAddresses);
}

THolderId TChunkPlacement::GetReplicationSource(const TChunk& chunk)
{
    // Right now we are just picking a random location (including cached ones).
    auto locations = chunk.GetLocations();
    int index = RandomNumber<size_t>(locations.size());
    return locations[index];
}

yvector<THolderId> TChunkPlacement::GetRemovalTargets(const TChunk& chunk, int count)
{
    // Construct a list of (holderId, loadFactor) pairs.
    typedef TPair<THolderId, double> TCandidatePair;
    yvector<TCandidatePair> candidates;
    candidates.reserve(chunk.StoredLocations().ysize());
    FOREACH(auto holderId, chunk.StoredLocations()) {
        const auto& holder = ChunkManager->GetHolder(holderId);
        double loadFactor = GetLoadFactor(holder);
        candidates.push_back(MakePair(holderId, loadFactor));
    }

    // Sort by loadFactor in descending order.
    std::sort(candidates.begin(), candidates.end(),
        [] (const TCandidatePair& lhs, const TCandidatePair& rhs)
        {
            return lhs.Second() > rhs.Second();
        });

    // Take first count holders.
    yvector<THolderId> result;
    result.reserve(count);
    FOREACH(auto pair, candidates) {
        if (result.ysize() >= count)
            break;
        result.push_back(pair.First());
    }
    return result;
}

THolderId TChunkPlacement::GetBalancingTarget(const TChunk& chunk, double maxFillCoeff)
{
    FOREACH (const auto& pair, LoadFactorMap) {
        const auto& holder = ChunkManager->GetHolder(pair.second);
        if (GetFillCoeff(holder) > maxFillCoeff) {
            break;
        }
        if (IsValidBalancingTarget(holder, chunk)) {
            return holder.GetId();
        }
    }
    return InvalidHolderId;
}

bool TChunkPlacement::IsValidUploadTarget(const THolder& targetHolder) const
{
    if (targetHolder.GetState() != EHolderState::Active) {
        // Do not upload anything to inactive holders.
        return false;
    }

    if (IsFull(targetHolder)) {
        // Do not upload anything to full holders.
        return false;
    }
            
    // Seems OK :)
    return true;
}

bool TChunkPlacement::IsValidBalancingTarget(const THolder& targetHolder, const TChunk& chunk) const
{
    if (!IsValidUploadTarget(targetHolder)) {
        // Balancing implies upload, after all.
        return false;
    }

    if (targetHolder.StoredChunkIds().find(chunk.GetId()) != targetHolder.StoredChunkIds().end())  {
        // Do not balance to a holder already having the chunk.
        return false;
    }

    FOREACH (const auto& jobId, targetHolder.JobIds()) {
        const auto& job = ChunkManager->GetJob(jobId);
        if (job.GetChunkId() == chunk.GetId()) {
            // Do not balance to a holder already having a job associated with this chunk.
            return false;
        }
    }

    auto* sink = ChunkManager->FindReplicationSink(targetHolder.GetAddress());
    if (sink) {
        if (static_cast<int>(sink->JobIds.size()) >= MaxReplicationFanIn) {
            // Do not balance to a holder with too many incoming replication jobs.
            return false;
        }

        FOREACH (const auto& jobId, sink->JobIds) {
            const auto& job = ChunkManager->GetJob(jobId);
            if (job.GetChunkId() == chunk.GetId()) {
                // Do not balance to a holder that is a replication target for the very same chunk.
                return false;
            }
        }
    }

    // Seems OK :)
    return true;
}

yvector<TChunkId> TChunkPlacement::GetBalancingChunks(const THolder& holder, int count)
{
    // Do not balance chunks that already have a job.
    yhash_set<TChunkId> forbiddenChunkIds;
    FOREACH (const auto& jobId, holder.JobIds()) {
        const auto& job = ChunkManager->GetJob(jobId);
        forbiddenChunkIds.insert(job.GetChunkId());
    }

    // Right now we just pick some (not even random!) chunks.
    yvector<TChunkId> result;
    result.reserve(count);
    FOREACH (const auto& chunkId, holder.StoredChunkIds()) {
        if (result.ysize() >= count)
            break;
        if (forbiddenChunkIds.find(chunkId) == forbiddenChunkIds.end()) {
            result.push_back(chunkId);
        }
    }
    FOREACH (const auto& chunkId, holder.CachedChunkIds()) {
        if (result.ysize() >= count)
            break;
        if (forbiddenChunkIds.find(chunkId) == forbiddenChunkIds.end()) {
            result.push_back(chunkId);
        }
    }

    return result;
}

int TChunkPlacement::GetSessionCount(const THolder& holder) const
{
    auto hintIt = HintedSessionsMap.find(holder.GetId());
    return hintIt == HintedSessionsMap.end() ? 0 : hintIt->Second();
}

double TChunkPlacement::GetLoadFactor(const THolder& holder) const
{
    const auto& statistics = holder.Statistics();
    return
        GetFillCoeff(holder) +
        ActiveSessionsPenalityCoeff * (statistics.session_count() + GetSessionCount(holder));
}

double TChunkPlacement::GetFillCoeff(const THolder& holder) const
{
    const auto& statistics = holder.Statistics();
    return
        (1.0 + statistics.used_space()) /
        (1.0 + statistics.used_space() + statistics.available_space());
}

bool TChunkPlacement::IsFull(const THolder& holder) const
{
    if (GetFillCoeff(holder) > MaxHolderFillCoeff)
        return true;

    const auto& statistics = holder.Statistics();
    if (statistics.available_space() - statistics.used_space() < MinHolderFreeSpace)
        return true;

    return false;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
