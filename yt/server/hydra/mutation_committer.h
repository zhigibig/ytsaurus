#pragma once

#include "private.h"
#include "mutation_context.h"

#include <core/concurrency/thread_affinity.h>

#include <core/misc/ring_queue.h>

#include <core/actions/signal.h>

#include <core/logging/tagged_logger.h>

#include <core/profiling/profiler.h>

#include <ytlib/election/public.h>

#include <ytlib/hydra/hydra_service_proxy.h>
#include <ytlib/hydra/version.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

//! A common base for TFollowerCommitter and TLeaderCommitter.
class TCommitter
    : public TRefCounted
{
public:
    TCommitter(
        NElection::TCellManagerPtr cellManager,
        TDecoratedAutomatonPtr decoratedAutomaton,
        IInvokerPtr epochControlInvoker,
        IInvokerPtr epochAutomatonInvoker,
        const NProfiling::TProfiler& profiler);

    ~TCommitter();

protected:
    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);

    NElection::TCellManagerPtr CellManager_;
    TDecoratedAutomatonPtr DecoratedAutomaton_;
    IInvokerPtr EpochControlInvoker_;
    IInvokerPtr EpochAutomatonInvoker_;
    NProfiling::TRateCounter CommitCounter_;
    NProfiling::TRateCounter BatchFlushCounter_;

    NLog::TTaggedLogger Logger;
    NProfiling::TProfiler Profiler;

 };

////////////////////////////////////////////////////////////////////////////////

//! Manages commits carried out by a leader.
class TLeaderCommitter
    : public TCommitter
{
public:
    //! Creates an instance.
    TLeaderCommitter(
        TDistributedHydraManagerConfigPtr config,
        NElection::TCellManagerPtr cellManager,
        TDecoratedAutomatonPtr decoratedAutomaton,
        IChangelogStorePtr changelogStore,
        TFollowerTrackerPtr followerTracker,
        const TEpochId& epoch,
        IInvokerPtr epochControlInvoker,
        IInvokerPtr epochAutomatonInvoker,
        const NProfiling::TProfiler& profiler);

    ~TLeaderCommitter();

    //! Initiates a new distributed commit.
    /*!
     *  A distributed commit is completed when the mutation is received, applied,
     *  and flushed to the changelog by a quorum of replicas.
     *
     *  \note Thread affinity: AutomatonThread
     */
    TFuture<TErrorOr<TMutationResponse>> Commit(const TMutationRequest& request);

    //! Sends out the current batch of mutations.
    void Flush();

    //! Returns a future that is set when all mutations submitted to #Commit are
    //! flushed by a quorum of changelogs.
    /*!
     *  \note Thread affinity: AutomatonThread
     */
    TFuture<void> GetQuorumFlushResult();

    //! Temporarily suspends writing mutations to the changelog and keeps them in memory.
    /*!
     *  \note Thread affinity: AutomatonThread
     */
    void SuspendLogging();

    //! Resumes an earlier suspended mutation logging and sends out all pending mutations.
    /*!
     *  \note Thread affinity: AutomatonThread
     */
    void ResumeLogging();


    //! Raised each time the current changelog reaches its maximum size.
    /*!
     *  \note Thread affinity: AutomatonThread
     */
    DEFINE_SIGNAL(void(), ChangelogLimitReached);

private:
    class TBatch;
    typedef TIntrusivePtr<TBatch> TBatchPtr;

    void OnBatchTimeout(TBatchPtr batch);
    void OnBatchCommitted(TBatchPtr batch, TError error);
    TIntrusivePtr<TBatch> GetOrCreateBatch(TVersion version);
    void AddToBatch(
        TVersion version,
        const TSharedRef& recordData,
        TFuture<void> localResult);
    void FlushCurrentBatch();

    TDistributedHydraManagerConfigPtr Config_;
    IChangelogStorePtr ChangelogStore_;
    TFollowerTrackerPtr FollowerTracker_;
    TEpochId EpochId_;

    struct TPendingMutation
    {
        TMutationRequest Request;
        TPromise<TErrorOr<TMutationResponse>> CommitPromise;
    };
    
    bool LoggingSuspended_;
    TRingQueue<TPendingMutation> PendingMutations_;

    TSpinLock BatchSpinLock_;
    TBatchPtr CurrentBatch_;
    NConcurrency::TDelayedExecutor::TCookie BatchTimeoutCookie_;

};

////////////////////////////////////////////////////////////////////////////////

//! Manages commits carried out by a follower.
class TFollowerCommitter
    : public TCommitter
{
public:
    //! Creates an instance.
    TFollowerCommitter(
        NElection::TCellManagerPtr cellManager,
        TDecoratedAutomatonPtr decoratedAutomaton,
        IInvokerPtr epochControlInvoker,
        IInvokerPtr epochAutomatonInvoker,
        const NProfiling::TProfiler& profiler);

    ~TFollowerCommitter();

    //! Logs a batch of mutations at the follower.
    /*!
     *  \note Thread affinity: ControlThread
     */
    TAsyncError LogMutations(
        TVersion expectedVersion,
        const std::vector<TSharedRef>& recordsData);

private:
    TAsyncError DoLogMutations(
        TVersion expectedVersion,
        const std::vector<TSharedRef>& recordsData);

};

////////////////////////////////////////////////////////////////////////////////


} // namespace NHydra
} // namespace NYT
