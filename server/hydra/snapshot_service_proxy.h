#pragma once

#include "public.h"

#include <yt/server/hydra/snapshot_service.pb.h>

#include <yt/core/rpc/client.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

class TSnapshotServiceProxy
    : public NRpc::TProxyBase
{
public:
    DEFINE_RPC_PROXY(TSnapshotServiceProxy, RPC_PROXY_DESC(SnapshotService));

    DEFINE_RPC_PROXY_METHOD(NProto, ReadSnapshot);
    DEFINE_RPC_PROXY_METHOD(NProto, LookupSnapshot);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
