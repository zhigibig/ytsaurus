#include "stdafx.h"
#include "serialize.h"

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

int GetCurrentSnapshotVersion()
{
    return 208;
}

bool ValidateSnapshotVersion(int version)
{
    return
        version == 124 ||
        version == 200 ||
        version == 201 ||
        version == 202 ||
        version == 203 ||
        version == 204 ||
        version == 205 ||
        version == 206 ||
        version == 207 ||
        version == 208;
}

////////////////////////////////////////////////////////////////////////////////

TLoadContext::TLoadContext(TBootstrap* bootstrap)
    : Bootstrap_(bootstrap)
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
