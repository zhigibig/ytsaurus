#include "cypress_manager.h"
#include "node_proxy.h"

#include "../ytree/yson_reader.h"
#include "../ytree/yson_writer.h"

namespace NYT {
namespace NCypress {

using namespace NMetaState;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = CypressLogger;

////////////////////////////////////////////////////////////////////////////////

TCypressManager::TCypressManager(
    TMetaStateManager::TPtr metaStateManager,
    TCompositeMetaState::TPtr metaState,
    TTransactionManager::TPtr transactionManager)
    : TMetaStatePart(metaStateManager, metaState)
    , TransactionManager(transactionManager)
{
    YASSERT(~transactionManager != NULL);

    transactionManager->OnTransactionCommitted().Subscribe(FromMethod(
        &TThis::OnTransactionCommitted,
        TPtr(this)));
    transactionManager->OnTransactionAborted().Subscribe(FromMethod(
        &TThis::OnTransactionAborted,
        TPtr(this)));

    RegisterMethod(this, &TThis::SetYPath);
    RegisterMethod(this, &TThis::RemoveYPath);
    RegisterMethod(this, &TThis::LockYPath);

    metaState->RegisterPart(this);
}

INode::TPtr TCypressManager::FindNode(
    const TNodeId& nodeId,
    const TTransactionId& transactionId)
{
    auto impl = FindNode(TBranchedNodeId(nodeId, transactionId));
    if (impl == NULL) {
        impl = FindNode(TBranchedNodeId(nodeId, NullTransactionId));
    }
    if (impl == NULL) {
        return NULL;
    }
    return ~impl->GetProxy(this, transactionId);
}

INode::TPtr TCypressManager::GetNode(
    const TNodeId& nodeId,
    const TTransactionId& transactionId)
{
    auto node = FindNode(nodeId, transactionId);
    YASSERT(~node != NULL);
    return node;
}

IStringNode::TPtr TCypressManager::CreateStringNode(const TTransactionId& transactionId)
{
    return ~CreateNode<TStringNode, TStringNodeProxy>(transactionId);
}

IInt64Node::TPtr TCypressManager::CreateInt64Node(const TTransactionId& transactionId)
{
    return ~CreateNode<TInt64Node, TInt64NodeProxy>(transactionId);
}

IDoubleNode::TPtr TCypressManager::CreateDoubleNode(const TTransactionId& transactionId)
{
    return ~CreateNode<TDoubleNode, TDoubleNodeProxy>(transactionId);
}

IMapNode::TPtr TCypressManager::CreateMapNode(const TTransactionId& transactionId)
{
    return ~CreateNode<TMapNode, TMapNodeProxy>(transactionId);
}

TLock* TCypressManager::CreateLock(const TNodeId& nodeId, const TTransactionId& transactionId)
{
    auto id = LockIdGenerator.Next();
    auto* lock = new TLock(id, nodeId, transactionId, ELockMode::ExclusiveWrite);
    LockMap.Insert(id, lock);
    auto& transaction = TransactionManager->GetTransactionForUpdate(transactionId);
    transaction.LockIds().push_back(lock->GetId());
    return lock;
}

ICypressNode& TCypressManager::BranchNode(const ICypressNode& node, const TTransactionId& transactionId)
{
    YASSERT(!node.GetId().IsBranched());
    auto nodeId = node.GetId().NodeId;
    auto branchedNode = node.Branch(transactionId);
    branchedNode->SetState(ENodeState::Branched);
    auto& transaction = TransactionManager->GetTransactionForUpdate(transactionId);
    transaction.BranchedNodeIds().push_back(nodeId);
    auto* branchedNodePtr = branchedNode.Release();
    NodeMap.Insert(TBranchedNodeId(nodeId, transactionId), branchedNodePtr);
    return *branchedNodePtr;
}

void TCypressManager::GetYPath(
    const TTransactionId& transactionId,
    TYPath path,
    IYsonConsumer* consumer)
{
    auto root = GetNode(RootNodeId, transactionId);
    NYTree::GetYPath(AsYPath(root), path, consumer);
}

void TCypressManager::SetYPath(
    const TTransactionId& transactionId,
    TYPath path,
    TYsonProducer::TPtr producer )
{
    auto root = GetNode(RootNodeId, transactionId);
    NYTree::SetYPath(AsYPath(root), path, producer);
}

TVoid TCypressManager::SetYPath(const NProto::TMsgSet& message)
{
    auto transactionId = TTransactionId::FromProto(message.GetTransactionId());
    auto path = message.GetPath();
    TStringInput inputStream(message.GetValue());
    auto producer = TYsonReader::GetProducer(&inputStream);
    SetYPath(transactionId, path, producer);
    return TVoid();
}

void TCypressManager::RemoveYPath(
    const TTransactionId& transactionId,
    TYPath path)
{
    auto root = GetNode(RootNodeId, transactionId);
    NYTree::RemoveYPath(AsYPath(root), path);
}

TVoid TCypressManager::RemoveYPath(const NProto::TMsgRemove& message)
{
    auto transactionId = TTransactionId::FromProto(message.GetTransactionId());
    auto path = message.GetPath();
    RemoveYPath(transactionId, path);
    return TVoid();
}

void TCypressManager::LockYPath(const TTransactionId& transactionId, TYPath path)
{
    auto root = GetNode(RootNodeId, transactionId);
    NYTree::LockYPath(AsYPath(root), path);
}

NYT::TVoid TCypressManager::LockYPath(const NProto::TMsgLock& message)
{
    auto transactionId = TTransactionId::FromProto(message.GetTransactionId());
    auto path = message.GetPath();
    LockYPath(transactionId, path);
    return TVoid();
}

Stroka TCypressManager::GetPartName() const
{
    return "Cypress";
}

TFuture<TVoid>::TPtr TCypressManager::Save(TOutputStream* stream, IInvoker::TPtr invoker)
{
    YUNIMPLEMENTED();
    //*stream << NodeIdGenerator
    //        << LockIdGenerator;
}

TFuture<TVoid>::TPtr TCypressManager::Load(TInputStream* stream, IInvoker::TPtr invoker)
{
    YUNIMPLEMENTED();
    //*stream >> NodeIdGenerator
    //        >> LockIdGenerator;
}

void TCypressManager::Clear()
{
    TBranchedNodeId id(RootNodeId, NullTransactionId);
    auto* root = new TMapNode(id);
    root->SetState(ENodeState::Committed);
    NodeMap.Insert(id, root);
}

void TCypressManager::OnTransactionCommitted(TTransaction& transaction)
{
    ReleaseLocks(transaction);
    MergeBranchedNodes(transaction);
    CommitCreatedNodes(transaction);
}

void TCypressManager::OnTransactionAborted(TTransaction& transaction)
{
    ReleaseLocks(transaction);
    RemoveBranchedNodes(transaction);
    RemoveCreatedNodes(transaction);
}

void TCypressManager::ReleaseLocks(TTransaction& transaction)
{
    // Iterate over all locks created by the transaction.
    FOREACH (const auto& lockId, transaction.LockIds()) {
        const auto& lock = LockMap.Get(lockId);

        // Walk up to the root and remove the locks.
        auto currentNodeId = lock.GetNodeId();
        while (currentNodeId != NullNodeId) {
            auto& node = NodeMap.GetForUpdate(TBranchedNodeId(currentNodeId, NullTransactionId));
            YVERIFY(node.LockIds().erase(lockId) == 1);
            currentNodeId = node.GetParentId();
        }
        LockMap.Remove(lockId);
    }
}

void TCypressManager::MergeBranchedNodes(TTransaction& transaction)
{
    auto transactionId = transaction.GetId();
    FOREACH (const auto& nodeId, transaction.BranchedNodeIds()) {
        auto& node = NodeMap.GetForUpdate(TBranchedNodeId(nodeId, NullTransactionId));
        YASSERT(node.GetState() == ENodeState::Committed);
        auto& branchedNode = NodeMap.GetForUpdate(TBranchedNodeId(nodeId, transactionId));
        YASSERT(branchedNode.GetState() == ENodeState::Branched);
        node.Merge(branchedNode);
        NodeMap.Remove(TBranchedNodeId(nodeId, transactionId));
    }
}

void TCypressManager::RemoveBranchedNodes(TTransaction& transaction)
{
    auto transactionId = transaction.GetId();
    FOREACH (const auto& nodeId, transaction.BranchedNodeIds()) {
        NodeMap.Remove(TBranchedNodeId(nodeId, transactionId));
    }
}

void TCypressManager::CommitCreatedNodes(TTransaction& transaction)
{
    FOREACH (const auto& nodeId, transaction.CreatedNodeIds()) {
        auto& node = NodeMap.GetForUpdate(TBranchedNodeId(nodeId, NullTransactionId));
        node.SetState(ENodeState::Committed);
    }
}

void TCypressManager::RemoveCreatedNodes(TTransaction& transaction)
{
    FOREACH (const auto& nodeId, transaction.CreatedNodeIds()) {
        NodeMap.Remove(TBranchedNodeId(nodeId, NullTransactionId));
    }
}

METAMAP_ACCESSORS_IMPL(TCypressManager, Lock, TLock, TLockId, LockMap);
METAMAP_ACCESSORS_IMPL(TCypressManager, Node, ICypressNode, TBranchedNodeId, NodeMap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypress
} // namespace NYT
