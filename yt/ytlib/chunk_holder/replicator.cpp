#include "stdafx.h"
#include "replicator.h"

#include "../misc/assert.h"
#include "../misc/string.h"
#include "../chunk_client/remote_writer.h"

namespace NYT {
namespace NChunkHolder {

////////////////////////////////////////////////////////////////////////////////

using namespace NYT::NChunkClient;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkHolderLogger;

////////////////////////////////////////////////////////////////////////////////

TJob::TJob(
    IInvoker::TPtr serviceInvoker,
    TChunkStore::TPtr chunkStore,
    TBlockStore::TPtr blockStore,
    EJobType jobType,
    const TJobId& jobId,
    TChunk::TPtr chunk,
    const yvector<Stroka>& targetAddresses)
    : ChunkStore(chunkStore)
    , BlockStore(blockStore)
    , JobType(jobType)
    , JobId(jobId)
    , State(EJobState::Running)
    , Chunk(chunk)
    , TargetAddresses(targetAddresses)
    , CancelableInvoker(New<TCancelableInvoker>(serviceInvoker))
{
    YASSERT(~serviceInvoker != NULL);
    YASSERT(~chunkStore != NULL);
    YASSERT(~blockStore != NULL);
    YASSERT(~chunk != NULL);
}

EJobType TJob::GetType() const
{
    return JobType;
}

TJobId TJob::GetJobId() const
{
    return JobId;
}

NYT::NChunkHolder::EJobState TJob::GetState() const
{
    return State;
}

yvector<Stroka> TJob::GetTargetAddresses() const
{
    return TargetAddresses;
}

TChunk::TPtr TJob::GetChunk() const
{
    return Chunk;
}

void TJob::Start()
{
    switch (JobType) {
        case EJobType::Remove: {
            LOG_INFO("Removal job started (JobId: %s, ChunkId: %s)",
                ~JobId.ToString(),
                ~Chunk->GetId().ToString());

            ChunkStore->RemoveChunk(~Chunk);

            LOG_DEBUG("Removal job completed (JobId: %s)",
                ~JobId.ToString());

            State = EJobState::Completed;
            break;
        }

        case EJobType::Replicate: {
            LOG_INFO("Replication job started (JobId: %s, TargetAddresses: [%s], ChunkId: %s)",
                ~JobId.ToString(),
                ~JoinToString(TargetAddresses),
                ~Chunk->GetId().ToString());

            Writer = New<TRemoteWriter>(
                TRemoteWriter::TConfig(),
                Chunk->GetId(),
                TargetAddresses);

            ReplicateBlock(TAsyncStreamState::TResult(), 0);
            break;
        }

        default:
            YUNREACHABLE();
    }
}

void TJob::Stop()
{
    CancelableInvoker->Cancel();
    if (~Writer != NULL) {
        Writer->Cancel("Job stopped.");
    }
}

void TJob::ReplicateBlock(TAsyncStreamState::TResult result, int blockIndex)
{
    if (!result.IsOK) {
        LOG_WARNING("Replication failed (JobId: %s, BlockIndex: %d, Error: %s): ",
            ~JobId.ToString(),
            blockIndex,
            ~result.ErrorMessage);

        State = EJobState::Failed;
        return;
    }

    if (blockIndex >= Chunk->GetBlockCount()) {
        LOG_DEBUG("All blocks are enqueued for replication (JobId: %s)",
            ~JobId.ToString());

        Writer->AsyncClose(Chunk->GetMasterMeta())->Subscribe(
            FromMethod(
                &TJob::OnWriterClosed,
                TPtr(this))
            ->Via(~CancelableInvoker));
        return;
    }

    TBlockId blockId(Chunk->GetId(), blockIndex);

    LOG_DEBUG("Retrieving block for replication (JobId: %s, BlockIndex: %d)",
        ~JobId.ToString(), 
        blockIndex);

    BlockStore->FindBlock(blockId)->Subscribe(
        FromMethod(
            &TJob::OnBlockLoaded,
            TPtr(this),
            blockIndex)
        ->Via(~CancelableInvoker));
}

void TJob::OnBlockLoaded(TCachedBlock::TPtr cachedBlock, int blockIndex)
{
    if (~cachedBlock == NULL) {
        LOG_WARNING("Replication chunk is missing (JobId: %s, BlockIndex: %d)",
            ~JobId.ToString(),
            blockIndex);

        State = EJobState::Failed;
        return;
    } 

    Writer->AsyncWriteBlock(cachedBlock->GetData())->Subscribe(
        FromMethod(&TJob::ReplicateBlock, TPtr(this), blockIndex + 1)
            ->Via(CancelableInvoker));
}

void TJob::OnWriterClosed(TAsyncStreamState::TResult result)
{
    if (result.IsOK) {
        LOG_DEBUG("Replication job completed (JobId: %s)",
            ~JobId.ToString());

        Writer.Reset();
        State = EJobState::Completed;
    } else {
        LOG_WARNING("Replication job failed (JobId: %s)",
            ~JobId.ToString());

        Writer.Reset();
        State = EJobState::Failed;
    }
}

////////////////////////////////////////////////////////////////////////////////

TReplicator::TReplicator(
    TChunkStore::TPtr chunkStore,
    TBlockStore::TPtr blockStore,
    IInvoker::TPtr serviceInvoker)
    : ChunkStore(chunkStore)
    , BlockStore(blockStore)
    , ServiceInvoker(serviceInvoker)
{ }

TJob::TPtr TReplicator::StartJob(
    EJobType jobType,
    const TJobId& jobId,
    TChunk::TPtr chunk,
    const yvector<Stroka>& targetAddresses)
{
    auto job = New<TJob>(
        ServiceInvoker,
        ChunkStore,
        BlockStore,
        jobType,
        jobId,
        chunk,
        targetAddresses);
    YVERIFY(Jobs.insert(MakePair(jobId, job)).Second());
    job->Start();

    return job;
}

void TReplicator::StopJob(TJob::TPtr job)
{
    job->Stop();
    YVERIFY(Jobs.erase(job->GetJobId()) == 1);
    
    LOG_INFO("Job stopped (JobId: %s, State: %s)",
        ~job->GetJobId().ToString(),
        ~job->GetState().ToString());
}

TJob::TPtr TReplicator::FindJob(const TJobId& jobId)
{
    auto it = Jobs.find(jobId);
    return it == Jobs.end() ? NULL : it->Second();
}

yvector<TJob::TPtr> TReplicator::GetAllJobs()
{
    yvector<TJob::TPtr> result;
    FOREACH(const auto& pair, Jobs) {
        result.push_back(pair.second);
    }
    return result;
}

void TReplicator::StopAllJobs()
{
    FOREACH(auto& pair, Jobs) {
        pair.second->Stop();
    }
    Jobs.clear();

    LOG_INFO("All jobs stopped");
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT
