#pragma once

#include "yson_consumer.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

//! Reconstructs a YTree from IYsonConsumer calls.
struct ITreeBuilder
    : public virtual IYsonConsumer
{
    //! Resets the instance.
    virtual void BeginTree() = 0;

    //! Returns the root node of the constructed tree.
    /*!
     *  \note
     *  Must be called after the tree is fully constructed.
     */
    virtual INodePtr EndTree() = 0;


    //! Enables inserting a pre-existing subtree into
    //! the currently constructed one.
    /*!
     *  The given subtree is injected as-is, no cloning is done.
     */
    virtual void OnNode(INode* node) = 0;
};

//! Creates a builder that makes explicit calls to the factory.
/*!
 *  \param factory A factory used for materializing the nodes.
 */
TAutoPtr<ITreeBuilder> CreateBuilderFromFactory(INodeFactory* factory);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

