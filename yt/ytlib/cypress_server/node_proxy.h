#pragma once

#include "public.h"
#include "node.h"

#include <ytlib/ytree/public.h>
#include <ytlib/object_server/object_proxy.h>

namespace NYT {
namespace NCypressServer {

////////////////////////////////////////////////////////////////////////////////

//! Extends NYTree::INode by adding functionality that is common to all
//! logical Cypress nodes.
struct ICypressNodeProxy
    : public virtual NYTree::INode
    , public virtual NObjectServer::IObjectProxy
{
    //! Returns the id of the transaction for which the proxy is created.
    virtual TTransactionId GetTransactionId() const = 0;

    //! Constructs a deep copy of the node.
    virtual ICypressNodeProxyPtr Clone() = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT
