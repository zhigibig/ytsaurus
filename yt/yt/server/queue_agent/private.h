#pragma once

#include <yt/yt/client/queue_client/common.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/library/profiling/sensor.h>

namespace NYT::NQueueAgent {

////////////////////////////////////////////////////////////////////////////////

inline const NLogging::TLogger AlertManagerLogger("AlertManager");
inline const NLogging::TLogger QueueAgentLogger("QueueAgent");
inline const NLogging::TLogger CypressSynchronizerLogger("CypressSynchronizer");
inline const NProfiling::TProfiler QueueAgentProfiler = NProfiling::TProfiler("/queue_agent").WithGlobal();

////////////////////////////////////////////////////////////////////////////////

namespace NAlerts {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_ERROR_ENUM(
    ((CypressSynchronizerUnableToFetchObjectRevisions)            (3000))
    ((CypressSynchronizerUnableToFetchAttributes)                 (3001))
    ((CypressSynchronizerPassFailed)                              (3002))

    ((QueueAgentPassFailed)                                       (3030))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NAlerts

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TAlertManager)
DECLARE_REFCOUNTED_CLASS(TAlertManagerDynamicConfig)

DECLARE_REFCOUNTED_CLASS(TQueueAgent)
DECLARE_REFCOUNTED_CLASS(TQueueAgentConfig)
DECLARE_REFCOUNTED_CLASS(TQueueControllerDynamicConfig)
DECLARE_REFCOUNTED_CLASS(TQueueAgentDynamicConfig)

DECLARE_REFCOUNTED_STRUCT(ICypressSynchronizer)
DECLARE_REFCOUNTED_CLASS(TCypressSynchronizer)
DECLARE_REFCOUNTED_CLASS(TCypressSynchronizerConfig)
DECLARE_REFCOUNTED_CLASS(TCypressSynchronizerDynamicConfig)

DECLARE_REFCOUNTED_CLASS(TQueueAgentServerConfig)
DECLARE_REFCOUNTED_CLASS(TQueueAgentServerDynamicConfig)

DECLARE_REFCOUNTED_CLASS(TDynamicConfigManager)

////////////////////////////////////////////////////////////////////////////////

using TAgentId = TString;

////////////////////////////////////////////////////////////////////////////////

using TRowRevision = ui64;
constexpr TRowRevision NullRowRevision = 0;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TQueueTable)
DECLARE_REFCOUNTED_CLASS(TConsumerTable)
DECLARE_REFCOUNTED_CLASS(TConsumerRegistrationTable)
DECLARE_REFCOUNTED_STRUCT(TDynamicState)

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IObjectStore)
DECLARE_REFCOUNTED_STRUCT(IObjectController)
DECLARE_REFCOUNTED_STRUCT(IQueueController)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EObjectKind,
    (Queue)
    (Consumer)
);

DEFINE_ENUM(EQueueFamily,
    //! Sentinel value that does not correspond to any valid queue type.
    ((Null)                       (0))
    //! Regular ordered dynamic table.
    ((OrderedDynamicTable)        (1))
)

////////////////////////////////////////////////////////////////////////////////

struct TQueueTableRow;
struct TConsumerTableRow;
struct TConsumerRegistrationTableRow;

using TConsumerRowMap = THashMap<NQueueClient::TCrossClusterReference, TConsumerTableRow>;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(TQueueSnapshot);
using TQueueSnapshotConstPtr = TIntrusivePtr<const TQueueSnapshot>;
DECLARE_REFCOUNTED_STRUCT(TQueuePartitionSnapshot);
DECLARE_REFCOUNTED_STRUCT(TConsumerSnapshot);
DECLARE_REFCOUNTED_STRUCT(TSubConsumerSnapshot)
DECLARE_REFCOUNTED_STRUCT(TConsumerPartitionSnapshot);
using TConsumerSnapshotConstPtr = TIntrusivePtr<const TConsumerSnapshot>;
using TSubConsumerSnapshotConstPtr = TIntrusivePtr<const TSubConsumerSnapshot>;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IQueueProfileManager);
DECLARE_REFCOUNTED_STRUCT(IConsumerProfileManager);

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EConsumerPartitionDisposition,
    //! Sentinel value.
    (None)
    //! At the end of the window, i.e. unread row count == 0.
    (UpToDate)
    //! Inside the window but not at the end, i.e. 0 < unread row count <= available row count.
    (PendingConsumption)
    //! Past the window, i.e. unread row count > available row count.
    (Expired)
    //! Ahead of the window, i.e. "unread row count < 0" (unread row count is capped)
    (Ahead)
)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueAgent
