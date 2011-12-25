#pragma once

#include "common.h"

#include "../misc/cache.h"
#include "../chunk_client/block_cache.h"

namespace NYT {
namespace NChunkHolder {

////////////////////////////////////////////////////////////////////////////////

//! Represents a cached block of chunk.
class TCachedBlock
    : public TCacheValueBase<NChunkClient::TBlockId, TCachedBlock>
{
public:
    typedef TIntrusivePtr<TCachedBlock> TPtr;

    //! Constructs a new block from id and data.
    TCachedBlock(const NChunkClient::TBlockId& blockId, const TSharedRef& data);

    ~TCachedBlock();

    //! Returns block data.
    TSharedRef GetData() const;

private:
    TSharedRef Data;

};

////////////////////////////////////////////////////////////////////////////////

class TChunkStore;
class TChunkCache;
class TReaderCache;

//! Manages cached blocks.
class TBlockStore
    : public TRefCountedBase
{
public:
    typedef TIntrusivePtr<TBlockStore> TPtr;

    //! Constructs a store.
    TBlockStore(
        TChunkHolderConfig* config,
        TChunkStore* chunkStore,
        TChunkCache* chunkCache,
        TReaderCache* readerCache);

    typedef TValueOrError<TCachedBlock::TPtr> TGetBlockResult;
    typedef TFuture<TGetBlockResult> TAsyncGetBlockResult;

    //! Gets (asynchronously) a block from the store.
    /*!
     * This call returns an async result that becomes set when the 
     * block is fetched. Fetching an already-cached block is cheap
     * (i.e. requires no context switch). Fetching an uncached block
     * enqueues a disk-read action to the appropriate IO queue.
     */
    TAsyncGetBlockResult::TPtr GetBlock(const NChunkClient::TBlockId& blockId);

    //! Puts a block into the store.
    /*!
     *  The store may already have another copy of the same block.
     *  In this case the block content is checked for identity.
     */
    TCachedBlock::TPtr PutBlock(const NChunkClient::TBlockId& blockId, const TSharedRef& data);

    //! Returns the number of bytes that are scheduled for disk read IO.
    i64 GetPendingReadSize() const;

    //! Returns a caching adapter.
    NChunkClient::IBlockCache* GetCache();

private:
    class TStoreImpl;
    friend class TStoreImpl;

    class TCacheImpl;

    TIntrusivePtr<TStoreImpl> StoreImpl;
    TIntrusivePtr<TCacheImpl> CacheImpl;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT

