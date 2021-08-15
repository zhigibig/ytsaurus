#include "cypress_integration.h"

#include "tablet.h"
#include "tablet_manager.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>

#include <yt/yt/server/master/cypress_server/node_detail.h>
#include <yt/yt/server/master/cypress_server/node_proxy_detail.h>
#include <yt/yt/server/master/cypress_server/virtual.h>

namespace NYT::NTabletServer {

using namespace NYPath;
using namespace NRpc;
using namespace NYson;
using namespace NYTree;
using namespace NObjectClient;
using namespace NCypressServer;
using namespace NTransactionServer;
using namespace NObjectServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

class TVirtualTabletMap
    : public TVirtualMulticellMapBase
{
public:
    TVirtualTabletMap(TBootstrap* bootstrap, INodePtr owningProxy)
        : TVirtualMulticellMapBase(bootstrap, owningProxy)
    { }

private:
    virtual std::vector<TObjectId> GetKeys(i64 sizeLimit) const override
    {
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        return ToObjectIds(GetValues(tabletManager->Tablets(), sizeLimit));
    }

    virtual bool IsValid(TObject* object) const override
    {
        return object->GetType() == EObjectType::Tablet;
    }

    virtual i64 GetSize() const override
    {
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        return tabletManager->Tablets().GetSize();
    }

    bool NeedSuppressUpstreamSync() const override
    {
        return false;
    }

protected:
    virtual TYPath GetWellKnownPath() const override
    {
        return "//sys/tablets";
    }
};

INodeTypeHandlerPtr CreateTabletMapTypeHandler(TBootstrap* bootstrap)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::TabletMap,
        BIND([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TVirtualTabletMap>(bootstrap, owningNode);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualTabletActionMap
    : public TVirtualMulticellMapBase
{
public:
    TVirtualTabletActionMap(TBootstrap* bootstrap, INodePtr owningProxy)
        : TVirtualMulticellMapBase(bootstrap, owningProxy)
    { }

private:
    virtual std::vector<TObjectId> GetKeys(i64 sizeLimit) const override
    {
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        return ToObjectIds(GetValues(tabletManager->TabletActions(), sizeLimit));
    }

    virtual bool IsValid(TObject* object) const override
    {
        return object->GetType() == EObjectType::TabletAction;
    }

    virtual i64 GetSize() const override
    {
        const auto& tabletManager = Bootstrap_->GetTabletManager();
        return tabletManager->TabletActions().GetSize();
    }

    bool NeedSuppressUpstreamSync() const override
    {
        return false;
    }

protected:
    virtual TYPath GetWellKnownPath() const override
    {
        return "//sys/tablet_actions";
    }
};

INodeTypeHandlerPtr CreateTabletActionMapTypeHandler(TBootstrap* bootstrap)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::TabletActionMap,
        BIND([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TVirtualTabletActionMap>(bootstrap, owningNode);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
