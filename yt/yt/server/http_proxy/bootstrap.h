#pragma once

#include "public.h"

#include <yt/yt/server/http_proxy/clickhouse/public.h>

#include <yt/yt/server/lib/chunk_pools/public.h>

#include <yt/yt/server/lib/zookeeper/public.h>

#include <yt/yt/library/auth_server/public.h>

#include <yt/yt/ytlib/api/public.h>
#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/library/monitoring/public.h>

#include <yt/yt/client/driver/public.h>

#include <yt/yt/core/bus/public.h>

#include <yt/yt/core/actions/public.h>

#include <yt/yt/core/concurrency/public.h>

#include <yt/yt/core/misc/atomic_object.h>
#include <yt/yt/core/misc/public.h>

#include <yt/yt/core/rpc/public.h>

#include <yt/yt/core/http/public.h>

#include <yt/yt/core/http/http.h>

#include <yt/yt/core/https/public.h>

#include <yt/yt/core/ytree/public.h>

namespace NYT::NHttpProxy {

////////////////////////////////////////////////////////////////////////////////

class TBootstrap
    : public NHttp::IHttpHandler
{
public:
    TBootstrap(TProxyConfigPtr config, NYTree::INodePtr configNode);
    ~TBootstrap();

    void Run();

    const IInvokerPtr& GetControlInvoker() const;

    const TProxyConfigPtr& GetConfig() const;
    TProxyDynamicConfigPtr GetDynamicConfig() const;
    const NApi::IClientPtr& GetRootClient() const;
    const NApi::NNative::IConnectionPtr& GetNativeConnection() const;
    const NDriver::IDriverPtr& GetDriverV3() const;
    const NDriver::IDriverPtr& GetDriverV4() const;
    const TCoordinatorPtr& GetCoordinator() const;
    const IAccessCheckerPtr& GetAccessChecker() const;
    const TCompositeHttpAuthenticatorPtr& GetHttpAuthenticator() const;
    const NAuth::TAuthenticationManagerPtr& GetAuthenticationManager() const;
    const NAuth::ITokenAuthenticatorPtr& GetTokenAuthenticator() const;
    const NAuth::ICookieAuthenticatorPtr& GetCookieAuthenticator() const;
    const IDynamicConfigManagerPtr& GetDynamicConfigManager() const;
    const NConcurrency::IPollerPtr& GetPoller() const;
    const TApiPtr& GetApi() const;

    void HandleRequest(
        const NHttp::IRequestPtr& req,
        const NHttp::IResponseWriterPtr& rsp) override;

private:
    const TProxyConfigPtr Config_;
    const NYTree::INodePtr ConfigNode_;
    const TInstant StartTime_ = TInstant::Now();

    TAtomicObject<TProxyDynamicConfigPtr> DynamicConfig_ = New<TProxyDynamicConfig>();

    NConcurrency::TActionQueuePtr Control_;
    NConcurrency::IPollerPtr Poller_;
    NConcurrency::IPollerPtr Acceptor_;

    NMonitoring::TMonitoringManagerPtr MonitoringManager_;
    NHttp::IServerPtr MonitoringServer_;

    NApi::NNative::IConnectionPtr Connection_;
    NApi::IClientPtr RootClient_;
    NApi::NNative::IClientPtr NativeClient_;
    NDriver::IDriverPtr DriverV3_;
    NDriver::IDriverPtr DriverV4_;

    NAuth::TAuthenticationManagerPtr AuthenticationManager_;
    NAuth::TAuthenticationManagerPtr TvmOnlyAuthenticationManager_;
    TCompositeHttpAuthenticatorPtr HttpAuthenticator_;

    IDynamicConfigManagerPtr DynamicConfigManager_;

    NRpc::IServerPtr RpcServer_;

    NHttp::IServerPtr ApiHttpServer_;
    NHttp::IServerPtr ApiHttpsServer_;
    NHttp::IServerPtr TvmOnlyApiHttpServer_;
    NHttp::IServerPtr TvmOnlyApiHttpsServer_;
    TApiPtr Api_;

    NClickHouse::TClickHouseHandlerPtr ClickHouseHandler_;

    // Zookeeper stuff.
    NConcurrency::TActionQueuePtr ZookeeperQueue_;
    NZookeeper::IClientPtr ZookeeperClient_;
    NZookeeper::IDriverPtr ZookeeperDriver_;
    NZookeeper::ISessionManagerPtr ZookeeperSessionManager_;
    NZookeeper::IServerPtr ZookeeperServer_;

    TCoordinatorPtr Coordinator_;
    THostsHandlerPtr HostsHandler_;
    TPingHandlerPtr PingHandler_;
    TDiscoverVersionsHandlerPtr DiscoverVersionsHandlerV2_;
    IAccessCheckerPtr AccessChecker_;

    ICoreDumperPtr CoreDumper_;

    void RegisterRoutes(const NHttp::IServerPtr& server);
    NHttp::IHttpHandlerPtr AllowCors(NHttp::IHttpHandlerPtr nextHandler) const;

    void SetupClients();

    void OnDynamicConfigChanged(
        const TProxyDynamicConfigPtr& /*oldConfig*/,
        const TProxyDynamicConfigPtr& newConfig);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttpProxy
