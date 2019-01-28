#pragma once

#include "public.h"

#include <yt/server/lib/exec_agent/config.h>

#include <yt/server/lib/misc/config.h>

#include <yt/server/lib/object_server/config.h>

#include <yt/server/node/query_agent/config.h>

#include <yt/server/lib/tablet_node/config.h>

#include <yt/server/node/data_node/config.h>

#include <yt/ytlib/hive/config.h>

#include <yt/ytlib/api/native/config.h>

#include <yt/ytlib/node_tracker_client/config.h>
#include <yt/ytlib/node_tracker_client/helpers.h>

#include <yt/core/concurrency/config.h>

namespace NYT::NCellNode {

////////////////////////////////////////////////////////////////////////////////

class TResourceLimitsConfig
    : public NYTree::TYsonSerializable
{
public:
    i64 Memory;

    TResourceLimitsConfig()
    {
        // Very low default, override for production use.
        RegisterParameter("memory", Memory)
            .GreaterThanOrEqual(0)
            .Default(5_GB);
    }
};

DEFINE_REFCOUNTED_TYPE(TResourceLimitsConfig)

////////////////////////////////////////////////////////////////////////////////

class   TBatchingChunkServiceConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration MaxBatchDelay;
    int MaxBatchCost;
    NConcurrency::TThroughputThrottlerConfigPtr CostThrottler;

    TBatchingChunkServiceConfig()
    {
        RegisterParameter("max_batch_delay", MaxBatchDelay)
            .Default(TDuration::Zero());
        RegisterParameter("max_batch_cost", MaxBatchCost)
            .Default(1000);
        RegisterParameter("cost_throttler", CostThrottler)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TBatchingChunkServiceConfig)

////////////////////////////////////////////////////////////////////////////////

class TCellNodeConfig
    : public TServerConfig
{
public:
    //! Interval between Orchid cache rebuilds.
    TDuration OrchidCacheUpdatePeriod;

    //! Node-to-master connection.
    NApi::NNative::TConnectionConfigPtr ClusterConnection;

    //! Node directory synchronization.
    NNodeTrackerClient::TNodeDirectorySynchronizerConfigPtr NodeDirectorySynchronizer;

    //! Data node configuration part.
    NDataNode::TDataNodeConfigPtr DataNode;

    //! Exec node configuration part.
    NExecAgent::TExecAgentConfigPtr ExecAgent;

    //! Tablet node configuration part.
    NTabletNode::TTabletNodeConfigPtr TabletNode;

    //! Query node configuration part.
    NQueryAgent::TQueryAgentConfigPtr QueryAgent;

    //! Metadata cache service configuration.
    NObjectServer::TMasterCacheServiceConfigPtr MasterCacheService;

    //! Chunk Service batcher and redirector.
    TBatchingChunkServiceConfigPtr BatchingChunkService;

    //! Known node addresses.
    NNodeTrackerClient::TNetworkAddressList Addresses;

    //! A set of tags to be assigned to this node.
    /*!
     * These tags are merged with others (e.g. provided by user and provided by master) to form
     * the full set of tags.
     */
    std::vector<TString> Tags;

    //! Limits for the node process and all jobs controlled by it.
    TResourceLimitsConfigPtr ResourceLimits;

    //! Timeout for RPC query in JobBandwidthThrottler.
    NJobProxy::TJobThrottlerConfigPtr JobThrottler;

    i64 FootprintMemorySize;

    TDuration FootprintUpdatePeriod;

    int SkynetHttpPort;

    NYTree::IMapNodePtr CypressAnnotations;

    TCellNodeConfig()
    {
        RegisterParameter("orchid_cache_update_period", OrchidCacheUpdatePeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("cluster_connection", ClusterConnection);
        RegisterParameter("node_directory_synchronizer", NodeDirectorySynchronizer)
            .DefaultNew();
        RegisterParameter("data_node", DataNode)
            .DefaultNew();
        RegisterParameter("exec_agent", ExecAgent)
            .DefaultNew();
        RegisterParameter("tablet_node", TabletNode)
            .DefaultNew();
        RegisterParameter("query_agent", QueryAgent)
            .DefaultNew();
        RegisterParameter("master_cache_service", MasterCacheService)
            .DefaultNew();
        RegisterParameter("batching_chunk_service", BatchingChunkService)
            .DefaultNew();
        RegisterParameter("addresses", Addresses)
            .Default();
        RegisterParameter("tags", Tags)
            .Default();
        RegisterParameter("resource_limits", ResourceLimits)
            .DefaultNew();
        RegisterParameter("job_throttler", JobThrottler)
            .DefaultNew();

        RegisterParameter("footprint_memory_size", FootprintMemorySize)
            .Default(1_GB)
            .GreaterThanOrEqual(100 * 1_MB);

        RegisterParameter("footprint_update_period", FootprintUpdatePeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("skynet_http_port", SkynetHttpPort)
            .Default(10080);

        RegisterParameter("cypress_annotations", CypressAnnotations)
            .Default(NYTree::BuildYsonNodeFluently()
                .BeginMap()
                .EndMap()
            ->AsMap());

        RegisterPostprocessor([&] () {
            NNodeTrackerClient::ValidateNodeTags(Tags);
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TCellNodeConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellNode
