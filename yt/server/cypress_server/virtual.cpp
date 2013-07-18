#include "stdafx.h"
#include "virtual.h"

#include <ytlib/misc/singleton.h>

#include <ytlib/ypath/tokenizer.h>

#include <server/cypress_server/node_detail.h>
#include <server/cypress_server/node_proxy_detail.h>

#include <server/cell_master/bootstrap.h>

namespace NYT {
namespace NCypressServer {

using namespace NRpc;
using namespace NYTree;
using namespace NYTree;
using namespace NYson;
using namespace NCellMaster;
using namespace NTransactionServer;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TVirtualNode
    : public TCypressNodeBase
{
public:
    explicit TVirtualNode(const TVersionedNodeId& id)
        : TCypressNodeBase(id)
    { }

};

////////////////////////////////////////////////////////////////////////////////

class TFailedLeaderValidationWrapper
    : public IYPathService
{
public:
    explicit TFailedLeaderValidationWrapper(TBootstrap* bootstrap)
        : Bootstrap(bootstrap)
    { }

    virtual TResolveResult Resolve(const TYPath& path, IServiceContextPtr context) override
    {
        UNUSED(context);

        return TResolveResult::Here(path);
    }

    virtual void Invoke(IServiceContextPtr context) override
    {
        UNUSED(context);

        Bootstrap->GetMetaStateFacade()->ValidateActiveLeader();
    }

    virtual Stroka GetLoggingCategory() const override
    {
        return "";
    }

    virtual bool IsWriteRequest(IServiceContextPtr context) const override
    {
        UNUSED(context);

        return false;
    }

    // TODO(panin): remove this when getting rid of IAttributeProvider
    virtual void SerializeAttributes(
        IYsonConsumer* /*consumer*/,
        const TAttributeFilter& /*filter*/,
        bool /*sortKeys*/) override
    {
        YUNREACHABLE();
    }

private:
    TBootstrap* Bootstrap;
};

class TLeaderValidatorWrapper
    : public IYPathService
{
public:
    TLeaderValidatorWrapper(
        TBootstrap* bootstrap,
        IYPathServicePtr underlyingService)
        : Bootstrap(bootstrap)
        , UnderlyingService(underlyingService)
    { }

    virtual TResolveResult Resolve(const TYPath& path, IServiceContextPtr context) override
    {
        if (!Bootstrap->GetMetaStateFacade()->IsActiveLeader()) {
            return TResolveResult::There(
                New<TFailedLeaderValidationWrapper>(Bootstrap),
                path);
        }
        return UnderlyingService->Resolve(path, context);
    }

    virtual void Invoke(IServiceContextPtr context) override
    {
        Bootstrap->GetMetaStateFacade()->ValidateActiveLeader();
        UnderlyingService->Invoke(context);
    }

    virtual Stroka GetLoggingCategory() const override
    {
        return UnderlyingService->GetLoggingCategory();
    }

    virtual bool IsWriteRequest(IServiceContextPtr context) const override
    {
        return UnderlyingService->IsWriteRequest(context);
    }

    // TODO(panin): remove this when getting rid of IAttributeProvider
    virtual void SerializeAttributes(
        IYsonConsumer* consumer,
        const TAttributeFilter& filter,
        bool sortKeys) override
    {
        UnderlyingService->SerializeAttributes(consumer, filter, sortKeys);
    }

private:
    TBootstrap* Bootstrap;
    IYPathServicePtr UnderlyingService;

};

////////////////////////////////////////////////////////////////////////////////

class TVirtualNodeProxy
    : public TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, IEntityNode, TVirtualNode>
{
public:
    TVirtualNodeProxy(
        INodeTypeHandlerPtr typeHandler,
        TBootstrap* bootstrap,
        TTransaction* transaction,
        TVirtualNode* trunkNode,
        IYPathServicePtr service,
        EVirtualNodeOptions options)
        : TBase(
            typeHandler,
            bootstrap,
            transaction,
            trunkNode)
        , Service(service)
        , Options(options)
    { }

private:
    typedef TCypressNodeProxyBase<TNontemplateCypressNodeProxyBase, IEntityNode, TVirtualNode> TBase;

    IYPathServicePtr Service;
    EVirtualNodeOptions Options;


    virtual TResolveResult ResolveSelf(const TYPath& path, IServiceContextPtr context) override
    {
        const auto& verb = context->GetVerb();
        if (Options & EVirtualNodeOptions::RedirectSelf &&
            verb != "Remove")
        {
            return TResolveResult::There(Service, path);
        } else {
            return TBase::ResolveSelf(path, context);
        }
    }

    virtual TResolveResult ResolveRecursive(const TYPath& path, IServiceContextPtr context) override
    {
        NYPath::TTokenizer tokenizer(path);
        switch (tokenizer.Advance()) {
            case NYPath::ETokenType::EndOfStream:
            case NYPath::ETokenType::Slash:
                return TResolveResult::There(Service, path);
            default:
                return TResolveResult::There(Service, "/" + path);
        }
    }


    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) override
    {
        auto* provider = GetTargetSystemAttributeProvider();
        if (provider) {
            provider->ListSystemAttributes(attributes);
        }

        TBase::ListSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        auto* provider = GetTargetSystemAttributeProvider();
        if (provider && provider->GetSystemAttribute(key, consumer)) {
            return true;
        }

        return TBase::GetSystemAttribute(key, consumer);
    }

