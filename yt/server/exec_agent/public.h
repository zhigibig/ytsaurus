#pragma once

#include <yt/ytlib/job_tracker_client/public.h>

#include <yt/core/misc/public.h>

namespace NYT {
namespace NExecAgent {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TJobProxyResources;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

using NJobTrackerClient::TJobId;
using NJobTrackerClient::TOperationId;
using NJobTrackerClient::EJobType;
using NJobTrackerClient::EJobState;
using NJobTrackerClient::EJobPhase;

DEFINE_ENUM(EErrorCode,
    ((ConfigCreationFailed)   (1100))
    ((AbortByScheduler)       (1101))
    ((ResourceOverdraft)      (1102))
    ((AllLocationsDisabled)   (1103))
    ((JobEnvironmentDisabled) (1104))
);

DEFINE_ENUM(ESandboxKind,
    (User)
    (Udf)
    (Home)
    (Pipes)
);

DEFINE_ENUM(EJobEnvironmentType,
    (Simple)
    (Cgroups)
);

extern const TEnumIndexedVector<Stroka, ESandboxKind> SandboxDirectoryNames;

extern const Stroka ProxyConfigFileName;

DECLARE_REFCOUNTED_CLASS(TSlotManager)
DECLARE_REFCOUNTED_CLASS(TSlotLocation)

DECLARE_REFCOUNTED_STRUCT(ISlot)

DECLARE_REFCOUNTED_CLASS(TSlotLocationConfig)

DECLARE_REFCOUNTED_CLASS(TSchedulerConnector)

DECLARE_REFCOUNTED_STRUCT(IJobEnvironment)

DECLARE_REFCOUNTED_CLASS(TJobEnvironmentConfig)
DECLARE_REFCOUNTED_CLASS(TSimpleJobEnvironmentConfig)
DECLARE_REFCOUNTED_CLASS(TCGroupJobEnvironmentConfig)

DECLARE_REFCOUNTED_CLASS(TSlotManagerConfig)
DECLARE_REFCOUNTED_CLASS(TSchedulerConnectorConfig)
DECLARE_REFCOUNTED_CLASS(TExecAgentConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
