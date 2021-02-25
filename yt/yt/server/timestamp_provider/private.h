#pragma once

#include <yt/library/profiling/sensor.h>

#include <yt/core/logging/log.h>

namespace NYT::NTimestampProvider {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TTimestampProviderConfig)

struct IBootstrap;

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger TimestampProviderLogger;

extern const NProfiling::TRegistry TimestampProviderProfiler;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTimestampProvider
