#pragma once

#include "public.h"

#include <yt/core/actions/public.h>

#include <yt/core/misc/error.h>

#include <yt/core/rpc/public.h>
#include <yt/core/rpc/rpc.pb.h>

#include <yt/core/tracing/trace_context.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

//! Returns a wrapper that sets the timeout for every request (unless it is given
//! explicitly in the request itself).
IChannelPtr CreateDefaultTimeoutChannel(
    IChannelPtr underlyingChannel,
    TDuration timeout);
IChannelFactoryPtr CreateDefaultTimeoutChannelFactory(
    IChannelFactoryPtr underlyingFactory,
    TDuration timeout);

//! Returns a wrapper that sets "authenticated_user" attribute in every request.
IChannelPtr CreateAuthenticatedChannel(
    IChannelPtr underlyingChannel,
    const Stroka& user);
IChannelFactoryPtr CreateAuthenticatedChannelFactory(
    IChannelFactoryPtr underlyingFactory,
    const Stroka& user);

//! Returns a wrapper that sets realm id in every request.
IChannelPtr CreateRealmChannel(
    IChannelPtr underlyingChannel,
    const TRealmId& realmId);
IChannelFactoryPtr CreateRealmChannelFactory(
    IChannelFactoryPtr underlyingFactory,
    const TRealmId& realmId);

//! Returns a wrapper that informs about channel failures.
/*!
 *  Channel failures are being detected via NRpc::IsChannelFailureError.
 */
IChannelPtr CreateFailureDetectingChannel(
    IChannelPtr underlyingChannel,
    TCallback<void(IChannelPtr)> onFailure);

//! Returns the trace context associated with the request.
//! If no trace context is attached, returns a disabled context.
NTracing::TTraceContext GetTraceContext(const NProto::TRequestHeader& header);

//! Attaches a given trace context to the request.
void SetTraceContext(
    NProto::TRequestHeader* header,
    const NTracing::TTraceContext& context);

//! Generates a random mutation id.
TMutationId GenerateMutationId();

//! Returns the mutation id associated with the context.
TMutationId GetMutationId(const IServiceContextPtr& context);

//! Returns the mutation id associated with the request.
TMutationId GetMutationId(const NProto::TRequestHeader& header);

void GenerateMutationId(const IClientRequestPtr& request);
void SetMutationId(NProto::TRequestHeader* header, const TMutationId& id, bool retry);
void SetMutationId(const IClientRequestPtr& request, const TMutationId& id, bool retry);
void SetOrGenerateMutationId(const IClientRequestPtr& request, const TMutationId& id, bool retry);

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
