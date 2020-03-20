#include "remote_timestamp_provider.h"
#include "batching_timestamp_provider.h"
#include "timestamp_provider_base.h"
#include "private.h"
#include "config.h"
#include "timestamp_service_proxy.h"

#include <yt/core/rpc/balancing_channel.h>
#include <yt/core/rpc/retrying_channel.h>

#include <yt/core/ytree/convert.h>
#include <yt/core/ytree/fluent.h>

namespace NYT::NTransactionClient {

using namespace NRpc;
using namespace NYTree;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

IChannelPtr CreateTimestampProviderChannel(
    TRemoteTimestampProviderConfigPtr config,
    IChannelFactoryPtr channelFactory)
{
    auto endpointDescription = TString("TimestampProvider@");
    auto endpointAttributes = ConvertToAttributes(BuildYsonStringFluently()
        .BeginMap()
            .Item("timestamp_provider").Value(true)
        .EndMap());
    auto channel = CreateBalancingChannel(
        config,
        channelFactory,
        endpointDescription,
        *endpointAttributes);
    channel = CreateRetryingChannel(
        config,
        channel);
    return channel;
}

IChannelPtr CreateTimestampProviderChannelFromAddresses(
    TRemoteTimestampProviderConfigPtr config,
    IChannelFactoryPtr channelFactory,
    const std::vector<TString>& discoveredAddresses)
{
    auto channelConfig = CloneYsonSerializable(config);
    if (!discoveredAddresses.empty()) {
        channelConfig->Addresses = discoveredAddresses;
    }
    return CreateTimestampProviderChannel(channelConfig, channelFactory);
}

////////////////////////////////////////////////////////////////////////////////

class TRemoteTimestampProvider
    : public TTimestampProviderBase
{
public:
    TRemoteTimestampProvider(
        IChannelPtr channel,
        TDuration defaultTimeout)
        : Proxy_(std::move(channel))
    {
        Proxy_.SetDefaultTimeout(defaultTimeout);
    }

private:
    TTimestampServiceProxy Proxy_;

    virtual TFuture<TTimestamp> DoGenerateTimestamps(int count) override
    {
        auto req = Proxy_.GenerateTimestamps();
        req->set_count(count);
        return req->Invoke().Apply(BIND([] (const TTimestampServiceProxy::TRspGenerateTimestampsPtr& rsp) {
            return static_cast<TTimestamp>(rsp->timestamp());
        }));
    }
};

////////////////////////////////////////////////////////////////////////////////

ITimestampProviderPtr CreateRemoteTimestampProvider(
    TRemoteTimestampProviderConfigPtr config,
    IChannelPtr channel)
{
    return New<TRemoteTimestampProvider>(std::move(channel), config->RpcTimeout);
}

ITimestampProviderPtr CreateBatchingRemoteTimestampProvider(
    TBatchingRemoteTimestampProviderConfigPtr config,
    IChannelPtr channel)
{
    auto underlying = CreateRemoteTimestampProvider(config, std::move(channel));
    auto wrapped = CreateBatchingTimestampProvider(std::move(underlying), config->UpdatePeriod, config->BatchPeriod);

    return wrapped;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionClient

