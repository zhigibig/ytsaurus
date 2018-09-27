#pragma once

#include "public.h"

namespace NYP {
namespace NServer {
namespace NObjects {

////////////////////////////////////////////////////////////////////////////////

struct IObjectTypeHandler
{
    virtual ~IObjectTypeHandler() = default;

    virtual const TDBTable* GetTable() = 0;
    virtual const TDBField* GetIdField() = 0;
    virtual EObjectType GetType() = 0;
    virtual EObjectType GetParentType() = 0;
    virtual const TDBField* GetParentIdField() = 0;
    virtual TChildrenAttributeBase* GetParentChildrenAttribute(TObject* parent) = 0;
    virtual TObject* GetAccessControlParent(TObject* object) = 0;

    virtual TAttributeSchema* GetRootAttributeSchema() = 0;
    virtual TAttributeSchema* GetIdAttributeSchema() = 0;
    virtual TAttributeSchema* GetParentIdAttributeSchema() = 0;

    virtual std::unique_ptr<TObject> InstantiateObject(
        const TObjectId& id,
        const TObjectId& parentId,
        ISession* session) = 0;

    virtual void BeforeObjectCreated(
        TTransaction* transaction,
        TObject* object) = 0;

    virtual void AfterObjectCreated(
        TTransaction* transaction,
        TObject* object) = 0;

    virtual void BeforeObjectRemoved(
        TTransaction* transaction,
        TObject* object) = 0;

    virtual void AfterObjectRemoved(
        TTransaction* transaction,
        TObject* object) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjects
} // namespace NServer
} // namespace NYP
