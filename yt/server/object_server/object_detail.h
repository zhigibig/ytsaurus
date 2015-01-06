#pragma once

#include "public.h"
#include "object.h"
#include "object_proxy.h"
#include "object_manager.h"

#include <core/misc/property.h>

#include <server/hydra/entity_map.h>

#include <core/ytree/ypath_detail.h>
#include <core/ytree/system_attribute_provider.h>

#include <ytlib/object_client/object_ypath.pb.h>
#include <ytlib/object_client/object_service_proxy.h>

#include <server/cell_master/public.h>

#include <server/transaction_server/public.h>

#include <server/security_server/public.h>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

class TObjectProxyBase
    : public virtual NYTree::TSupportsAttributes
    , public virtual NYTree::ISystemAttributeProvider
    , public virtual IObjectProxy
{
public:
    TObjectProxyBase(
        NCellMaster::TBootstrap* bootstrap,
        TObjectBase* object);
    ~TObjectProxyBase();

    // IObjectProxy members
    virtual const TObjectId& GetId() const override;
    virtual const NYTree::IAttributeDictionary& Attributes() const override;
    virtual NYTree::IAttributeDictionary* MutableAttributes() override;
    virtual void Invoke(NRpc::IServiceContextPtr context) override;
    virtual void SerializeAttributes(
        NYson::IYsonConsumer* consumer,
        const NYTree::TAttributeFilter& filter,
        bool sortKeys) override;

protected:
    friend class TObjectManager;
    class TCustomAttributeDictionary;

    NCellMaster::TBootstrap* Bootstrap;
    TObjectBase* Object;

    std::unique_ptr<NYTree::IAttributeDictionary> CustomAttributes;

    DECLARE_YPATH_SERVICE_METHOD(NObjectClient::NProto, GetBasicAttributes);
    DECLARE_YPATH_SERVICE_METHOD(NObjectClient::NProto, CheckPermission);


    //! Returns the full object id that coincides with #Id
    //! for non-versioned objects and additionally includes transaction id for
    //! versioned ones.
    virtual TVersionedObjectId GetVersionedId() const = 0;

    //! Returns the ACD for the object or |nullptr| is none exists.
    virtual NSecurityServer::TAccessControlDescriptor* FindThisAcd() = 0;

    void GuardedInvoke(NRpc::IServiceContextPtr context);
    virtual bool DoInvoke(NRpc::IServiceContextPtr context) override;

    // NYTree::TSupportsAttributes members
    virtual NYTree::IAttributeDictionary* GetCustomAttributes() override;
    virtual ISystemAttributeProvider* GetBuiltinAttributeProvider() override;

    virtual std::unique_ptr<NYTree::IAttributeDictionary> DoCreateCustomAttributes();

    // NYTree::ISystemAttributeProvider members
    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) override;
    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) override;
    virtual TFuture<void> GetBuiltinAttributeAsync(const Stroka& key, NYson::IYsonConsumer* consumer) override;
    virtual bool SetBuiltinAttribute(const Stroka& key, const NYTree::TYsonString& value) override;

    TObjectBase* GetSchema(EObjectType type);
    TObjectBase* GetThisSchema();

    void DeclareMutating();
    void DeclareNonMutating();

    void ValidateTransaction();
    void ValidateNoTransaction();

    // TSupportsPermissions members
    virtual void ValidatePermission(
        NYTree::EPermissionCheckScope scope,
        NYTree::EPermission permission) override;

    void ValidatePermission(
        TObjectBase* object,
        NYTree::EPermission permission);

    bool IsRecovery() const;
    bool IsLeader() const;

    void ValidateActiveLeader() const;
    void ForwardToLeader(NRpc::IServiceContextPtr context);
    void OnLeaderResponse(
        NRpc::IServiceContextPtr context,
        const NObjectClient::TObjectServiceProxy::TErrorOrRspExecuteBatchPtr& batchRspOrError);

    virtual bool IsLoggingEnabled() const override;
    virtual NLog::TLogger CreateLogger() const override;

};

////////////////////////////////////////////////////////////////////////////////

class TNontemplateNonversionedObjectProxyBase
    : public TObjectProxyBase
{
public:
    TNontemplateNonversionedObjectProxyBase(
        NCellMaster::TBootstrap* bootstrap,
        TObjectBase* object);

protected:
    virtual bool DoInvoke(NRpc::IServiceContextPtr context) override;

    virtual void GetSelf(TReqGet* request, TRspGet* response, TCtxGetPtr context) override;

    virtual void ValidateRemoval();
    virtual void RemoveSelf(TReqRemove* request, TRspRemove* response, TCtxRemovePtr context) override;

    virtual TVersionedObjectId GetVersionedId() const override;
    virtual NSecurityServer::TAccessControlDescriptor* FindThisAcd() override;

};

////////////////////////////////////////////////////////////////////////////////

template <class TObject>
class TNonversionedObjectProxyBase
    : public TNontemplateNonversionedObjectProxyBase
{
public:
    TNonversionedObjectProxyBase(NCellMaster::TBootstrap* bootstrap, TObject* object)
        : TNontemplateNonversionedObjectProxyBase(bootstrap, object)
    { }

protected:
    const TObject* GetThisTypedImpl() const
    {
        return static_cast<const TObject*>(Object);
    }

    TObject* GetThisTypedImpl()
    {
        return static_cast<TObject*>(Object);
    }

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT

