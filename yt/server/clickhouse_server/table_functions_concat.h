#pragma once

#include "private.h"
#include "cluster_tracker.h"

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

void RegisterConcatenatingTableFunctions(IExecutionClusterPtr cluster);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
