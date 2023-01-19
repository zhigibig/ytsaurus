#include "discovery_service.h"

#include "bootstrap.h"
#include "config.h"
#include "private.h"

#include <yt/yt/server/lib/rpc_proxy/proxy_coordinator.h>

#include <yt/yt/server/lib/cypress_registrar/cypress_registrar.h>
#include <yt/yt/server/lib/cypress_registrar/config.h>

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/config.h>
#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/rpc_helpers.h>

#include <yt/yt/ytlib/object_client/object_service_proxy.h>

#include <yt/yt/ytlib/api/native/connection.h>

#include <yt/yt/client/api/rpc_proxy/address_helpers.h>
#include <yt/yt/client/api/rpc_proxy/discovery_service_proxy.h>

#include <yt/yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/ytlib/security_client/public.h>

#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/net/address.h>
#include <yt/yt/core/net/local_address.h>

#include <yt/yt/core/ytree/convert.h>
#include <yt/yt/core/ytree/helpers.h>

#include <yt/yt/core/utilex/random.h>

#include <yt/yt/core/rpc/service_detail.h>

#include <yt/yt/library/profiling/solomon/registry.h>

#include <yt/yt/build/build.h>

namespace NYT::NRpcProxy {

using namespace NApi;
using namespace NApi::NRpcProxy;
using namespace NYson;
using namespace NYTree;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NCypressClient;
using namespace NRpc;
using namespace NNet;
using namespace NApi::NNative;

using NYT::ToProto;

////////////////////////////////////////////////////////////////////////////////

static const TString ExpirationTimeAttributeName = "expiration_time";
static const TString VersionAttributeName = "version";
static const TString StartTimeAttributeName = "start_time";
static const TString AnnotationsAttributeName = "annotations";
static const TString AddressesAttributeName = "addresses";

////////////////////////////////////////////////////////////////////////////////

namespace {

const TServiceDescriptor& GetDescriptor()
{
    static const auto descriptor = TServiceDescriptor(DiscoveryServiceName)
        .SetProtocolVersion({0, 0});
    return descriptor;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TDiscoveryService
    : public TServiceBase
{
public:
    TDiscoveryService(
        TBootstrap* bootstrap)
        : TServiceBase(
            bootstrap->GetWorkerInvoker(),
            GetDescriptor(),
            RpcProxyLogger)
        , Bootstrap_(bootstrap)
        , Config_(bootstrap->GetConfig()->DiscoveryService)
        , Coordinator_(bootstrap->GetProxyCoordinator())
        , RootClient_(bootstrap->GetNativeClient())
        , ProxyPath_(RpcProxiesPath + "/" + BuildServiceAddress(
            GetLocalHostName(),
            Bootstrap_->GetConfig()->RpcPort))
        , AliveUpdateExecutor_(New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TDiscoveryService::OnPeriodicEvent, MakeWeak(this), &TDiscoveryService::UpdateLiveness),
            TPeriodicExecutorOptions::WithJitter(Config_->LivenessUpdatePeriod)))
        , ProxyUpdateExecutor_(New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TDiscoveryService::OnPeriodicEvent, MakeWeak(this), &TDiscoveryService::UpdateProxies),
            TPeriodicExecutorOptions::WithJitter(Config_->ProxyUpdatePeriod)))
        , GrpcPort_(GetGrpcPort())
        , GrpcProxyPath_(BuildGrpcProxyPath())
    {
        for (const auto& descriptor : GetProxyDescriptors()) {
            TCypressRegistrarOptions options{
                .RootPath = descriptor.CypressPath,
                .OrchidRemoteAddresses = Bootstrap_->GetLocalAddresses(),
                .CreateAliveChild = true,
                .EnableImplicitInitialization = false,
                .AttributesOnStart = BuildAttributeDictionaryFluently()
                    .Item(VersionAttributeName).Value(GetVersion())
                    .Item(StartTimeAttributeName).Value(TInstant::Now())
                    .Item(AnnotationsAttributeName).Value(Bootstrap_->GetConfig()->CypressAnnotations)
                    .Item(AddressesAttributeName).Value(descriptor.Addresses)
                    .Finish()
            };
            CypressRegistrars_.push_back(CreateCypressRegistrar(
                std::move(options),
                Config_->CypressRegistrar,
                RootClient_,
                Bootstrap_->GetControlInvoker()));
        }

        AliveUpdateExecutor_->Start();
        ProxyUpdateExecutor_->Start();

        RegisterMethod(RPC_SERVICE_METHOD_DESC(DiscoverProxies));
    }

