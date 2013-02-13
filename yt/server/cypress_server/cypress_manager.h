#pragma once

#include "public.h"
#include "node.h"
#include "type_handler.h"
#include "node_proxy.h"
#include "lock.h"

#include <ytlib/misc/small_vector.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/id_generator.h>

#include <ytlib/ytree/ypath_service.h>
#include <ytlib/ytree/tree_builder.h>

#include <ytlib/meta_state/meta_state_manager.h>
#include <ytlib/meta_state/composite_meta_state.h>
#include <ytlib/meta_state/map.h>
#include <ytlib/meta_state/mutation.h>

#include <server/object_server/object_manager.h>

#include <server/cell_master/public.h>

#include <server/transaction_server/transaction.h>
#include <server/transaction_server/transaction_manager.h>

#include <server/security_server/public.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

class TCypressManager
    : public NMetaState::TMetaStatePart
{
public:
    explicit TCypressManager(NCellMaster::TBootstrap* bootstrap);

    void Initialize();

    void RegisterHandler(INodeTypeHandlerPtr handler);
    INodeTypeHandlerPtr FindHandler(NObjectClient::EObjectType type);
    INodeTypeHandlerPtr GetHandler(NObjectClient::EObjectType type);
    INodeTypeHandlerPtr GetHandler(const TCypressNodeBase* node);

    typedef NRpc::TTypedServiceRequest<NCypressClient::NProto::TReqCreate> TReqCreate;
    typedef NRpc::TTypedServiceResponse<NCypressClient::NProto::TRspCreate> TRspCreate;

    //! Creates a new node.
    /*!
     *  The call does the following:
     *  - Creates a new node.
     *  - Sets its attributes.
     *  - Registers the new node.
     *  - Locks it with exclusive mode.
     */
    TCypressNodeBase* CreateNode(
        INodeTypeHandlerPtr handler,
        NTransactionServer::TTransaction* transaction,
        NSecurityServer::TAccount* account,
        NYTree::IAttributeDictionary* attributes,
        TReqCreate* request,
        TRspCreate* response);

    //! Clones a node.
    /*!
     *  The call does the following:
     *  - Creates a clone of #sourceNode.
     *  - Registers the cloned node.
     *  - Locks the cloned node with exclusive mode.
     */
    TCypressNodeBase* CloneNode(
        TCypressNodeBase* sourceNode,
        NTransactionServer::TTransaction* transaction);

    //! Returns the root node.
    TCypressNodeBase* GetRootNode() const;

    //! Returns a service representing the root.
    //! This service is fully thread-safe.
    NYTree::IYPathServicePtr GetRootService() const;

    //! Creates a resolver that provides a view in the context of a given transaction.
    NYTree::IYPathResolverPtr CreateResolver(NTransactionServer::TTransaction* transaction = nullptr);

    //! Similar to |FindNode| provided by |DECLARE_METAMAP_ACCESSORS| but
    //! specially optimized for the case of null transaction.
    TCypressNodeBase* FindNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    TCypressNodeBase* GetVersionedNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    ICypressNodeProxyPtr GetVersionedNodeProxy(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction = nullptr);

    TCypressNodeBase* LockVersionedNode(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request,
        bool recursive = false);

    void SetModified(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    void RegisterNode(
        TAutoPtr<TCypressNodeBase> node,
        NTransactionServer::TTransaction* transaction,
        NYTree::IAttributeDictionary* attributes = nullptr);

    DECLARE_METAMAP_ACCESSORS(Node, TCypressNodeBase, TVersionedNodeId);

private:
    typedef TCypressManager TThis;

    class TNodeTypeHandler;
    class TYPathResolver;
    class TRootService;

    class TNodeMapTraits
    {
    public:
        explicit TNodeMapTraits(TCypressManager* cypressManager);

        TAutoPtr<TCypressNodeBase> Create(const TVersionedNodeId& id) const;

    private:
        TCypressManager* CypressManager;

    };

    NCellMaster::TBootstrap* Bootstrap;

    NMetaState::TMetaStateMap<TVersionedNodeId, TCypressNodeBase, TNodeMapTraits> NodeMap;

    std::vector<INodeTypeHandlerPtr> TypeToHandler;

    TNodeId RootNodeId;
    TCypressNodeBase* RootNode;
    NYTree::IYPathServicePtr RootService;

    yhash_map<TCypressNodeBase*, INodeBehaviorPtr> NodeBehaviors;

    void DestroyNode(TCypressNodeBase* trunkNode);

    // TMetaStatePart overrides.
    virtual void OnLeaderRecoveryComplete() override;
    virtual void OnStopLeading() override;
    virtual void Clear() override;

    void SaveKeys(const NCellMaster::TSaveContext& context) const;
    void SaveValues(const NCellMaster::TSaveContext& context) const;
    void LoadKeys(const NCellMaster::TLoadContext& context);
    void LoadValues(const NCellMaster::TLoadContext& context);

    void OnTransactionCommitted(NTransactionServer::TTransaction* transaction);
    void OnTransactionAborted(NTransactionServer::TTransaction* transaction);

    void ReleaseLocks(NTransactionServer::TTransaction* transaction);
    void MergeNodes(NTransactionServer::TTransaction* transaction);
    void MergeNode(
        NTransactionServer::TTransaction* transaction,
        TCypressNodeBase* branchedNode);
    void RemoveBranchedNodes(NTransactionServer::TTransaction* transaction);
    void RemoveBranchedNode(TCypressNodeBase* branchedNode);
    void ReleaseCreatedNodes(NTransactionServer::TTransaction* transaction);
    void PromoteLocks(NTransactionServer::TTransaction* transaction);
    void PromoteLock(TLock* lock, NTransactionServer::TTransaction* parentTransaction);

    void CreateNodeBehavior(TCypressNodeBase* trunkNode);
    void DestroyNodeBehavior(TCypressNodeBase* trunkNode);

    void ValidateLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request,
        bool* isMandatory);
    void ValidateLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request);
    bool IsRedundantLock(
        const TLock& existingLock,
        const TLockRequest& request);

    static bool IsParentTransaction(
        NTransactionServer::TTransaction* transaction,
        NTransactionServer::TTransaction* parent);
    static bool IsConcurrentTransaction(
        NTransactionServer::TTransaction* transaction1,
        NTransactionServer::TTransaction* transaction2);

    TCypressNodeBase* AcquireLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request);
    TLock* DoAcquireLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        const TLockRequest& request);
    void ReleaseLock(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction);

    typedef TSmallVector<TCypressNodeBase*, 1> TSubtreeNodes;
    void ListSubtreeNodes(
        TCypressNodeBase* trunkNode,
        NTransactionServer::TTransaction* transaction,
        TSubtreeNodes* nodes);

   TCypressNodeBase* BranchNode(
       TCypressNodeBase* originatingNode,
       NTransactionServer::TTransaction* transaction,
       ELockMode mode);

    NYPath::TYPath GetNodePath(
       TCypressNodeBase* trunkNode,
       NTransactionServer::TTransaction* transaction);

    DECLARE_THREAD_AFFINITY_SLOT(StateThread);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
