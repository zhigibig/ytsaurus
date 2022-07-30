#include "public.h"

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

struct IHunkTabletScanner
    : public TRefCounted
{
    virtual void Scan(THunkTablet* tablet) = 0;
};

DEFINE_REFCOUNTED_TYPE(IHunkTabletScanner);

////////////////////////////////////////////////////////////////////////////////

IHunkTabletScannerPtr CreateHunkTabletScanner(
    IBootstrap* bootstrap,
    ITabletSlotPtr tabletSlot);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
