#include "stdafx.h"
#include "cypress_integration.h"

#include <core/misc/lazy_ptr.h>

#include <core/concurrency/action_queue.h>

#include <core/ytree/ephemeral_node_factory.h>
#include <core/ytree/ypath_detail.h>

#include <core/ytree/ypath.pb.h>

#include <core/rpc/channel.h>
#include <core/rpc/message.h>
#include <core/rpc/channel_cache.h>

#include <ytlib/orchid/orchid_service_proxy.h>

#include <server/cell_master/bootstrap.h>

#include <server/object_server/object_manager.h>
#include <server/object_server/object_proxy.h>

#include <server/cypress_server/node.h>
#include <server/cypress_server/virtual.h>

namespace NYT {
namespace NOrchid {

using namespace NRpc;
using namespace NBus;
using namespace NYTree;
using namespace NCypressServer;
using namespace NObjectServer;
using namespace NCellMaster;
using namespace NTransactionServer;
using namespace NOrchid::NProto;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = OrchidLogger;

static TChannelCache ChannelCache;
static TLazyIntrusivePtr<TActionQueue> OrchidQueue(TActionQueue::CreateFactory("Orchid"));

////////////////////////////////////////////////////////////////////////////////

class TOrchidYPathService
    : public IYPathService
{
public:
    TOrchidYPathService(
        TBootstrap* bootstrap,
        TCypressNodeBase* trunkNode,
        TTransaction* transaction)
        : Bootstrap(bootstrap)
        , TrunkNode(trunkNode)
        , Transaction(transaction)
    {
        YCHECK(trunkNode->IsTrunk());
    }

    TResolveResult Resolve(
        const TYPath& path,
        IServiceContextPtr context) override
    {
        UNUSED(context);

        return TResolveResult::Here(path);
    }

    void Invoke(IServiceContextPtr context) override
    {
        auto manifest = LoadManifest();

        auto channel = ChannelCache.GetChannel(manifest->RemoteAddress);

        TOrchidServiceProxy proxy(channel);
        proxy.SetDefaultTimeout(manifest->Timeout);

        auto path = GetRedirectPath(manifest, GetRequestYPath(context));
        auto verb = context->GetVerb();

        auto requestMessage = context->GetRequestMessage();
        NRpc::NProto::TRequestHeader requestHeader;
        if (!ParseRequestHeader(requestMessage, &requestHeader)) {
            context->Reply(TError("Error parsing request header"));
            return;
        }

        SetRequestYPath(&requestHeader, path);

        auto innerRequestMessage = SetRequestHeader(requestMessage, requestHeader);

        auto outerRequest = proxy.Execute();
        outerRequest->Attachments() = innerRequestMessage.ToVector();

        LOG_INFO("Sending request to the remote Orchid (RemoteAddress: %s, Path: %s, Verb: %s, RequestId: %s)",
            ~manifest->RemoteAddress,
            ~path,
            ~verb,
            ~ToString(outerRequest->GetRequestId()));

        outerRequest->Invoke().Subscribe(
            BIND(
                &TOrchidYPathService::OnResponse,
                MakeStrong(this),
                context,
                manifest,
                path,
                verb)
            .Via(OrchidQueue->GetInvoker()));
    }

    Stroka GetLoggingCategory() const override
    {
        return OrchidLogger.GetCategory();
    }

    // TODO(panin): remove this when getting rid of IAttributeProvider
    virtual void SerializeAttributes(
        NYson::IYsonConsumer* /*consumer*/,
        const TAttributeFilter& /*filter*/,
        bool /*sortKeys*/) override
    {
        YUNREACHABLE();
    }

private:
    TBootstrap* Bootstrap;
    TCypressNodeBase* TrunkNode;
    TTransaction* Transaction;

    TOrchidManifest::TPtr LoadManifest()
    {
        auto objectManager = Bootstrap->GetObjectManager();
        auto proxy = objectManager->GetProxy(TrunkNode, Transaction);
        auto manifest = New<TOrchidManifest>();
        auto manifestNode = ConvertToNode(proxy->Attributes());
        try {
            manifest->Load(manifestNode);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing Orchid manifest")
                << ex;
        }
        return manifest;
    }

    void OnResponse(
        IServiceContextPtr context,
        TOrchidManifest::TPtr manifest,
        const TYPath& path,
        const Stroka& verb,
        TOrchidServiceProxy::TRspExecutePtr response)
    {
        LOG_INFO(*response, "Reply from a remote Orchid received (RequestId: %s)",
            ~ToString(context->GetRequestId()));

        if (response->IsOK()) {
            auto innerResponseMessage = TSharedRefArray(response->Attachments());
            context->Reply(innerResponseMessage);
        } else {
            context->Reply(TError("Error executing an Orchid operation (Path: %s, Verb: %s, RemoteAddress: %s, RemoteRoot: %s)",
                ~path,
                ~verb,
                ~manifest->RemoteAddress,
                ~manifest->RemoteRoot)
                << response->GetError());
        }
    }

    static Stroka GetRedirectPath(TOrchidManifest::TPtr manifest, const TYPath& path)
    {
        return manifest->RemoteRoot + path;
    }
};

INodeTypeHandlerPtr CreateOrchidTypeHandler(TBootstrap* bootstrap)
{
    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::Orchid,
        BIND([=] (TCypressNodeBase* trunkNode, TTransaction* transaction) -> IYPathServicePtr {
            return New<TOrchidYPathService>(bootstrap, trunkNode, transaction);
        }),
        EVirtualNodeOptions::None);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NOrchid
} // namespace NYT
