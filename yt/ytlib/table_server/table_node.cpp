#include "stdafx.h"
#include "table_node.h"
#include "table_node_proxy.h"

namespace NYT {
namespace NTableServer {

using namespace NCypress;
using namespace NYTree;
using namespace NChunkServer;

////////////////////////////////////////////////////////////////////////////////

TTableNode::TTableNode(const TVersionedNodeId& id)
    : TCypressNodeBase(id)
{ }

TTableNode::TTableNode(const TVersionedNodeId& id, const TTableNode& other)
    : TCypressNodeBase(id, other)
    , ChunkListId_(other.ChunkListId_)
{ }

EObjectType TTableNode::GetObjectType() const
{
    return EObjectType::Table;
}

TAutoPtr<ICypressNode> TTableNode::Clone() const
{
    return new TTableNode(Id, *this);
}

void TTableNode::Save(TOutputStream* output) const
{
    TCypressNodeBase::Save(output);
    ::Save(output, ChunkListId_);
}

void TTableNode::Load(TInputStream* input)
{
    TCypressNodeBase::Load(input);
    ::Load(input, ChunkListId_);
}

////////////////////////////////////////////////////////////////////////////////

class TTableNodeTypeHandler
    : public TCypressNodeTypeHandlerBase<TTableNode>
{
public:
    TTableNodeTypeHandler(
        TCypressManager* cypressManager,
        TChunkManager* chunkManager)
        : TCypressNodeTypeHandlerBase<TTableNode>(cypressManager)
        , ChunkManager(chunkManager)
    { }

    EObjectType GetObjectType()
    {
        return EObjectType::Table;
    }

    ENodeType GetNodeType()
    {
        return ENodeType::Entity;
    }

    virtual TAutoPtr<ICypressNode> CreateFromManifest(
        const TNodeId& nodeId,
        const TTransactionId& transactionId,
        IMapNode* manifest)
    {
        UNUSED(transactionId);
        UNUSED(manifest);

        TAutoPtr<TTableNode> node = new TTableNode(nodeId);

        // Create an empty chunk list and reference it from the node.
        auto& chunkList = ChunkManager->CreateChunkList();
        auto chunkListId = chunkList.GetId();
        node->SetChunkListId(chunkListId);
        CypressManager->GetObjectManager()->RefObject(chunkListId);

        // TODO(babenko): stupid TAutoPtr does not support upcast.
        return node.Release();
    }

    virtual TIntrusivePtr<ICypressNodeProxy> GetProxy(
        const ICypressNode& node,
        const TTransactionId& transactionId)
    {
        return New<TTableNodeProxy>(
            this,
            ~CypressManager,
            ~ChunkManager,
            transactionId,
            node.GetId().ObjectId);
    }

protected:
    virtual void DoDestroy(TTableNode& node)
    {
        CypressManager->GetObjectManager()->UnrefObject(node.GetChunkListId());
    }

    virtual void DoBranch(
        const TTableNode& originatingNode,
        TTableNode& branchedNode)
    {
        // branchedNode is a copy of originatingNode.
        
        // Create composite chunk list and place it in the root of branchedNode.
        auto& compositeChunkList = ChunkManager->CreateChunkList();
        auto compositeChunkListId = compositeChunkList.GetId();
        branchedNode.SetChunkListId(compositeChunkListId);
        CypressManager->GetObjectManager()->RefObject(compositeChunkListId);

        // Make the original chunk list a child of the composite one.
        auto committedChunkListId = originatingNode.GetChunkListId();
        compositeChunkList.ChildrenIds().push_back(committedChunkListId);
        CypressManager->GetObjectManager()->RefObject(committedChunkListId);
    }

    virtual void DoMerge(
        TTableNode& originatingNode,
        TTableNode& branchedNode)
    {
        // TODO(babenko): this needs much improvement

        // Obtain the chunk list of branchedNode.
        auto branchedChunkListId = branchedNode.GetChunkListId();
        auto& branchedChunkList = ChunkManager->GetChunkListForUpdate(branchedChunkListId);
        YASSERT(branchedChunkList.GetObjectRefCounter() == 1);

        // Replace the first child of the branched chunk list with the current chunk list of originatingNode.
        YASSERT(branchedChunkList.ChildrenIds().size() >= 1);
        auto oldFirstChildId = branchedChunkList.ChildrenIds()[0];
        auto newFirstChildId = originatingNode.GetChunkListId();
        branchedChunkList.ChildrenIds()[0] = newFirstChildId;
        CypressManager->GetObjectManager()->RefObject(newFirstChildId);
        CypressManager->GetObjectManager()->UnrefObject(oldFirstChildId);

        // Replace the chunk list of originatingNode.
        auto originatingNodeId = originatingNode.GetId().ObjectId;
        originatingNode.SetChunkListId(branchedChunkListId);
        CypressManager->GetObjectManager()->UnrefObject(newFirstChildId);
    }

private:
    NChunkServer::TChunkManager::TPtr ChunkManager;

};

INodeTypeHandler::TPtr CreateTableTypeHandler(
    TCypressManager* cypressManager,
    TChunkManager* chunkManager)
{
    return New<TTableNodeTypeHandler>(
        cypressManager,
        chunkManager);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT

