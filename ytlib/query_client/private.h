#pragma once

#include "public.h"

#include <yt/ytlib/table_client/public.h>

#include <yt/core/logging/log.h>

// FIXME(lukyan): try to fix in new visual studio
#ifdef _win_
#define MOVE(name) name
#else
#define MOVE(name) name = std::move(name)
#endif

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger QueryClientLogger;

NLogging::TLogger BuildLogger(TConstQueryPtr query);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