private:
    TBootstrap* const Bootstrap_;
    const TDiscoveryServiceConfigPtr Config_;
    const IProxyCoordinatorPtr Coordinator_;
    const NNative::IClientPtr RootClient_;
    const TString ProxyPath_;
    const TPeriodicExecutorPtr AliveUpdateExecutor_;
    const TPeriodicExecutorPtr ProxyUpdateExecutor_;
    const std::optional<int> GrpcPort_;
    const std::optional<TString> GrpcProxyPath_;
    TCompactVector<ICypressRegistrarPtr, 2> CypressRegistrars_;

    TInstant LastSuccessTimestamp_ = Now();

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, ProxySpinLock_);

    struct TProxy
    {
        TProxyAddressMap Addresses;
        TString Role;
    };

    std::vector<TProxy> AvailableProxies_;

    bool Initialized_ = false;

    std::optional<int> GetGrpcPort()
    {
        const auto& grpcServerConfig = Bootstrap_->GetConfig()->GrpcServer;
        if (!grpcServerConfig) {
            return std::nullopt;
        }

        const auto& addresses = grpcServerConfig->Addresses;
        YT_VERIFY(addresses.size() == 1);

        int port;
        ParseServiceAddress(addresses[0]->Address, nullptr, &port);

        return port;
    }

    std::optional<TString> BuildGrpcProxyPath()
    {
        if (!GrpcPort_) {
            return std::nullopt;
        }

        return GrpcProxiesPath + "/" + BuildServiceAddress(GetLocalHostName(), *GrpcPort_);
    }

    struct TProxyDescriptor
    {
        TProxyAddressMap Addresses;
        TString CypressPath;
    };

    std::vector<TProxyDescriptor> GetProxyDescriptors() const
    {
        const auto& config = Bootstrap_->GetConfig();

        auto proxyAddressMap = TProxyAddressMap{
            {EAddressType::InternalRpc, GetLocalAddresses(config->Addresses, config->RpcPort)},
            {EAddressType::MonitoringHttp, GetLocalAddresses(config->Addresses, config->MonitoringPort)}
        };

        if (config->TvmOnlyAuth && config->TvmOnlyRpcPort) {
            auto addresses = GetLocalAddresses(config->Addresses, config->TvmOnlyRpcPort);
            proxyAddressMap.emplace(EAddressType::TvmOnlyInternalRpc, addresses);
        }

        std::vector<TProxyDescriptor> descriptors = {{proxyAddressMap, ProxyPath_}};
        if (GrpcProxyPath_) {
            auto grpcProxyAddressMap = TProxyAddressMap{
                {EAddressType::InternalRpc, GetLocalAddresses({}, *GrpcPort_)}
            };
            descriptors.push_back({grpcProxyAddressMap, *GrpcProxyPath_});
        }

        return descriptors;
    }

    std::vector<TString> GetCypressPaths() const
    {
        std::vector<TString> paths = {ProxyPath_};
        if (GrpcProxyPath_) {
            paths.push_back(*GrpcProxyPath_);
        }
        return paths;
    }

    template <typename T>
    TYsonString ConvertToYsonStringNestingLimited(const T& value)
    {
        const auto nestingLevelLimit = RootClient_
            ->GetNativeConnection()
            ->GetConfig()
            ->CypressWriteYsonNestingLevelLimit;
        return NYson::ConvertToYsonStringNestingLimited(value, nestingLevelLimit);
    }

    void CreateProxyNode()
    {
        try {
            for (const auto& registrar : CypressRegistrars_) {
                WaitFor(registrar->CreateNodes())
                    .ThrowOnError();
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error creating proxy node %Qv", ProxyPath_)
                << ex;
        }

        YT_LOG_INFO("Proxy node created (Path: %v)", ProxyPath_);
    }

    bool IsAvailable() const
    {
        return Now() - LastSuccessTimestamp_ < Config_->AvailabilityPeriod;
    }

    void OnPeriodicEvent(void (TDiscoveryService::*action)())
    {
        TDuration backoffDuration;
        while (true) {
            try {
                (this->*action)();
                return;
            } catch (const std::exception& ex) {
                backoffDuration = Min(backoffDuration + RandomDuration(Max(backoffDuration, Config_->LivenessUpdatePeriod)),
                    Config_->BackoffPeriod);
                YT_LOG_WARNING(ex, "Failed to perform update, backing off (Duration: %v)", backoffDuration);
                if (!IsAvailable() && Coordinator_->SetAvailableState(false)) {
                    Initialized_ = false;
                    YT_LOG_WARNING("Connectivity lost");
                }
                TDelayedExecutor::WaitForDuration(backoffDuration);
            }
        }
    }

    void UpdateLiveness()
    {
        if (!Initialized_) {
            CreateProxyNode();
            Initialized_ = true;
        }

        try {
            for (const auto& registrar : CypressRegistrars_) {
                WaitFor(registrar->UpdateNodes())
                    .ThrowOnError();
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error updating proxy liveness")
                << ex;
        }

        LastSuccessTimestamp_ = Now();
        if (Coordinator_->SetAvailableState(true)) {
            YT_LOG_INFO("Connectivity restored");
        }
    }

    void UpdateProxies()
    {
        TMasterReadOptions options{
            .ReadFrom = EMasterChannelKind::LocalCache,
            .ExpireAfterSuccessfulUpdateTime = Config_->ProxyUpdatePeriod,
            .ExpireAfterFailedUpdateTime = Config_->ProxyUpdatePeriod,
            .CacheStickyGroupSize = 1
        };

        auto connection = Bootstrap_->GetNativeConnection();
        auto channel = RootClient_->GetMasterChannelOrThrow(options.ReadFrom);
        auto proxy = CreateObjectServiceReadProxy(
            RootClient_,
            options.ReadFrom,
            PrimaryMasterCellTagSentinel,
            connection->GetStickyGroupSizeCache());

        auto batchReq = proxy.ExecuteBatch();
        SetBalancingHeader(batchReq, connection->GetConfig(), options);

        {
            auto req = TYPathProxy::Get(ProxyPath_ + "/@");
            ToProto(
                req->mutable_attributes()->mutable_keys(),
                std::vector<TString>{
                    RoleAttributeName,
                    BannedAttributeName,
                    BanMessageAttributeName,
                });
            SetCachingHeader(req, connection->GetConfig(), options);
            batchReq->AddRequest(req, "get_ban");
        }

        {
            auto req = TYPathProxy::Get(RpcProxiesPath);
            ToProto(
                req->mutable_attributes()->mutable_keys(),
                std::vector<TString>{
                    RoleAttributeName,
                    BannedAttributeName,
                    AddressesAttributeName,
                });
            SetCachingHeader(req, connection->GetConfig(), options);
            batchReq->AddRequest(req, "get_proxies");
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting states of proxies");
        const auto& batchRsp = batchRspOrError.Value();

        {
            auto rsp = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_ban").Value();
            auto attributes = ConvertToAttributes(TYsonString(rsp->value()));
            bool banned = attributes->Get(BannedAttributeName, false);
            bool changed = Coordinator_->SetBannedState(banned);
            if (changed) {
                if (banned) {
                    Coordinator_->SetBanMessage(attributes->Get(BanMessageAttributeName, TString()));
                }
                YT_LOG_INFO("Proxy has been %v (Path: %v)", banned ? "banned" : "unbanned", ProxyPath_);
            }

            auto role = attributes->Find<TString>(RoleAttributeName);
            Coordinator_->SetProxyRole(role);

            if (role) {
                NProfiling::TSolomonRegistry::Get()->SetDynamicTags({NProfiling::TTag{"proxy_role", *role}});
            } else {
                NProfiling::TSolomonRegistry::Get()->SetDynamicTags({NProfiling::TTag{"proxy_role", DefaultProxyRole}});
            }
        }
        {
            std::vector<TProxy> proxies;

            auto rsp = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_proxies").Value();
            auto nodeResult = ConvertToNode(TYsonString(rsp->value()));

            for (const auto& child : nodeResult->AsMap()->GetChildren()) {
                const auto& attributes = child.second->Attributes();

                bool banned = attributes.Get(BannedAttributeName, false);
                auto role = attributes.Get<TString>(RoleAttributeName, DefaultProxyRole);
                auto addresses = attributes.Get<TProxyAddressMap>(AddressesAttributeName, {});
                bool alive = static_cast<bool>(child.second->AsMap()->FindChild(AliveNodeName));

                bool available = alive && !banned;
                if (available) {
                    if (addresses.size() == 0) {
                        addresses[DefaultAddressType] = TAddressMap{{DefaultNetworkName, child.first}};
                    }
                    proxies.push_back({addresses, role});
                }
            }
            YT_LOG_DEBUG("Updated proxy list (ProxyCount: %v)", proxies.size());

            {
                auto guard = Guard(ProxySpinLock_);
                AvailableProxies_ = std::move(proxies);
            }
        }
    }

    DECLARE_RPC_SERVICE_METHOD(NApi::NRpcProxy::NProto, DiscoverProxies)
    {
        Coordinator_->ValidateOperable();

        auto roleFilter = request->has_role() ? request->role() : DefaultProxyRole;
        auto addressType = request->has_address_type()
            ? CheckedEnumCast<EAddressType>(request->address_type())
            : DefaultAddressType;
        auto networkName = request->has_network_name() ? request->network_name() : DefaultNetworkName;

        context->SetRequestInfo("Role: %v", roleFilter);

        {
            auto guard = Guard(ProxySpinLock_);
            for (const auto& proxy : AvailableProxies_) {
                if (proxy.Role != roleFilter) {
                    continue;
                }

                auto address = GetAddressOrNull(proxy.Addresses, addressType, networkName);
                if (address) {
                    *response->mutable_addresses()->Add() = *address;
                }
            }
        }

        context->SetResponseInfo("ProxyCount: %v", response->addresses_size());
        context->Reply();
    }
};

////////////////////////////////////////////////////////////////////////////////

IServicePtr CreateDiscoveryService(
    TBootstrap* bootstrap)
{
    return New<TDiscoveryService>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NRpcProxy
