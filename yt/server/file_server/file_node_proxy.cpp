#include "stdafx.h"
#include "file_node_proxy.h"
#include "file_node.h"

#include <core/misc/string.h>

#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/chunk.pb.h>

#include <server/chunk_server/chunk.h>
#include <server/chunk_server/chunk_list.h>
#include <server/chunk_server/chunk_manager.h>
#include <server/chunk_server/chunk_owner_node_proxy.h>

namespace NYT {
namespace NFileServer {

using namespace NChunkServer;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NCypressServer;
using namespace NYTree;
using namespace NYson;
using namespace NTransactionServer;

using NChunkClient::TChannel;

////////////////////////////////////////////////////////////////////////////////

class TFileNodeProxy
    : public TCypressNodeProxyBase<TChunkOwnerNodeProxy, IEntityNode, TFileNode>
{
public:
    TFileNodeProxy(
        INodeTypeHandlerPtr typeHandler,
        NCellMaster::TBootstrap* bootstrap,
        TTransaction* transaction,
        TFileNode* trunkNode)
        : TBase(
            typeHandler,
            bootstrap,
            transaction,
            trunkNode)
    { }

private:
    typedef TCypressNodeProxyBase<TChunkOwnerNodeProxy, IEntityNode, TFileNode> TBase;

    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) override
    {
        attributes->push_back(TAttributeInfo("executable", true, false, true));
        attributes->push_back(TAttributeInfo("file_name", true, false, true));
        TBase::ListSystemAttributes(attributes);
    }

    virtual void ValidateCustomAttributeUpdate(
        const Stroka& key,
        const TNullable<TYsonString>& oldValue,
        const TNullable<TYsonString>& newValue) override
    {
        UNUSED(oldValue);

        if (key == "executable" && newValue) {
            ConvertTo<bool>(*newValue);
            return;
        }

        if (key == "file_name" && newValue) {
            ConvertTo<Stroka>(*newValue);
            return;
        }

        TBase::ValidateCustomAttributeUpdate(key, oldValue, newValue);
    }

    virtual ELockMode GetLockMode(EUpdateMode updateMode) override
    {
        return ELockMode::Exclusive;
    }

    virtual void ValidatePathAttributes(
        const TNullable<TChannel>& channel,
        const TReadLimit& upperLimit,
        const TReadLimit& lowerLimit) override
    {
        if (channel) {
            THROW_ERROR_EXCEPTION("Column selectors are not supported for files");
        }

        if (upperLimit.has_key() || upperLimit.has_row_index() ||
            lowerLimit.has_key() || lowerLimit.has_row_index())
        {
            THROW_ERROR_EXCEPTION("Row selectors are not supported for files");
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

ICypressNodeProxyPtr CreateFileNodeProxy(
    INodeTypeHandlerPtr typeHandler,
    NCellMaster::TBootstrap* bootstrap,
    TTransaction* transaction,
    TFileNode* trunkNode)
{

    return New<TFileNodeProxy>(
        typeHandler,
        bootstrap,
        transaction,
        trunkNode);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileServer
} // namespace NYT
