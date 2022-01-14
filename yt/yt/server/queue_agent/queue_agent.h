#pragma once

#include "private.h"

#include "dynamic_state.h"

#include <yt/yt/ytlib/hive/public.h>

namespace NYT::NQueueAgent {

////////////////////////////////////////////////////////////////////////////////

//! Object responsible for tracking the list of queues assigned to this particular controller.
class TQueueAgent
    : public TRefCounted
{
public:
    DEFINE_BYVAL_RO_PROPERTY(NYTree::IYPathServicePtr, OrchidService);

public:
    TQueueAgent(
        TQueueAgentConfigPtr config,
        NHiveClient::TClusterDirectoryPtr clusterDirectory,
        IInvokerPtr controlInvoker,
        TDynamicStatePtr dynamicState);

    void Start();

    void Stop();

private:
    const TQueueAgentConfigPtr Config_;
    const NHiveClient::TClusterDirectoryPtr ClusterDirectory_;
    const IInvokerPtr ControlInvoker_;
    const TDynamicStatePtr DynamicState_;
    const NConcurrency::TThreadPoolPtr ControllerThreadPool_;
    const NConcurrency::TPeriodicExecutorPtr PollExecutor_;

    std::atomic<bool> Active_ = false;

    struct TQueue
    {
        //! Row revision of a queue row corresponding to this object.
        TRowRevision RowRevision = NullRowRevision;

        //! If set, defines the reason why this queue is not functioning properly.
        //! Invariant: either #Error.IsOK() or #Controller == nullptr.
        TError Error;

        //! Queue controller that does all background activity.
        IQueueControllerPtr Controller;

        //! If #Error.IsOK(), contains the deduced type of a queue.
        EQueueType QueueType = EQueueType::Null;

        //! Revisions of the consumer rows, for which the controller was created.
        THashMap<TCrossClusterReference, TRowRevision> ConsumerRowRevisions;

        //! Properly stops #Controller if it is set and resets it.
        void Reset();
    };

    using TQueueMap = THashMap<TCrossClusterReference, TQueue>;
    TQueueMap Queues_;

    struct TConsumer
    {
        //! Row revision of a consumer row corresponding to this object.
        TRowRevision RowRevision = NullRowRevision;

        //! If set, defines the reason why this consumer is not functioning properly.
        TError Error;
        //! Target cross-cluster reference.
        std::optional<TCrossClusterReference> Target;
    };

    using TConsumerMap = THashMap<TCrossClusterReference, TConsumer>;
    TConsumerMap Consumers_;

    //! Latest non-trivial poll iteration error.
    TError LatestPollError_ = TError() << TErrorAttribute("poll_index", -1);
    //! Current poll iteration instant.
    TInstant PollInstant_ = TInstant::Zero();
    //! Index of a current poll iteration.
    i64 PollIndex_ = 0;

    void BuildOrchid(NYson::IYsonConsumer* consumer) const;

    //! One iteration of state polling and queue/consumer in-memory state updating.
    void Poll();

    //! Stops periodic polling, resets all controllers and erases queue and consumer mappings.
    void DoStop();
};

DEFINE_REFCOUNTED_TYPE(TQueueAgent)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueAgent
