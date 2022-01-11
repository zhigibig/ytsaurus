#pragma once

#include "public.h"

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

struct IBackupManager
    : public virtual TRefCounted
{
    virtual void Initialize() = 0;
};

DEFINE_REFCOUNTED_TYPE(IBackupManager)

////////////////////////////////////////////////////////////////////////////////

IBackupManagerPtr CreateBackupManager(ITabletSlotPtr slot, IBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
