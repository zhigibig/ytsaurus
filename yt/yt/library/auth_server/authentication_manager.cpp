#include "authentication_manager.h"

#include "blackbox_service.h"
#include "blackbox_cookie_authenticator.h"
#include "cookie_authenticator.h"
#include "cypress_cookie_manager.h"
#include "cypress_token_authenticator.h"
#include "tvm_service.h"
#include "ticket_authenticator.h"
#include "token_authenticator.h"
#include "config.h"

#include <yt/yt/core/rpc/authenticator.h>

namespace NYT::NAuth {

using namespace NApi;
using namespace NConcurrency;
using namespace NHttp;
using namespace NProfiling;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TAuthenticationManager
    : public IAuthenticationManager
{
public:
    TAuthenticationManager(
        TAuthenticationManagerConfigPtr config,
        IPollerPtr poller,
        NApi::IClientPtr client,
        TProfiler profiler)
    {
        std::vector<NRpc::IAuthenticatorPtr> rpcAuthenticators;
        std::vector<ITokenAuthenticatorPtr> tokenAuthenticators;
        std::vector<ICookieAuthenticatorPtr> cookieAuthenticators;

        if (config->TvmService && poller) {
            TvmService_ = CreateTvmService(
                config->TvmService,
                profiler.WithPrefix("/tvm/remote"));
        }

        IBlackboxServicePtr blackboxService;
        if (config->BlackboxService && poller) {
            blackboxService = CreateBlackboxService(
                config->BlackboxService,
                TvmService_,
                poller,
                profiler.WithPrefix("/blackbox"));
        }

        if (config->CypressCookieManager) {
            CypressCookieManager_ = CreateCypressCookieManager(
                config->CypressCookieManager,
                client,
                profiler);
            cookieAuthenticators.push_back(CypressCookieManager_->GetCookieAuthenticator());
        }

        if (config->BlackboxTokenAuthenticator && blackboxService) {
            // COMPAT(gritukan): Set proper values in proxy configs and remove this code.
            if (!TvmService_) {
                config->BlackboxTokenAuthenticator->GetUserTicket = false;
            }

            tokenAuthenticators.push_back(
                CreateCachingTokenAuthenticator(
                    config->BlackboxTokenAuthenticator,
                    CreateBlackboxTokenAuthenticator(
                        config->BlackboxTokenAuthenticator,
                        blackboxService,
                        profiler.WithPrefix("/blackbox_token_authenticator/remote")),
                    profiler.WithPrefix("/blackbox_token_authenticator/cache")));
        }

        if (config->CypressTokenAuthenticator && client) {
            tokenAuthenticators.push_back(
                CreateCachingTokenAuthenticator(
                    config->CypressTokenAuthenticator,
                    CreateLegacyCypressTokenAuthenticator(
                        config->CypressTokenAuthenticator,
                        client),
                    profiler.WithPrefix("/legacy_cypress_token_authenticator/cache")));

            tokenAuthenticators.push_back(
                CreateCachingTokenAuthenticator(
                    config->CypressTokenAuthenticator,
                    CreateCypressTokenAuthenticator(client),
                    profiler.WithPrefix("/cypress_token_authenticator/cache")));
        }

        if (config->BlackboxCookieAuthenticator && blackboxService) {
            // COMPAT(gritukan): Set proper values in proxy configs and remove this code.
            if (!TvmService_) {
                config->BlackboxCookieAuthenticator->GetUserTicket = false;
            }

            cookieAuthenticators.push_back(CreateCachingCookieAuthenticator(
                config->BlackboxCookieAuthenticator,
                CreateBlackboxCookieAuthenticator(
                    config->BlackboxCookieAuthenticator,
                    blackboxService),
                profiler.WithPrefix("/blackbox_cookie_authenticator/cache")));
        }

        if (blackboxService && config->BlackboxTicketAuthenticator) {
            TicketAuthenticator_ = CreateBlackboxTicketAuthenticator(
                config->BlackboxTicketAuthenticator,
                blackboxService,
                TvmService_);
            rpcAuthenticators.push_back(
                CreateTicketAuthenticatorWrapper(TicketAuthenticator_));
        }

        if (!tokenAuthenticators.empty()) {
            rpcAuthenticators.push_back(CreateTokenAuthenticatorWrapper(
                CreateCompositeTokenAuthenticator(tokenAuthenticators)));
        }

        if (!config->RequireAuthentication) {
            tokenAuthenticators.push_back(CreateNoopTokenAuthenticator());
        }
        TokenAuthenticator_ = CreateCompositeTokenAuthenticator(tokenAuthenticators);

        CookieAuthenticator_ = CreateCompositeCookieAuthenticator(
            std::move(cookieAuthenticators));
        rpcAuthenticators.push_back(CreateCookieAuthenticatorWrapper(CookieAuthenticator_));

        if (!config->RequireAuthentication) {
            rpcAuthenticators.push_back(NRpc::CreateNoopAuthenticator());
        }
        RpcAuthenticator_ = CreateCompositeAuthenticator(std::move(rpcAuthenticators));
    }

    void Start() override
    {
        if (CypressCookieManager_) {
            CypressCookieManager_->Start();
        }
    }

    void Stop() override
    {
        if (CypressCookieManager_) {
            CypressCookieManager_->Stop();
        }
    }

    const NRpc::IAuthenticatorPtr& GetRpcAuthenticator() const override
    {
        return RpcAuthenticator_;
    }

    const ITokenAuthenticatorPtr& GetTokenAuthenticator() const override
    {
        return TokenAuthenticator_;
    }

    const ICookieAuthenticatorPtr& GetCookieAuthenticator() const override
    {
        return CookieAuthenticator_;
    }

    const ITicketAuthenticatorPtr& GetTicketAuthenticator() const override
    {
        return TicketAuthenticator_;
    }

    const ITvmServicePtr& GetTvmService() const override
    {
        return TvmService_;
    }

    const ICypressCookieManagerPtr& GetCypressCookieManager() const override
    {
        return CypressCookieManager_;
    }

private:
    ITvmServicePtr TvmService_;
    NRpc::IAuthenticatorPtr RpcAuthenticator_;
    ITokenAuthenticatorPtr TokenAuthenticator_;
    ICookieAuthenticatorPtr CookieAuthenticator_;
    ITicketAuthenticatorPtr TicketAuthenticator_;

    ICypressCookieManagerPtr CypressCookieManager_;
};

////////////////////////////////////////////////////////////////////////////////

IAuthenticationManagerPtr CreateAuthenticationManager(
    TAuthenticationManagerConfigPtr config,
    IPollerPtr poller,
    NApi::IClientPtr client,
    TProfiler profiler)
{
    return New<TAuthenticationManager>(
        std::move(config),
        std::move(poller),
        std::move(client),
        std::move(profiler));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NAuth
