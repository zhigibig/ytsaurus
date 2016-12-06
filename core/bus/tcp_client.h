#pragma once

#include "public.h"

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

//! Initializes a new client for communicating with a given address.
IBusClientPtr CreateTcpBusClient(TTcpBusClientConfigPtr config);

//////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT
