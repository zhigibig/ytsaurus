#pragma once

#include "common.h"
#include "chunk_store.h"
#include "block_store.h"

#include <ytlib/misc/guid.h>
#include <ytlib/misc/async_stream_state.h>
#include <ytlib/chunk_client/async_reader.h>
#include <ytlib/chunk_client/async_writer.h>
#include <ytlib/logging/tagged_logger.h>


namespace NYT {
namespace NChunkHolder {

////////////////////////////////////////////////////////////////////////////////

class TJobExecutor;

//! Represents a replication job on a chunk holder.
class TJob
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TJob> TPtr;

    TJob(
        IInvoker* serviceInvoker,
        TChunkStore* chunkStore,
        TBlockStore* blockStore,
        EJobType jobType,
        const TJobId& jobId,
        TStoredChunk* chunk,
        const yvector<Stroka>& targetAddresses);

    //! Returns the type.
    EJobType GetType() const;

    //! Returns the id.
    TJobId GetJobId() const;

    //! Returns the current state.
    EJobState GetState() const;

    //! Returns the addresses of chunk holders where the chunk is being replicated to.
    yvector<Stroka> GetTargetAddresses() const;

    //! Returns the chunk that is being replicated.
    TChunk::TPtr GetChunk() const;

private:
    friend class TJobExecutor;

    TChunkStore::TPtr ChunkStore;
    TBlockStore::TPtr BlockStore;
    EJobType JobType;
    TJobId JobId;
    EJobState State;
    TStoredChunk::TPtr Chunk;
    NProto::TChunkInfo ChunkInfo;
    yvector<Stroka> TargetAddresses;
    NChunkClient::IAsyncWriter::TPtr Writer;
    TCancelableInvoker::TPtr CancelableInvoker;
    
    NLog::TTaggedLogger Logger;

    void Start();
    void Stop();

    void ReplicateBlock(TError error, int blockIndex);
    void OnChunkInfoLoaded(NChunkClient::IAsyncReader::TGetInfoResult result);
    void OnBlockLoaded(TBlockStore::TGetBlockResult result, int blockIndex);
    void OnWriterClosed(TError error);
};

////////////////////////////////////////////////////////////////////////////////

//! Controls chunk replication and removal on a chunk holder.
/*!
 *  Each chunk holder has a set of currently active replication jobs.
 *  These jobs are started by the master and are used for two purposes:
 *  making additional replicas of chunks lacking enough of them and
 *  moving chunks around chunk holders to ensure even distribution.
 *  
 *  Each job is represented by an instance of TJob class.
 *  A job is created by calling #StartJob and stopped by calling #StopJob methods.
 *  
 *  Each job may be either running, completed or failed.
 *  Completed and failed job do not vanish automatically. It is the responsibility
 *  of the master to stop them.
 *  
 *  The status of all jobs is propagated to the master with each heartbeat.
 *  This way the master obtains the outcomes of each job it had started.
 * 
 *  A job is identified by its id, which is assigned by the master when a job is started.
 *  Using master-controlled id assignment eliminates the need for additional RPC round-trips
 *  for getting these ids from the holder.
 */
class TJobExecutor
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TJobExecutor> TPtr;

    //! Constructs a new instance.
    TJobExecutor(
        TChunkStore* chunkStore,
        TBlockStore* blockStore,
        IInvoker* serviceInvoker);
    
    //! Starts a new job with the given parameters.
    TJob::TPtr StartJob(
        EJobType jobType,
        const TJobId& jobId,
        TStoredChunk* chunk,
        const yvector<Stroka>& targetAddresses);

    //! Stops the job.
    void StopJob(TJob* job);

    // TODO: is it needed?
    //! Stop all currently active jobs.
    void StopAllJobs();

    //! Finds job by its id. Returns NULL if no job is found.
    TJob::TPtr FindJob(const TJobId& jobId);

    //! Gets all active jobs.
    yvector<TJob::TPtr> GetAllJobs();

private:
    typedef yhash_map<TJobId, TJob::TPtr> TJobMap;

    TChunkStore::TPtr ChunkStore;
    TBlockStore::TPtr BlockStore;
    IInvoker::TPtr ServiceInvoker;

    TJobMap Jobs;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT

