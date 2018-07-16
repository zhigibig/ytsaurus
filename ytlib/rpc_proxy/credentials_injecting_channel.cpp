#include "credentials_injecting_channel.h"

#include <yt/core/rpc/client.h>
#include <yt/core/rpc/channel_detail.h>
#include <yt/core/rpc/proto/rpc.pb.h>

namespace NYT {
namespace NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TUserInjectingChannel
    : public TChannelWrapper
{
public:
    TUserInjectingChannel(IChannelPtr underlyingChannel, const TString& user)
        : TChannelWrapper(std::move(underlyingChannel))
        , User_(user)
    { }

    virtual IClientRequestControlPtr Send(
        IClientRequestPtr request,
        IClientResponseHandlerPtr responseHandler,
        const TSendOptions& options) override
    {
        DoInject(request);

        return TChannelWrapper::Send(
            std::move(request),
            std::move(responseHandler),
            options);
    }

protected:
    virtual void DoInject(const IClientRequestPtr& request)
    {
        request->SetUser(User_);
        request->SetUserAgent("yt-cpp-rpc-client/1.0");
    }

private:
    const TString User_;
};

IChannelPtr CreateUserInjectingChannel(IChannelPtr underlyingChannel, const TString& user)
{
    YCHECK(underlyingChannel);
    return New<TUserInjectingChannel>(std::move(underlyingChannel), user);
}

////////////////////////////////////////////////////////////////////////////////

class TTokenInjectingChannel
    : public TUserInjectingChannel
{
public:
    TTokenInjectingChannel(
        IChannelPtr underlyingChannel,
        const TString& user,
        const TString& token,
        // COMPAT(babenko)
        const TString& userIP)
        : TUserInjectingChannel(std::move(underlyingChannel), user)
        , Token_(token)
        // COMPAT(babenko)
        , UserIP_(userIP)
    { }

protected:
    virtual void DoInject(const IClientRequestPtr& request) override
    {
        TUserInjectingChannel::DoInject(request);

        auto* ext = request->Header().MutableExtension(NRpc::NProto::TCredentialsExt::credentials_ext);
        ext->set_token(Token_);
        // COMPAT(babenko)
        ext->set_user_ip(UserIP_);
    }

private:
    const TString Token_;
    const TString UserIP_;
};

IChannelPtr CreateTokenInjectingChannel(
    IChannelPtr underlyingChannel,
    const TString& user,
    const TString& token,
    // COMPAT(babenko)
    const TString& userIP)
{
    YCHECK(underlyingChannel);
    return New<TTokenInjectingChannel>(
        std::move(underlyingChannel),
        user,
        token,
        // COMPAT(babenko)
        userIP);
}

////////////////////////////////////////////////////////////////////////////////

class TCookieInjectingChannel
    : public TUserInjectingChannel
{
public:
    TCookieInjectingChannel(
        IChannelPtr underlyingChannel,
        const TString& user,
        const TString& sessionId,
        const TString& sslSessionId)
        : TUserInjectingChannel(std::move(underlyingChannel), user)
        , SessionId_(sessionId)
        , SslSessionId_(sslSessionId)
    { }

protected:
    virtual void DoInject(const IClientRequestPtr& request) override
    {
        TUserInjectingChannel::DoInject(request);

        auto* ext = request->Header().MutableExtension(NRpc::NProto::TCredentialsExt::credentials_ext);
        ext->set_session_id(SessionId_);
        ext->set_ssl_session_id(SslSessionId_);
    }

private:
    const TString SessionId_;
    const TString SslSessionId_;
};

IChannelPtr CreateCookieInjectingChannel(
    IChannelPtr underlyingChannel,
    const TString& user,
    const TString& sessionId,
    const TString& sslSessionId)
{
    YCHECK(underlyingChannel);
    return New<TCookieInjectingChannel>(
        std::move(underlyingChannel),
        user,
        sessionId,
        sslSessionId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpcProxy
} // namespace NYT
