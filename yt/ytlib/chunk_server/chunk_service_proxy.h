#pragma once

#include "common.h"
#include "chunk_service.pb.h"

#include <ytlib/rpc/service.h>
#include <ytlib/rpc/client.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

class TChunkServiceProxy
    : public NRpc::TProxyBase
{
public:
    typedef TIntrusivePtr<TChunkServiceProxy> TPtr;

    static Stroka GetServiceName()
    {
        return "ChunkManager";
    }

    DECLARE_ENUM(EErrorCode,
        ((NoSuchTransaction)(1))
        ((NoSuchHolder)(2))
        ((NoSuchChunk)(3))
        ((NoSuchChunkList)(4))
        ((NoSuchChunkTree)(5))
        ((NotEnoughHolders)(6))
    );

    TChunkServiceProxy(NRpc::IChannel* channel)
        : TProxyBase(channel, GetServiceName())
    { }

    DEFINE_RPC_PROXY_METHOD(NProto, RegisterHolder);
    DEFINE_RPC_PROXY_METHOD(NProto, HolderHeartbeat);
    DEFINE_RPC_PROXY_METHOD(NProto, CreateChunks);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
