#pragma once

#include "public.h"

#include <yt/server/cell_node/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/core/actions/future.h>

#include <yt/core/misc/optional.h>
#include <yt/core/misc/async_cache.h>
#include <yt/core/misc/property.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

//! Represents a cached chunk meta.
class TCachedChunkMeta
    : public TAsyncCacheValueBase<TChunkId, TCachedChunkMeta>
{
public:
    DEFINE_BYVAL_RO_PROPERTY(NChunkClient::TRefCountedChunkMetaPtr, Meta);

public:
    TCachedChunkMeta(
        TChunkId chunkId,
        NChunkClient::TRefCountedChunkMetaPtr meta,
        NCellNode::TNodeMemoryTracker* memoryTracker);

    i64 GetSize() const;

private:
    // NB: Avoid including TMemoryUsageTracker here.
    std::unique_ptr<NCellNode::TNodeMemoryTrackerGuard> MemoryTrackerGuard_;

};

DEFINE_REFCOUNTED_TYPE(TCachedChunkMeta)

using TCachedChunkMetaCookie = TAsyncSlruCacheBase<TChunkId, TCachedChunkMeta>::TInsertCookie;

////////////////////////////////////////////////////////////////////////////////

//! Manages (in particular, caches) metas of chunks stored at Data Node.
/*!
 *  \note
 *  Thread affinity: any
 */
class TChunkMetaManager
    : public TRefCounted
{
public:
    TChunkMetaManager(
        TDataNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap);

    ~TChunkMetaManager();

    //! Returns cached chunk meta if prevent in the cache; if not then returns null.
    NChunkClient::TRefCountedChunkMetaPtr FindCachedMeta(TChunkId chunkId);

    //! Puts chunk meta into the cache.
    /*!
     *  Typically invoked when chunk session finishes and the meta is ready at hand.
     */
    void PutCachedMeta(
        TChunkId chunkId,
        NChunkClient::TRefCountedChunkMetaPtr meta);

    //! Starts an asynchronous chunk meta load.
    /*!
     *  See TAsyncCacheValueBase for more details.
     */
    TCachedChunkMetaCookie BeginInsertCachedMeta(TChunkId chunkId);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TChunkMetaManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
