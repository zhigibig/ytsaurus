#pragma once

#include <yt/ytlib/election/public.h>

#include <yt/core/misc/public.h>

#include <yt/core/rpc/public.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TSnapshotMeta;
class TChangelogMeta;

} // namespace NProto

DECLARE_REFCOUNTED_CLASS(TPeerConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TRemoteSnapshotStoreOptions)
DECLARE_REFCOUNTED_CLASS(TRemoteChangelogStoreOptions)

DEFINE_ENUM(EPeerState,
    ((None)                       (0))
    ((Stopped)                    (1))
    ((Elections)                  (2))
    ((FollowerRecovery)           (3))
    ((Following)                  (4))
    ((LeaderRecovery)             (5))
    ((Leading)                    (6))
);

DEFINE_ENUM(EErrorCode,
    ((NoSuchSnapshot)             (600))
    ((NoSuchChangelog)            (601))
    ((InvalidEpoch)               (602))
    ((InvalidVersion)             (603))
    ((OutOfOrderMutations)        (609))
    ((InvalidSnapshotVersion)     (610))
);

DEFINE_ENUM(EPeerKind,
    (Leader)
    (Follower)
    (LeaderOrFollower)
);

////////////////////////////////////////////////////////////////////////////////

using NElection::TCellId;
using NElection::NullCellId;
using NElection::TPeerId;
using NElection::InvalidPeerId;
using NElection::TPeerPriority;
using NElection::TEpochId;

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
