#pragma once

#include "public.h"

#include <yt/yt/core/rpc/public.h>

namespace NYT::NChaosNode {

////////////////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateChaosService(
    IChaosSlotPtr slot,
    IBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosNode
