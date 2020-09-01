#include "cypress_integration.h"
#include "transaction.h"

#include <yt/server/master/cell_master/bootstrap.h>

#include <yt/server/master/cypress_server/virtual.h>

#include <yt/server/master/object_server/object_manager.h>

#include <yt/server/master/transaction_server/transaction_manager.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/misc/string.h>

#include <yt/core/ytree/virtual.h>

namespace NYT::NTransactionServer {

using namespace NYTree;
using namespace NCypressServer;
using namespace NCellMaster;
using namespace NObjectClient;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

template <class T>
class TVirtualTransactionMapBase
    : public TVirtualMulticellMapBase
{
public:
    TVirtualTransactionMapBase(
        TBootstrap* bootstrap,
        INodePtr owningNode)
        : TVirtualMulticellMapBase(bootstrap, owningNode)
    { }

private:
    const THashSet<TTransaction*>& Transactions() const
    {
        return static_cast<const T*>(this)->Transactions();
    }

    virtual std::vector<TObjectId> GetKeys(i64 sizeLimit) const override
    {
        return ToObjectIds(Transactions(), sizeLimit);
    }

    virtual bool IsValid(TObject* object) const override
    {
        return IsObjectAlive(object);
    }

    virtual bool NeedSuppressUpstreamSync() const
    {
        return false;
    }

    virtual i64 GetSize() const override
    {
        return Transactions().size();
    }

    virtual NYPath::TYPath GetWellKnownPath() const override
    {
        return static_cast<const T*>(this)->GetWellKnownPath();
    }
};

class TVirtualTopmostTransactionMap
    : public TVirtualTransactionMapBase<TVirtualTopmostTransactionMap>
{
public:
    using TBase = TVirtualTransactionMapBase<TVirtualTopmostTransactionMap>;

    using TBase::TBase;

    const THashSet<TTransaction*>& Transactions() const
    {
        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        return transactionManager->NativeTopmostTransactions();
    }

    NYPath::TYPath GetWellKnownPath() const
    {
        return "//sys/topmost_transactions";
    }
};

class TVirtualTransactionMap
    : public TVirtualTransactionMapBase<TVirtualTransactionMap>
{
public:
    using TBase = TVirtualTransactionMapBase<TVirtualTransactionMap>;

    using TBase::TBase;

    const THashSet<TTransaction*>& Transactions() const
    {
        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        return transactionManager->NativeTransactions();
    }

    NYPath::TYPath GetWellKnownPath() const
    {
        return "//sys/transactions";
    }
};

////////////////////////////////////////////////////////////////////////////////

INodeTypeHandlerPtr CreateTransactionMapTypeHandler(TBootstrap* bootstrap)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::TransactionMap,
        BIND([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TVirtualTransactionMap>(bootstrap, std::move(owningNode));
        }),
        EVirtualNodeOptions::RedirectSelf);
}

INodeTypeHandlerPtr CreateTopmostTransactionMapTypeHandler(TBootstrap* bootstrap)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::TopmostTransactionMap,
        BIND([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TVirtualTopmostTransactionMap>(bootstrap, std::move(owningNode));
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionServer
