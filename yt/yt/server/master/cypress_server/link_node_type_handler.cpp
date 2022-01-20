#include "link_node_type_handler.h"
#include "link_node.h"
#include "link_node_proxy.h"
#include "shard.h"
#include "portal_exit_node.h"

namespace NYT::NCypressServer {

using namespace NYTree;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NCellMaster;
using namespace NTransactionServer;
using namespace NSecurityServer;

////////////////////////////////////////////////////////////////////////////////

class TLinkNodeTypeHandler
    : public TCypressNodeTypeHandlerBase<TLinkNode>
{
private:
    using TBase = TCypressNodeTypeHandlerBase<TLinkNode>;

public:
    using TBase::TBase;

    EObjectType GetObjectType() const override
    {
        return EObjectType::Link;
    }

    ENodeType GetNodeType() const override
    {
        return ENodeType::Entity;
    }

private:
    ICypressNodeProxyPtr DoGetProxy(
        TLinkNode* trunkNode,
        TTransaction* transaction) override
    {
        return CreateLinkNodeProxy(
            Bootstrap_,
            &Metadata_,
            transaction,
            trunkNode);
    }

    std::unique_ptr<TLinkNode> DoCreate(
        TVersionedNodeId id,
        const TCreateNodeContext& context) override
    {
        // TODO(babenko): Make sure that target_path is valid upon creation.
        auto targetPath = context.ExplicitAttributes->GetAndRemove<TString>("target_path");

        auto implHolder = TBase::DoCreate(id, context);
        implHolder->SetTargetPath(targetPath);

        return implHolder;
    }

    void DoBranch(
        const TLinkNode* originatingNode,
        TLinkNode* branchedNode,
        const TLockRequest& lockRequest) override
    {
        TBase::DoBranch(originatingNode, branchedNode, lockRequest);

        branchedNode->SetTargetPath(originatingNode->GetTargetPath());
    }

    void DoMerge(
        TLinkNode* originatingNode,
        TLinkNode* branchedNode) override
    {
        TBase::DoMerge(originatingNode, branchedNode);

        originatingNode->SetTargetPath(branchedNode->GetTargetPath());
    }

    void DoClone(
        TLinkNode* sourceNode,
        TLinkNode* clonedTrunkNode,
        ICypressNodeFactory* factory,
        ENodeCloneMode mode,
        TAccount* account) override
    {
        TBase::DoClone(sourceNode, clonedTrunkNode, factory, mode, account);

        clonedTrunkNode->SetTargetPath(sourceNode->GetTargetPath());
    }

    bool HasBranchedChangesImpl(
        TLinkNode* originatingNode,
        TLinkNode* branchedNode) override
    {
        if (TBase::HasBranchedChangesImpl(originatingNode, branchedNode)) {
            return true;
        }

        return branchedNode->GetTargetPath() != originatingNode->GetTargetPath();
    }

    void DoBeginCopy(
        TLinkNode* node,
        TBeginCopyContext* context) override
    {
        TBase::DoBeginCopy(node, context);

        using NYT::Save;
        Save(*context, node->GetTargetPath());
    }

    void DoEndCopy(
        TLinkNode* trunkNode,
        TEndCopyContext* context,
        ICypressNodeFactory* factory) override
    {
        TBase::DoEndCopy(trunkNode, context, factory);

        using NYT::Load;
        trunkNode->SetTargetPath(Load<TYPath>(*context));
    }
};

////////////////////////////////////////////////////////////////////////////////

INodeTypeHandlerPtr CreateLinkNodeTypeHandler(TBootstrap* bootstrap)
{
    return New<TLinkNodeTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
