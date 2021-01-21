#pragma once

#include <yt/core/misc/public.h>

#include <yt/core/logging/log.h>

#include <yt/ytlib/discovery_client/public.h>

namespace NYT::NDiscoveryServer {

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger DiscoveryServerLogger;

////////////////////////////////////////////////////////////////////////////////

using TGroupId = NDiscoveryClient::TGroupId;
using TMemberId = NDiscoveryClient::TMemberId;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TMember)
DECLARE_REFCOUNTED_CLASS(TGroup)
DECLARE_REFCOUNTED_CLASS(TGroupManager)
DECLARE_REFCOUNTED_CLASS(TGroupTree)

DECLARE_REFCOUNTED_STRUCT(IDiscoveryServer)

DECLARE_REFCOUNTED_CLASS(TDiscoveryServerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDiscoveryServer
