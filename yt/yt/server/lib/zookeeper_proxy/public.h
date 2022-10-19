#pragma once

#include <yt/yt/client/zookeeper/public.h>

namespace NYT::NZookeeperProxy {

////////////////////////////////////////////////////////////////////////////////

using NZookeeperClient::TSessionId;

////////////////////////////////////////////////////////////////////////////////

struct IBootstrapProxy;
struct IBootstrap;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(TZookeeperServerConfig)
DECLARE_REFCOUNTED_STRUCT(TZookeeperProxyConfig)

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IConnection)
DECLARE_REFCOUNTED_STRUCT(IServer)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NZookeeperProxy
