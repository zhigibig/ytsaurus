#pragma once

#include "common.h"
#include "channel.h"
#include "client.h"

#include "../misc/property.h"

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

//! Implements a simple retry policy.
/*!
 *  If a remote call fails at RPC level (see #NRpc::EErrorCode::IsRpcError)
 *  it is retried a given number of times with a given back off time.
 *  
 *  If the request is still failing, then EErrorCode::Unavailable is returned.
 */  
class TRetriableChannel
    : public IChannel
{
    //! An underlying channel.
    DECLARE_BYVAL_RO_PROPERTY(UnderlyingChannel, IChannel::TPtr);

    //! A interval between successive attempts.
    DECLARE_BYVAL_RO_PROPERTY(BackoffTime, TDuration);

    //! Maximum number of retry attempts.
    DECLARE_BYVAL_RO_PROPERTY(RetryCount, int);

public:
    typedef TIntrusivePtr<TRetriableChannel> TPtr;

    //! Initializes an instance.
    /*!
     *  For more information about parameters see public properties.
     */
    TRetriableChannel(
        IChannel* underlyingChannel, 
        TDuration backoffTime, 
        int retryCount);

    TFuture<TError>::TPtr Send(
        IClientRequest::TPtr request, 
        IClientResponseHandler::TPtr responseHandler, 
        TDuration timeout);

    void Terminate();

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT