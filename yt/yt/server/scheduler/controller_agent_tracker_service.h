#pragma once

#include "public.h"

#include <yt/yt/core/rpc/public.h>

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateControllerAgentTrackerService(TBootstrap* bootstrap, const NRpc::TResponseKeeperPtr& responseKeeper);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler

