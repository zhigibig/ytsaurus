#include "authentication_manager.h"
#include "token_authenticator.h"
#include "cookie_authenticator.h"
#include "default_blackbox_service.h"

#include <yt/ytlib/auth/config.h>

#include <yt/core/rpc/authenticator.h>

namespace NYT {
namespace NAuth {

using namespace NApi;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TAuthenticationManager::TImpl
{
public:
    TImpl(
        TAuthenticationManagerConfigPtr config,
        IInvokerPtr invoker,
        IClientPtr client)
    {
        std::vector<NRpc::IAuthenticatorPtr> rpcAuthenticators;
        std::vector<NAuth::ITokenAuthenticatorPtr> tokenAuthenticators;

        IBlackboxServicePtr blackboxService;
        if (config->BlackboxService && invoker) {
            blackboxService = CreateDefaultBlackboxService(
                config->BlackboxService,
                invoker);
        }

        if (config->BlackboxTokenAuthenticator && blackboxService) {
            tokenAuthenticators.push_back(
                CreateCachingTokenAuthenticator(
                    config->BlackboxTokenAuthenticator,
                    CreateBlackboxTokenAuthenticator(
                        config->BlackboxTokenAuthenticator,
                        std::move(blackboxService))));
        }

        if (config->CypressTokenAuthenticator && client) {
            tokenAuthenticators.push_back(
                CreateCachingTokenAuthenticator(
                    config->CypressTokenAuthenticator,
                    CreateCypressTokenAuthenticator(
                        config->CypressTokenAuthenticator,
                        client)));
        }

        if (!tokenAuthenticators.empty()) {
            rpcAuthenticators.push_back(
                CreateTokenAuthenticatorWrapper(
                    CreateCompositeTokenAuthenticator(std::move(tokenAuthenticators))));

        }

        if (config->BlackboxCookieAuthenticator && blackboxService) {
            rpcAuthenticators.push_back(
                CreateCookieAuthenticatorWrapper(
                    CreateCachingCookieAuthenticator(
                    config->BlackboxCookieAuthenticator,
                        CreateBlackboxCookieAuthenticator(
                            config->BlackboxCookieAuthenticator,
                            std::move(blackboxService)))));
        }

        if (!config->RequireAuthentication) {
            rpcAuthenticators.push_back(CreateNoopAuthenticator());
            tokenAuthenticators.push_back(CreateNoopTokenAuthenticator());
        }

        RpcAuthenticator_ = CreateCompositeAuthenticator(std::move(rpcAuthenticators));
        TokenAuthenticator_ = CreateCompositeTokenAuthenticator(std::move(tokenAuthenticators));
    }
    
    const NRpc::IAuthenticatorPtr& GetRpcAuthenticator() const
    {
        return RpcAuthenticator_;
    }

    const NAuth::ITokenAuthenticatorPtr& GetTokenAuthenticator() const
    {
        return TokenAuthenticator_;
    }

private:
    NRpc::IAuthenticatorPtr RpcAuthenticator_;
    NAuth::ITokenAuthenticatorPtr TokenAuthenticator_;
};

////////////////////////////////////////////////////////////////////////////////

TAuthenticationManager::TAuthenticationManager(
    TAuthenticationManagerConfigPtr config,
    IInvokerPtr invoker,
    IClientPtr client)
    : Impl_(std::make_unique<TImpl>(
        std::move(config),
        std::move(invoker),
        std::move(client)))
{ }

const NRpc::IAuthenticatorPtr& TAuthenticationManager::GetRpcAuthenticator() const
{
    return Impl_->GetRpcAuthenticator();
}

const NAuth::ITokenAuthenticatorPtr& TAuthenticationManager::GetTokenAuthenticator() const
{
    return Impl_->GetTokenAuthenticator();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NAuth
} // namespace NYT

