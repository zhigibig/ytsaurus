#pragma once

#include "private.h"
#include "dynamic_state.h"

#include <yt/yt/server/lib/cypress_election/public.h>

#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/ytlib/hive/public.h>

#include <yt/yt/core/ytree/public.h>

#include <yt/yt/core/rpc/bus/public.h>

namespace NYT::NQueueAgent {

////////////////////////////////////////////////////////////////////////////////

struct TClusterProfilingCounters
{
    NProfiling::TGauge Queues;
    NProfiling::TGauge Consumers;
    NProfiling::TGauge Partitions;

    explicit TClusterProfilingCounters(NProfiling::TProfiler profiler);
};

//! Object responsible for tracking the list of queues assigned to this particular controller.
class TQueueAgent
    : public TRefCounted
{
public:
    TQueueAgent(
        TQueueAgentConfigPtr config,
        NApi::NNative::IConnectionPtr nativeConnection,
        NHiveClient::TClientDirectoryPtr clientDirectory,
        IInvokerPtr controlInvoker,
        TDynamicStatePtr dynamicState,
        NCypressElection::ICypressElectionManagerPtr electionManager,
        TString agentId);

    void Start();

    void Stop();

    NYTree::IMapNodePtr GetOrchidNode() const;

    void OnDynamicConfigChanged(
        const TQueueAgentDynamicConfigPtr& oldConfig,
        const TQueueAgentDynamicConfigPtr& newConfig);

    void PopulateAlerts(std::vector<TError>* alerts) const;

private:
    const TQueueAgentConfigPtr Config_;
    TQueueAgentDynamicConfigPtr DynamicConfig_;
    const NHiveClient::TClientDirectoryPtr ClientDirectory_;
    const IInvokerPtr ControlInvoker_;
    const TDynamicStatePtr DynamicState_;
    const NCypressElection::ICypressElectionManagerPtr ElectionManager_;
    const NConcurrency::TThreadPoolPtr ControllerThreadPool_;
    const NConcurrency::TPeriodicExecutorPtr PollExecutor_;
    TString AgentId_;
    THashMap<TString, TClusterProfilingCounters> ClusterProfilingCounters_;

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
        EQueueFamily QueueFamily = EQueueFamily::Null;

        //! Revisions of the consumer rows, for which the controller was created.
        THashMap<NQueueClient::TCrossClusterReference, TRowRevision> ConsumerRowRevisions;

        //! Properly stops #Controller if it is set and resets it.
        void Reset();
    };

    using TQueueMap = THashMap<NQueueClient::TCrossClusterReference, TQueue>;
    TQueueMap Queues_;

    struct TConsumer
    {
        //! Row revision of a consumer row corresponding to this object.
        TRowRevision RowRevision = NullRowRevision;

        //! If set, defines the reason why this consumer is not functioning properly.
        TError Error;
        //! Target cross-cluster reference.
        std::optional<NQueueClient::TCrossClusterReference> Target;
    };

    using TConsumerMap = THashMap<NQueueClient::TCrossClusterReference, TConsumer>;
    TConsumerMap Consumers_;

    //! Current poll error if any.
    TError PollError_;
    //! Current poll iteration instant.
    TInstant PollInstant_ = TInstant::Zero();
    //! Index of a current poll iteration.
    i64 PollIndex_ = 0;

    NRpc::IChannelFactoryPtr QueueAgentChannelFactory_;

    NYTree::INodePtr QueueObjectServiceNode_;
    NYTree::INodePtr ConsumerObjectServiceNode_;

    std::vector<TError> Alerts_;

    NYTree::IYPathServicePtr RedirectYPathRequestToLeader(TStringBuf queryRoot, TStringBuf key);

    void BuildQueueYson(const NQueueClient::TCrossClusterReference& queueRef, const TQueue& queue, NYson::IYsonConsumer* ysonConsumer);
    void BuildConsumerYson(const NQueueClient::TCrossClusterReference& consumerRef, const TConsumer& consumer, NYson::IYsonConsumer* ysonConsumer);

    //! One iteration of state polling and queue/consumer in-memory state updating.
    void Poll();

    //! Stops periodic polling, resets all controllers and erases queue and consumer mappings.
    void DoStop();

    void DoPopulateAlerts(std::vector<TError>* alerts) const;

    TClusterProfilingCounters& GetOrCreateClusterProfilingCounters(TString cluster);

    void Profile();
};

DEFINE_REFCOUNTED_TYPE(TQueueAgent)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueAgent
