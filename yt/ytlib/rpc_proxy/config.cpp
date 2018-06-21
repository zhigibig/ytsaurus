#include "config.h"

namespace NYT {
namespace NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

TConnectionConfig::TConnectionConfig()
{
    RegisterParameter("domain", Domain)
        .Default("yt.yandex-team.ru");
    RegisterParameter("cluster_url", ClusterUrl)
        .Default();
    RegisterParameter("proxy_role", ProxyRole)
        .Optional();
    RegisterParameter("addresses", Addresses)
        .Default();
    RegisterPostprocessor([this] {
        if (!ClusterUrl && Addresses.empty()) {
            THROW_ERROR_EXCEPTION("Either \"cluster_url\" or \"addresses\" must be specified");
        }

        if (ClusterUrl) {
            if (ClusterUrl->find('.') == TString::npos &&
                ClusterUrl->find(':') == TString::npos &&
                ClusterUrl->find("localhost") == TString::npos)
            {
                ClusterUrl = *ClusterUrl + ".yt.yandex.net";
            }

            if (!ClusterUrl->StartsWith("http://")) {
                ClusterUrl = "http://" + *ClusterUrl;
            }
        }
    });
    RegisterParameter("ping_period", PingPeriod)
        .Default(TDuration::Seconds(3));
    RegisterParameter("proxy_list_update_period", ProxyListUpdatePeriod)
        .Default(TDuration::Seconds(5));
    RegisterParameter("max_proxy_list_update_attempts", MaxProxyListUpdateAttempts)
        .Default(7);
    RegisterParameter("rpc_timeout", RpcTimeout)
        .Default(TDuration::Seconds(30));
    RegisterParameter("timestamp_provider_update_period", TimestampProviderUpdatePeriod)
        .Default(TDuration::Seconds(3));
    RegisterParameter("default_transaction_timeout", DefaultTransactionTimeout)
        .Default(TDuration::Seconds(15));
    RegisterParameter("default_ping_period", DefaultPingPeriod)
        .Default(TDuration::Seconds(5));
    RegisterParameter("bus_client", BusClient)
        .DefaultNew();
    RegisterParameter("http_client", HttpClient)
        .DefaultNew();
    // COMPAT(prime)
    RegisterParameter("send_legacy_user_ip", SendLegacyUserIP)
        .Default(true);
    // COMPAT(prime)
    RegisterParameter("discover_proxies_from_cypress", DiscoverProxiesFromCypress)
        .Default(true);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpcProxy
} // namespace NYT
