#pragma once

#include "public.h"

#include <ytlib/misc/nullable.h>
#include <ytlib/misc/property.h>
#include <ytlib/misc/error.h>
#include <ytlib/bus/client.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

/*!
 * \note Thread affinity: any.
 */
struct IChannel
    : public virtual TRefCounted
{
    //! Gets default timeout.
    virtual TNullable<TDuration> GetDefaultTimeout() const = 0;

    //! Sends a request via the channel.
    /*!
     *  \param request A request to send.
     *  \param responseHandler An object that will handle a response.
     *  \param timeout Request processing timeout.
     */
    virtual void Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        TNullable<TDuration> timeout) = 0;

    //! Shuts down the channel.
    /*!
     *  It is safe to call this method multiple times.
     *  After the first call the instance is no longer usable.
     */
    virtual void Terminate(const TError& error = TError("Channel terminated")) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
