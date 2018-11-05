#pragma once

#include "private.h"

#include <yt/core/misc/property.h>
#include <yt/core/misc/ref_tracked.h>

namespace NYP {
namespace NServer {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TNetworkModule
    : public NYT::TRefTracked<TNetworkModule>
{
public:
    DEFINE_BYREF_RW_PROPERTY(int, AllocatedInternetAddressCount);
    DEFINE_BYREF_RW_PROPERTY(int, InternetAddressCount);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NServer
} // namespace NYP
