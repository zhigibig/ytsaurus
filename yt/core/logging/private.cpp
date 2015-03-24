#include "stdafx.h"
#include "private.h"

namespace NYT {
namespace NLogging {

////////////////////////////////////////////////////////////////////////////////

const char* const SystemLoggingCategory = "Logging";
const char* const DefaultStderrWriterName = "Stderr";
const ELogLevel DefaultStderrMinLevel = ELogLevel::Info;
NProfiling::TProfiler LoggingProfiler("/logging");

////////////////////////////////////////////////////////////////////////////////

} // namespace NLogging
} // namespace NYT
