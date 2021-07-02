#pragma once

#include "public.h"

#include <yt/yt/core/http/public.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

NHttp::IHttpHandlerPtr MakeSkynetHttpHandler(IBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
