#pragma once

#include <ytlib/misc/common.h>

namespace NYT {
namespace NCellNode {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap;

struct TCellNodeConfig;
typedef TIntrusivePtr<TCellNodeConfig> TCellNodeConfigPtr;

////////////////////////////////////////////////////////////////////////////////

DECLARE_ENUM(EControlThreadQueue,
    (Default)
    (Heartbeat)
);

////////////////////////////////////////////////////////////////////////////////
            
} // namespace NCellNode
} // namespace NYT
