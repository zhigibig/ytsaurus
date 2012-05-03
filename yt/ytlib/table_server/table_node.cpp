#include "stdafx.h"
#include "table_node.h"
#include "table_node_proxy.h"

#include <ytlib/chunk_server/chunk.h>
#include <ytlib/chunk_server/chunk_list.h>
#include <ytlib/cell_master/bootstrap.h>
#include <ytlib/cell_master/load_context.h>
#include <ytlib/table_client/schema.h>

namespace NYT {
namespace NTableServer {

using namespace NCellMaster;
using namespace NCypress;
using namespace NYTree;
using namespace NChunkServer;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

TTableNode::TTableNode(const TVersionedNodeId& id)
    : TCypressNodeBase(id)
{ }

TTableNode::TTableNode(const TVersionedNodeId& id, const TTableNode& other)
    : TCypressNodeBase(id, other)
    , ChunkList_(other.ChunkList_)
{ }

EObjectType TTableNode::GetObjectType() const
{
    return EObjectType::Table;
}

void TTableNode::Save(TOutputStream* output) const
{
    TCypressNodeBase::Save(output);
    SaveObjectRef(output, ChunkList_);
    ::Save(output, KeyColumns_);
}

void TTableNode::Load(const TLoadContext& context, TInputStream* input)
{
    TCypressNodeBase::Load(context, input);
    LoadObjectRef(input, ChunkList_, context);
    ::Load(input, KeyColumns_);
}

////////////////////////////////////////////////////////////////////////////////

class TTableNodeTypeHandler
    : public TCypressNodeTypeHandlerBase<TTableNode>
{
public:
    typedef TCypressNodeTypeHandlerBase<TTableNode> TBase;

    TTableNodeTypeHandler(TBootstrap* bootstrap)
        : TBase(bootstrap)
    { }

    EObjectType GetObjectType()
    {
        return EObjectType::Table;
    }

    ENodeType GetNodeType()
    {
        return ENodeType::Entity;
    }

    virtual bool IsLockModeSupported(ELockMode mode)
    {
        return
            mode == ELockMode::Exclusive ||
            mode == ELockMode::Shared ||
            mode == ELockMode::Snapshot;
    }

    virtual TNodeId CreateDynamic(
        NTransactionServer::TTransaction* transaction,
        TReqCreate* request,
        TRspCreate* response)
    {
        auto chunkManager = Bootstrap->GetChunkManager();
        auto cypressManager = Bootstrap->GetCypressManager();
        auto objectManager = Bootstrap->GetObjectManager();

        // Parse and validate schema, if any.
        auto ysonChannels = request->Attributes().FindYson("channels");
        if (ysonChannels) {
            try {
                ChannelsFromYson(ysonChannels.Get());
            } catch (const std::exception& ex) {
                ythrow yexception() << Sprintf("Invalid table schema\n%s", ex.what());
            }
        } else {
            request->Attributes().SetYson("channels", "[]");
        }

        auto nodeId = objectManager->GenerateId(EObjectType::Table);
        TAutoPtr<TTableNode> node(new TTableNode(nodeId));

        // Create an empty chunk list and reference it from the node.
        auto& chunkList = chunkManager->CreateChunkList();
        node->SetChunkList(&chunkList);

        auto chunkListId = chunkList.GetId();
        objectManager->RefObject(chunkListId);

        cypressManager->RegisterNode(transaction, node.Release());

        return nodeId;
    }

    virtual TIntrusivePtr<ICypressNodeProxy> GetProxy(
        const TNodeId& nodeId,
        NTransactionServer::TTransaction* transaction)
    {
        return New<TTableNodeProxy>(
            this,
            Bootstrap,
            transaction,
            nodeId);
    }

protected:
    virtual void DoDestroy(TTableNode& node)
    {
        Bootstrap->GetObjectManager()->UnrefObject(node.GetChunkList()->GetId());
    }

    virtual void DoBranch(const TTableNode& originatingNode, TTableNode& branchedNode)
    {
        // branchedNode is a copy of originatingNode.
        
        auto chunkManager = Bootstrap->GetChunkManager();
        auto objectManager = Bootstrap->GetObjectManager();

        // Create composite chunk list and place it in the root of branchedNode.
        auto& branchedChunkList = chunkManager->CreateChunkList();
        branchedNode.SetChunkList(&branchedChunkList);

        auto branchedChunkListId = branchedChunkList.GetId();
        objectManager->RefObject(branchedChunkListId);

        // Make the original chunk list a child of the composite one.
        yvector<TChunkTreeRef> children;
        auto* originatingChunkList = originatingNode.GetChunkList();
        children.push_back(TChunkTreeRef(originatingChunkList));
        chunkManager->AttachToChunkList(branchedChunkList, children);

        // Propagate "sorted" attributes.
        branchedChunkList.SetSorted(originatingChunkList->GetSorted());
    }

    // TODO(babenko): this needs much improvement
    virtual void DoMerge(TTableNode& originatingNode, TTableNode& branchedNode)
    {
        auto chunkManager = Bootstrap->GetChunkManager();
        auto objectManager = Bootstrap->GetObjectManager();

        // Obtain the chunk list of branchedNode.
        auto branchedChunkList = branchedNode.GetChunkList();
        YASSERT(branchedChunkList->GetObjectRefCounter() == 1);

        // Replace the first child of the branched chunk list with the current chunk list of originatingNode.
        YASSERT(branchedChunkList->Children().size() >= 1);
        auto oldFirstChild = branchedChunkList->Children()[0];
        auto newFirstChild = originatingNode.GetChunkList();
        auto newFirstChildId = newFirstChild->GetId();
        branchedChunkList->Children()[0] = TChunkTreeRef(newFirstChild);
        objectManager->RefObject(newFirstChildId);
        objectManager->UnrefObject(oldFirstChild.GetId());

        // Replace the chunk list of originatingNode.
        originatingNode.SetChunkList(branchedChunkList);
        objectManager->UnrefObject(newFirstChildId);
    }

};

INodeTypeHandler::TPtr CreateTableTypeHandler(TBootstrap* bootstrap)
{
    return New<TTableNodeTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableServer
} // namespace NYT

