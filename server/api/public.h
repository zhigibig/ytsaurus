#pragma once

#include <yp/server/misc/public.h>

namespace NYP::NServer::NApi {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMasterInterface,
    (Client)
    (SecureClient)
    (Agent)
);

DECLARE_REFCOUNTED_CLASS(TGetUserAccessAllowedToConfig)
DECLARE_REFCOUNTED_CLASS(TObjectServiceConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NApi