    virtual bool SetSystemAttribute(const Stroka& key, const TYsonString& value) override
    {
        auto* provider = GetTargetSystemAttributeProvider();
        if (provider && provider->SetSystemAttribute(key, value)) {
            return true;
        }

        return TBase::SetSystemAttribute(key, value);
    }

    virtual bool DoInvoke(NRpc::IServiceContextPtr context) override
    {
        auto metaStateFacade = Bootstrap->GetMetaStateFacade();
        if ((Options & EVirtualNodeOptions::RequireLeader) && !metaStateFacade->GetManager()->GetMutationContext()) {
            metaStateFacade->ValidateActiveLeader();
        }

        return TBase::DoInvoke(context);
    }

    ISystemAttributeProvider* GetTargetSystemAttributeProvider()
    {
        return dynamic_cast<ISystemAttributeProvider*>(~Service);
    }

};

////////////////////////////////////////////////////////////////////////////////

class TVirtualNodeTypeHandler
    : public TCypressNodeTypeHandlerBase<TVirtualNode>
{
public:
    TVirtualNodeTypeHandler(
        TBootstrap* bootstrap,
        TYPathServiceProducer producer,
        EObjectType objectType,
        EVirtualNodeOptions options)
        : TCypressNodeTypeHandlerBase<TVirtualNode>(bootstrap)
        , Producer(producer)
        , ObjectType(objectType)
        , Options(options)
    { }

    virtual EObjectType GetObjectType() override
    {
        return ObjectType;
    }

    virtual ENodeType GetNodeType() override
    {
        return ENodeType::Entity;
    }

private:
    TYPathServiceProducer Producer;
    EObjectType ObjectType;
    EVirtualNodeOptions Options;

    virtual ICypressNodeProxyPtr DoGetProxy(
        TVirtualNode* trunkNode,
        TTransaction* transaction) override
    {
        auto service = Producer.Run(trunkNode, transaction);
        return New<TVirtualNodeProxy>(
            this,
            Bootstrap,
            transaction,
            trunkNode,
            service,
            Options);
    }


};

INodeTypeHandlerPtr CreateVirtualTypeHandler(
    TBootstrap* bootstrap,
    EObjectType objectType,
    TYPathServiceProducer producer,
    EVirtualNodeOptions options)
{
    return New<TVirtualNodeTypeHandler>(
        bootstrap,
        producer,
        objectType,
        options);
}

INodeTypeHandlerPtr CreateVirtualTypeHandler(
    TBootstrap* bootstrap,
    EObjectType objectType,
    IYPathServicePtr service,
    EVirtualNodeOptions options)
{
    return CreateVirtualTypeHandler(
        bootstrap,
        objectType,
        BIND([=] (TCypressNodeBase* trunkNode, TTransaction* transaction) -> IYPathServicePtr {
            UNUSED(trunkNode);
            UNUSED(transaction);
            return service;
        }),
        options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
