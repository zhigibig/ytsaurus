#pragma once

#include "public.h"

#include <ytlib/misc/error.h>

#include <ytlib/ytree/yson_serializable.h>

namespace NYT {
namespace NElection {

////////////////////////////////////////////////////////////////////////////////

struct TCellConfig
    : public TYsonSerializable
{
    //! RPC interface port number.
    int RpcPort;

    //! Master server addresses.
    std::vector<Stroka> Addresses;

    TCellConfig()
    {
        Register("rpc_port", RpcPort)
            .Default(9000);
        Register("addresses", Addresses)
            .NonEmpty();

        RegisterValidator([&] () {
            if (Addresses.size() % 2 != 1) {
                THROW_ERROR_EXCEPTION("Number of masters must be odd");
            }
        });
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TElectionManagerConfig
    : public TYsonSerializable
{
    TDuration VotingRoundInterval;
    TDuration RpcTimeout;
    TDuration FollowerPingInterval;
    TDuration FollowerPingTimeout;
    TDuration ReadyToFollowTimeout;
    TDuration PotentialFollowerTimeout;

    TElectionManagerConfig()
    {
        Register("voting_round_interval", VotingRoundInterval)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(100));
        Register("rpc_timeout", RpcTimeout)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(1000));
        Register("follower_ping_interval", FollowerPingInterval)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(1000));
        Register("follower_ping_timeout", FollowerPingTimeout)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(5000));
        Register("ready_to_follow_timeout", ReadyToFollowTimeout)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(5000));
        Register("potential_follower_timeout", PotentialFollowerTimeout)
            .GreaterThan(TDuration())
            .Default(TDuration::MilliSeconds(5000));
    }
};

typedef TIntrusivePtr<TElectionManagerConfig> TElectionManagerConfigPtr;

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
