#include "private.h"

namespace NYT::NHttpProxy {

////////////////////////////////////////////////////////////////////////////////

const NLogging::TLogger HttpProxyLogger("HttpProxy");
const NProfiling::TProfiler HttpProxyProfiler("/http_proxy");

const NLogging::TLogger HttpStructuredProxyLogger("HttpStructuredProxy");
const NProfiling::TProfiler HttpStructuredProxyProfiler("/http_structured_proxy");

const TString ClickHouseUserName = "yt-clickhouse";

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy

