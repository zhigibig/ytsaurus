#include "stdafx.h"
#include "roaming_channel.h"
#include "client.h"
#include "channel_detail.h"

#include <core/actions/future.h>

namespace NYT {
namespace NRpc {

using namespace NBus;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TRoamingChannel
    : public IChannel
{
public:
    explicit TRoamingChannel(IRoamingChannelProviderPtr provider)
        : Provider_(std::move(provider))
    { }

    virtual TNullable<TDuration> GetDefaultTimeout() const override
    {
        return DefaultTimeout_;
    }

    virtual void SetDefaultTimeout(const TNullable<TDuration>& timeout) override
    {
        DefaultTimeout_ = timeout;
    }

    virtual Stroka GetEndpointTextDescription() const override
    {
        return Provider_->GetEndpointTextDescription();
    }

    virtual TYsonString GetEndpointYsonDescription() const override
    {
        return Provider_->GetEndpointYsonDescription();
    }

    virtual IClientRequestControlPtr Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        TNullable<TDuration> timeout,
        bool requestAck) override
    {
        YASSERT(request);
        YASSERT(responseHandler);

        auto actualTimeout = timeout ? timeout : DefaultTimeout_;

        auto asyncChannel = Provider_->GetChannel(request->GetService());

        // NB: Optimize for the typical case of sync channel acquisition.
        auto channelOrError = asyncChannel.TryGet();
        if (channelOrError) {
            if (channelOrError->IsOK()) {
                const auto& channel = channelOrError->Value();
                return channel->Send(
                    std::move(request),
                    std::move(responseHandler),
                    actualTimeout,
                    requestAck);
            } else {
                responseHandler->HandleError(*channelOrError);
                return New<TClientRequestControlThunk>();
            }
        }

        auto requestControlThunk = New<TClientRequestControlThunk>();

        asyncChannel.Subscribe(
            BIND(
                &TRoamingChannel::OnGotChannel,
                MakeStrong(this),
                request,
                responseHandler,
                actualTimeout,
                requestAck,
                requestControlThunk));

        return requestControlThunk;
    }

    virtual TFuture<void> Terminate(const TError& error) override
    {
        return Provider_->Terminate(error);
    }

private:
    const IRoamingChannelProviderPtr Provider_;

    TNullable<TDuration> DefaultTimeout_;


    void OnGotChannel(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        TNullable<TDuration> timeout,
        bool requestAck,
        TClientRequestControlThunkPtr requestControlThunk,
        const TErrorOr<IChannelPtr>& result)
    {
        if (!result.IsOK()) {
            responseHandler->HandleError(result);
            return;
        }

        const auto& channel = result.Value();
        auto requestControl = channel->Send(
            std::move(request),
            std::move(responseHandler),
            timeout,
            requestAck);
        requestControlThunk->SetUnderlying(std::move(requestControl));
    }

};

IChannelPtr CreateRoamingChannel(IRoamingChannelProviderPtr provider)
{
    YCHECK(provider);

    return New<TRoamingChannel>(std::move(provider));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
