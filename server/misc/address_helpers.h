#pragma once

#include <yt/ytlib/node_tracker_client/node_directory.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

NNodeTrackerClient::TAddressMap GetLocalAddresses(
    const NNodeTrackerClient::TAddressList& addresses,
    int port);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
