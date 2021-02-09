#include "private.h"

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

const NLogging::TLogger TabletNodeLogger("TabletNode");
const NProfiling::TRegistry TabletNodeProfiler{"/tablet_node"};
const NLogging::TLogger LsmLogger("Lsm");

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
