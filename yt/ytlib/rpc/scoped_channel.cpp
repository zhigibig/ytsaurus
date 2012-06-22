#include "stdafx.h"
#include "scoped_channel.h"
#include "client.h"

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

namespace {

class TScopedChannel
    : public IChannel
{
public:
    explicit TScopedChannel(IChannelPtr underlyingChannel);

    TNullable<TDuration> GetDefaultTimeout() const OVERRIDE;

    void Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        TNullable<TDuration> timeout) OVERRIDE;

    void Terminate(const TError& error) OVERRIDE;

    void OnRequestCompleted();

private:
    IChannelPtr UnderlyingChannel;

    TSpinLock SpinLock;
    bool Terminated;
    TError TerminationError;
    int OutstandingRequestCount;
    TPromise<void> OutstandingRequestsCompleted;

};

typedef TIntrusivePtr<TScopedChannel> TScopedChannelPtr;

class TScopedResponseHandler
    : public IClientResponseHandler
{
public:
    TScopedResponseHandler(
        IClientResponseHandlerPtr underlyingHandler,
        TScopedChannelPtr channel)
        : UnderlyingHandler(MoveRV(underlyingHandler))
        , Channel(channel)
    { }

    void OnAcknowledgement() OVERRIDE
    {
        UnderlyingHandler->OnAcknowledgement();
    }
    
    void OnResponse(NBus::IMessagePtr message) OVERRIDE
    {
        UnderlyingHandler->OnResponse(MoveRV(message));
        Channel->OnRequestCompleted();
    }

    void OnError(const TError& error) OVERRIDE
    {
        UnderlyingHandler->OnError(error);
        Channel->OnRequestCompleted();
    }

private:
    IClientResponseHandlerPtr UnderlyingHandler;
    TScopedChannelPtr Channel;

};

TScopedChannel::TScopedChannel(IChannelPtr underlyingChannel)
    : UnderlyingChannel(MoveRV(underlyingChannel))
    , Terminated(false)
    , OutstandingRequestCount(0)
    , OutstandingRequestsCompleted(NewPromise<void>())
{ }

TNullable<TDuration> TScopedChannel::GetDefaultTimeout() const
{
    return UnderlyingChannel->GetDefaultTimeout();
}

void TScopedChannel::Send(
    IClientRequestPtr request,
    IClientResponseHandlerPtr responseHandler,
    TNullable<TDuration> timeout)
{
    {
        TGuard<TSpinLock> guard(SpinLock);
        if (Terminated) {
            guard.Release();
            responseHandler->OnError(TerminationError);
            return;
        }
        ++OutstandingRequestCount;
    }
    auto scopedHandler = New<TScopedResponseHandler>(MoveRV(responseHandler), this);
    UnderlyingChannel->Send(request, MoveRV(scopedHandler), timeout);
}

void TScopedChannel::Terminate(const TError& error)
{
    TGuard<TSpinLock> guard(SpinLock);
    if (Terminated) {
        return;
    }
    Terminated = true;
    if (OutstandingRequestCount == 0) {
        return;
    }
    guard.Release();
    OutstandingRequestsCompleted.Get();
}

void TScopedChannel::OnRequestCompleted()
{
    TGuard<TSpinLock> guard(SpinLock);
    --OutstandingRequestCount;
    if (Terminated && OutstandingRequestCount == 0) {
        guard.Release();
        OutstandingRequestsCompleted.Set();
    }
}

} // namespace

IChannelPtr CreateScopedChannel(IChannelPtr underlyingChannel)
{
    YASSERT(underlyingChannel);
    return New<TScopedChannel>(underlyingChannel);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
