#pragma once

#include "public.h"

#include <yt/core/http/config.h>

namespace NYT::NRpc::NHttp {

////////////////////////////////////////////////////////////////////////////////

class TServerConfig
    : public NHttp::TServerConfig
{ };

DEFINE_REFCOUNTED_TYPE(TServerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpc::NHttp
