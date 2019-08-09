#pragma once

#include "public.h"

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

NHydra::TReign GetCurrentReign();
bool ValidateSnapshotReign(NHydra::TReign);
NHydra::EFinalRecoveryAction GetActionToRecoverFromReign(NHydra::TReign reign);

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETabletReign,
    ((LongForgottenBase)             (100008)) // aozeritsky
    ((SaveLastCommitTimestamp)       (100009)) // savrus
    ((AddTabletCellLifeState)        (100010)) // savrus
    ((SerializeChunkReadRange)       (100011)) // ifsmirnov
    ((SafeReplicatedLogSchema)       (100012)) // savrus
    ((BulkInsert)                    (100013)) // savrus
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
