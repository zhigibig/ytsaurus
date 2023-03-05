#pragma once

#include <yt/yt/core/misc/guid.h>

#include <library/cpp/yt/misc/enum.h>

namespace NYT::NQueryTrackerClient {

///////////////////////////////////////////////////////////////////////////////

YT_DEFINE_ERROR_ENUM(
    ((IncarnationMismatch)  (3900))
    ((QueryNotFound)        (3901))
    ((QueryResultNotFound)  (3902))
);

////////////////////////////////////////////////////////////////////////////////

using TQueryId = TGuid;

////////////////////////////////////////////////////////////////////////////////

DEFINE_STRING_SERIALIZABLE_ENUM(EQueryEngine,
    (Ql)
    (Yql)
    (Mock)
)

DEFINE_STRING_SERIALIZABLE_ENUM(EQueryState,
    (Draft)
    (Pending)
    (Running)
    (Aborting)
    (Aborted)
    (Completing)
    (Completed)
    (Failing)
    (Failed)
)

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryTrackerClient
