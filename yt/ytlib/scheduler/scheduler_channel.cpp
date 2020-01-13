#include "scheduler_channel.h"
#include "config.h"

#include <yt/ytlib/object_client/object_service_proxy.h>
#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/core/bus/tcp/config.h>
#include <yt/core/bus/tcp/client.h>

#include <yt/core/rpc/bus/channel.h>
#include <yt/core/rpc/retrying_channel.h>
#include <yt/core/rpc/roaming_channel.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/ypath_proxy.h>

namespace NYT::NScheduler {

using namespace NBus;
using namespace NRpc;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;
using namespace NNodeTrackerClient;

////////////////////////////////////////////////////////////////////////////////

class TSchedulerChannelProvider
    : public IRoamingChannelProvider
{
public:
    TSchedulerChannelProvider(
        TSchedulerConnectionConfigPtr config,
        IChannelFactoryPtr channelFactory,
        IChannelPtr masterChannel,
        const TNetworkPreferenceList& networks)
        : Config_(std::move(config))
        , ChannelFactory_(std::move(channelFactory))
        , MasterChannel_(std::move(masterChannel))
        , Networks_(networks)
        , EndpointDescription_(Format("Scheduler@%v",
            MasterChannel_->GetEndpointDescription()))
        , EndpointAttributes_(ConvertToAttributes(BuildYsonStringFluently()
            .BeginMap()
                .Item("scheduler").Value(true)
                .Items(MasterChannel_->GetEndpointAttributes())
            .EndMap()))
    { }

    virtual const TString& GetEndpointDescription() const override
    {
        return EndpointDescription_;
    }

    virtual const NYTree::IAttributeDictionary& GetEndpointAttributes() const override
    {
        return *EndpointAttributes_;
    }

    virtual TNetworkId GetNetworkId() const override
    {
        TGuard<TSpinLock> guard(SpinLock_);
        if (CachedChannel_) {
            return CachedChannel_->GetNetworkId();
        } else {
            return DefaultNetworkId;
        }

    }

    virtual TFuture<IChannelPtr> GetChannel(const IClientRequestPtr& /*request*/) override
    {
        {
            TGuard<TSpinLock> guard(SpinLock_);
            if (CachedChannel_) {
                return MakeFuture(CachedChannel_);
            }
        }

        TObjectServiceProxy proxy(MasterChannel_);
        auto batchReq = proxy.ExecuteBatch();
        batchReq->AddRequest(TYPathProxy::Get("//sys/scheduler/@addresses"));
        return batchReq->Invoke()
            .Apply(BIND([=, this_ = MakeStrong(this)] (const TObjectServiceProxy::TRspExecuteBatchPtr& batchRsp) -> IChannelPtr {
                auto rsp = batchRsp->GetResponse<TYPathProxy::TRspGet>(0);
                if (rsp.FindMatching(NYT::NYTree::EErrorCode::ResolveError)) {
                    THROW_ERROR_EXCEPTION("No scheduler is configured");
                }

                THROW_ERROR_EXCEPTION_IF_FAILED(rsp, "Cannot determine scheduler address");

                auto addresses = ConvertTo<TAddressMap>(TYsonString(rsp.Value()->value()));

                auto channel = ChannelFactory_->CreateChannel(GetAddressWithNetworkOrThrow(addresses, Networks_));
                channel = CreateFailureDetectingChannel(
                    channel,
                    Config_->RpcAcknowledgementTimeout,
                    BIND(&TSchedulerChannelProvider::OnChannelFailed, MakeWeak(this)));

                {
                    TGuard<TSpinLock> guard(SpinLock_);
                    CachedChannel_ = channel;
                }

                return channel;
            }));
    }

    virtual TFuture<void> Terminate(const TError& error) override
    {
        TGuard<TSpinLock> guard(SpinLock_);
        return CachedChannel_ ? CachedChannel_->Terminate(error) : VoidFuture;
    }

private:
    const TSchedulerConnectionConfigPtr Config_;
    const IChannelFactoryPtr ChannelFactory_;
    const IChannelPtr MasterChannel_;
    const TNetworkPreferenceList Networks_;

    const TString EndpointDescription_;
    const std::unique_ptr<IAttributeDictionary> EndpointAttributes_;

    TSpinLock SpinLock_;
    IChannelPtr CachedChannel_;


    void OnChannelFailed(const IChannelPtr& channel, const TError& /*error*/)
    {
        TGuard<TSpinLock> guard(SpinLock_);
        if (CachedChannel_ == channel) {
            CachedChannel_.Reset();
        }
    }

};

IChannelPtr CreateSchedulerChannel(
    TSchedulerConnectionConfigPtr config,
    IChannelFactoryPtr channelFactory,
    IChannelPtr masterChannel,
    const TNetworkPreferenceList& networks)
{
    YT_VERIFY(config);
    YT_VERIFY(channelFactory);
    YT_VERIFY(masterChannel);

    auto channelProvider = New<TSchedulerChannelProvider>(config, channelFactory, masterChannel, networks);
    auto channel = CreateRoamingChannel(channelProvider);

    channel = CreateRetryingChannel(config, channel);

    channel = CreateDefaultTimeoutChannel(channel, config->RpcTimeout);

    return channel;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
