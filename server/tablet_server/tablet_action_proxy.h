#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/object_server/public.h>

namespace NYT {
namespace NTabletServer {

////////////////////////////////////////////////////////////////////////////////

NObjectServer::IObjectProxyPtr CreateTabletActionProxy(
    NCellMaster::TBootstrap* bootstrap,
    NObjectServer::TObjectTypeMetadata* metadata,
    TTabletAction* action);

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletServer
} // namespace NYT

