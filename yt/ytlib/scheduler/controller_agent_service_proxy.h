#pragma once

#include "public.h"

#include <yt/ytlib/scheduler/controller_agent_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TControllerAgentServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TControllerAgentServiceProxy, RPC_PROXY_DESC(ControllerAgentService)
        .SetProtocolVersion(1));

    DEFINE_RPC_PROXY_METHOD(NProto, Heartbeat);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

