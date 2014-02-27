#include "stdafx.h"
#include "orchid_service.h"
#include "orchid_service_proxy.h"
#include "private.h"

#include <core/ytree/ypath_detail.h>
#include <core/ytree/ypath_client.h>

#include <core/rpc/message.h>

namespace NYT {
namespace NOrchid {

using namespace NBus;
using namespace NRpc;
using namespace NRpc::NProto;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = OrchidLogger;

////////////////////////////////////////////////////////////////////////////////

class TOrchidService
    : public TServiceBase
{
public:
    TOrchidService(
        NYTree::INodePtr root,
        IInvokerPtr invoker)
        : TServiceBase(
            invoker,
            TOrchidServiceProxy::GetServiceName(),
            OrchidLogger.GetCategory())
    {
        YCHECK(root);

        RootService = CreateRootService(root);
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Execute));
    }

private:
    typedef TOrchidService TThis;

    NYTree::IYPathServicePtr RootService;

    DECLARE_RPC_SERVICE_METHOD(NProto, Execute)
    {
        UNUSED(request);
        UNUSED(response);

        auto requestMessage = TSharedRefArray(request->Attachments());

        TRequestHeader requestHeader;
        if (!ParseRequestHeader(requestMessage, &requestHeader)) {
            THROW_ERROR_EXCEPTION("Error parsing request header");
        }

        const auto& path = requestHeader.path();
        const auto& verb = requestHeader.verb();

        context->SetRequestInfo("Path: %s, Verb: %s",
            ~path,
            ~verb);

        ExecuteVerb(RootService, requestMessage)
            .Subscribe(BIND([=] (TSharedRefArray responseMessage) {
                TResponseHeader responseHeader;
                YCHECK(ParseResponseHeader(responseMessage, &responseHeader));

                auto error = FromProto(responseHeader.error());

                context->SetRequestInfo("Error: %s", ~ToString(error));

                response->Attachments() = responseMessage.ToVector();
                context->Reply();
            }));
    }

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NOrchid
} // namespace NYT

