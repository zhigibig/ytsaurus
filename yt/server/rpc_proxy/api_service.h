#pragma once

#include "public.h"

#include <yt/server/cell_proxy/public.h>

#include <yt/ytlib/rpc_proxy/public.h>

#include <yt/core/rpc/public.h>

namespace NYT {
namespace NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

NRpc::IServicePtr CreateApiService(
    NCellProxy::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpcProxy
} // namespace NYT
