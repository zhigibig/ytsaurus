#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NHive {

////////////////////////////////////////////////////////////////////////////////

class THiveManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent |Ping| requests to remote Hive Manager.
    TDuration PingPeriod;

    //! Timeout for all RPC requests exchanged by Hive Managers.
    TDuration RpcTimeout;

    //! Timeout for Ping RPC requests.
    TDuration PingRpcTimeout;

    //! Timeout for Send RPC requests.
    TDuration SendRpcTimeout;

    //! Timeout for Send RPC requests.
    TDuration PostRpcTimeout;

    //! Maximum number of messages to send via a single |PostMessages| request.
    int MaxMessagesPerPost;

    //! Maximum number of bytes to send via a single |PostMessages| request.
    i64 MaxBytesPerPost;

    THiveManagerConfig()
    {
        RegisterParameter("ping_period", PingPeriod)
            .Default(TDuration::Seconds(15));
        RegisterParameter("ping_rpc_timeout", PingRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("send_rpc_timeout", SendRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("post_rpc_timeout", PostRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("max_messages_per_post", MaxMessagesPerPost)
            .Default(16384);
        RegisterParameter("max_bytes_per_post", MaxBytesPerPost)
            .Default((i64) 16 * 1024 * 1024);
    }
};

DEFINE_REFCOUNTED_TYPE(THiveManagerConfig)

class TCellDirectorySynchronizerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent SyncCells requests to the primary Hive Manager.
    TDuration SyncPeriod;

    TCellDirectorySynchronizerConfig()
    {
        RegisterParameter("sync_period", SyncPeriod)
            .Default(TDuration::Seconds(15));
    }
};

DEFINE_REFCOUNTED_TYPE(TCellDirectorySynchronizerConfig)

class TTransactionSupervisorConfig
    : public NYTree::TYsonSerializable
{
public:
    TTransactionSupervisorConfig()
    { }
};

DEFINE_REFCOUNTED_TYPE(TTransactionSupervisorConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHive
} // namespace NYT
