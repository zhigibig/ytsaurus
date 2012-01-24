#pragma once

#include "common.h"
#include "config.h"

#include <ytlib/rpc/channel_cache.h>

#include <ytlib/misc/configurable.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

class TCellManager
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TCellManager> TPtr;
    typedef TCellConfig TConfig;

    TCellManager(TConfig* config);

    TPeerId GetSelfId() const;
    i32 GetPeerCount() const;
    i32 GetQuorum() const;
    Stroka GetPeerAddress(TPeerId id) const;

    template <class TProxy>
    TAutoPtr<TProxy> GetMasterProxy(TPeerId id) const;

private:
    TConfig::TPtr Config;
    static NRpc::TChannelCache ChannelCache;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT

#define CELL_MANAGER_INL_H_
#include "cell_manager-inl.h"
#undef CELL_MANAGER_INL_H_
