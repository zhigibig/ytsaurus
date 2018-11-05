#pragma once

#include <yt/core/logging/log.h>

namespace NYP {
namespace NServer {
namespace NObjects {

////////////////////////////////////////////////////////////////////////////////

extern const NYT::NLogging::TLogger Logger;

static constexpr int DBVersion = 13;

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjects
} // namespace NServer
} // namespace NYP
