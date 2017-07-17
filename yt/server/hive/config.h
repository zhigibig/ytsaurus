#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NHiveServer {

////////////////////////////////////////////////////////////////////////////////

class THiveManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent |Ping| requests to remote Hive Manager.
    TDuration PingPeriod;

    //! Interval between consequent idle (i.e. not carrying any payload) |PostMessages|
    //! requests to remote Hive Manager.
    TDuration IdlePostPeriod;

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
        RegisterParameter("idle_post_period", IdlePostPeriod)
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
            .Default((i64) 16 * MB);
    }
};

DEFINE_REFCOUNTED_TYPE(THiveManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TTransactionSupervisorConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration ParticipantProbationPeriod;
    TDuration RpcTimeout;
    TDuration ParticipantBackoffTime;

    TTransactionSupervisorConfig()
    {
        RegisterParameter("participant_probation_period", ParticipantProbationPeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("rpc_timeout", RpcTimeout)
            .Default(TDuration::Seconds(5));
        RegisterParameter("participant_backoff_time", ParticipantBackoffTime)
            .Default(TDuration::Seconds(5));
    }
};;

DEFINE_REFCOUNTED_TYPE(TTransactionSupervisorConfig)

////////////////////////////////////////////////////////////////////////////////

class TCellDirectorySynchronizerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Interval between consequent SyncCells requests to the primary Hive Manager.
    TDuration SyncPeriod;

    TCellDirectorySynchronizerConfig()
    {
        RegisterParameter("sync_period", SyncPeriod)
            .Default(TDuration::Seconds(3));
    }
};

DEFINE_REFCOUNTED_TYPE(TCellDirectorySynchronizerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHiveServer
} // namespace NYT
