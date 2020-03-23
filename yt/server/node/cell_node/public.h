#pragma once

#include <yt/server/lib/misc/public.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/misc/public.h>

namespace NYT::NCellNode {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap;

DECLARE_REFCOUNTED_CLASS(TResourceLimitsConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicConfigManagerConfig)
DECLARE_REFCOUNTED_CLASS(TCellNodeConfig)
DECLARE_REFCOUNTED_CLASS(TCellNodeDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TBatchingChunkServiceConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicConfigManager)

using NNodeTrackerClient::TNodeMemoryTracker;
using NNodeTrackerClient::TNodeMemoryTrackerPtr;
using NNodeTrackerClient::TNodeMemoryTrackerGuard;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EErrorCode,
    ((UnrecognizedConfigOption)              (1400))
    ((FailedToFetchDynamicConfig)            (1401))
    ((DuplicateSuitableDynamicConfigs)       (1402))
    ((UnrecognizedDynamicConfigOption)       (1403))
    ((FailedToApplyDynamicConfig)            (1404))
    ((InvalidDynamicConfig)                  (1405))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellNode
