#include "stdafx.h"
#include "block_store.h"
#include "private.h"
#include "chunk.h"
#include "config.h"
#include "chunk_registry.h"
#include "reader_cache.h"
#include "location.h"
#include "chunk_meta_extensions.h"

#include <ytlib/chunk_holder/chunk.pb.h>
#include <ytlib/chunk_client/file_reader.h>
#include <ytlib/chunk_client/block_cache.h>

namespace NYT {
namespace NChunkHolder {

using namespace NChunkClient;
using namespace NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = ChunkHolderLogger;

////////////////////////////////////////////////////////////////////////////////

TCachedBlock::TCachedBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const Stroka& source)
    : TCacheValueBase<TBlockId, TCachedBlock>(blockId)
    , Data_(data)
    , Source_(source)
{ }

TCachedBlock::~TCachedBlock()
{
    LOG_DEBUG("Purged cached block (BlockId: %s)", ~GetKey().ToString());
}

////////////////////////////////////////////////////////////////////////////////

class TBlockStore::TStoreImpl 
    : public TWeightLimitedCache<TBlockId, TCachedBlock>
{
public:
    DEFINE_BYVAL_RO_PROPERTY(TAtomic, PendingReadSize);

    TStoreImpl(
        TChunkHolderConfigPtr config,
        TChunkRegistryPtr chunkRegistry,
        TReaderCachePtr readerCache)
        : TWeightLimitedCache<TBlockId, TCachedBlock>(config->MaxCachedBlocksSize)
        , ChunkRegistry(chunkRegistry)
        , ReaderCache(readerCache)
        , PendingReadSize_(0)
    { }

    TCachedBlockPtr Put(const TBlockId& blockId, const TSharedRef& data, const Stroka& source)
    {
        while (true) {
            TInsertCookie cookie(blockId);
            if (BeginInsert(&cookie)) {
                auto block = New<TCachedBlock>(blockId, data, source);
                cookie.EndInsert(block);

                LOG_DEBUG("Block is put into cache (BlockId: %s, BlockSize: %" PRISZT ")",
                    ~blockId.ToString(),
                    data.Size());

                return block;
            }

            auto result = cookie.GetValue().Get();
            if (!result.IsOK()) {
                // Looks like a parallel Get request has completed unsuccessfully.
                continue;
            }

            // This is a cruel reality.
            // Since we never evict blocks of removed chunks from the cache
            // it is possible for a block to be put there more than once.
            // We shall reuse the cached copy but for sanity's sake let's
            // check that the content is the same.
            auto block = result.Value();

            if (!TRef::CompareContent(data, block->GetData())) {
                LOG_FATAL("Trying to cache a block for which a different cached copy already exists (BlockId: %s)",
                    ~blockId.ToString());
            }
            
            LOG_DEBUG("Block is resurrected in cache (BlockId: %s)", ~blockId.ToString());

            return block;
        }
    }

    TAsyncGetBlockResult Get(const TBlockId& blockId)
    {
        TSharedPtr<TInsertCookie> cookie(new TInsertCookie(blockId));
        if (!BeginInsert(~cookie)) {
            LOG_DEBUG("Block cache hit (BlockId: %s)", ~blockId.ToString());
            return cookie->GetValue();
        }

        auto chunk = ChunkRegistry->FindChunk(blockId.ChunkId);
        if (!chunk) {
            cookie->Cancel(TError(
                TChunkHolderServiceProxy::EErrorCode::NoSuchChunk,
                Sprintf("No such chunk (ChunkId: %s)", ~blockId.ChunkId.ToString())));
            return cookie->GetValue();
        }
     
        LOG_DEBUG("Block cache miss (BlockId: %s)", ~blockId.ToString());

        auto invoker = chunk->GetLocation()->GetInvoker();
        invoker->Invoke(BIND(
            &TStoreImpl::DoReadBlock,
            MakeStrong(this),
            chunk,
            blockId,
            cookie));
        
        return cookie->GetValue();
    }

    TCachedBlockPtr Find(const TBlockId& blockId)
    {
        auto asyncResult = Lookup(blockId);
        if (asyncResult.IsNull()) {
            LOG_DEBUG("Block cache miss (BlockId: %s)", ~blockId.ToString());
            return NULL;            
        }

        TNullable<TGetBlockResult> maybeResult = asyncResult.TryGet();
        if (maybeResult && maybeResult->IsOK()) {
            LOG_DEBUG("Block cache hit (BlockId: %s)", ~blockId.ToString());
            return maybeResult->Value();
        } else {
            LOG_DEBUG("Block cache miss (BlockId: %s)", ~blockId.ToString());
            return NULL;
        }
    }

private:
    TChunkRegistryPtr ChunkRegistry;
    TReaderCachePtr ReaderCache;

    virtual i64 GetWeight(TCachedBlock* block) const
    {
        return block->GetData().Size();
    }

    void DoReadBlock(
        TChunkPtr chunk,
        const TBlockId& blockId,
        TSharedPtr<TInsertCookie> cookie)
    {
        auto readerResult = ReaderCache->GetReader(chunk);
        if (!readerResult.IsOK()) {
            cookie->Cancel(readerResult);
            return;
        }

        auto reader = readerResult.Value();

        const auto& chunkMeta = reader->GetChunkMeta();
        const auto blocksExt = GetProtoExtension<TBlocksExt>(chunkMeta.extensions());
        const auto& blockInfo = blocksExt.blocks(blockId.BlockIndex);
        auto blockSize = blockInfo.size();
        
        AtomicAdd(PendingReadSize_, blockSize);
        LOG_DEBUG("Pending read size increased (BlockSize: %d, PendingReadSize: %" PRISZT,
            blockSize,
            PendingReadSize_);

        TSharedRef data;
        try {
            data = reader->ReadBlock(blockId.BlockIndex);
        } catch (const std::exception& ex) {
            LOG_FATAL("Error reading chunk block (BlockId: %s)\n%s",
                ~blockId.ToString(),
                ex.what());
        }

        AtomicSub(PendingReadSize_, blockSize);
        LOG_DEBUG("Pending read size decreased (BlockSize: %d, PendingReadSize: %" PRISZT,
            blockSize,
            PendingReadSize_);

        if (!data) {
            cookie->Cancel(TError(
                TChunkHolderServiceProxy::EErrorCode::NoSuchBlock,
                Sprintf("No such block (BlockId: %s)", ~blockId.ToString())));
            return;
        }

        auto block = New<TCachedBlock>(blockId, data, Stroka());
        cookie->EndInsert(block);

        LOG_DEBUG("Finished loading block into cache (BlockId: %s)", ~blockId.ToString());
    }
};

////////////////////////////////////////////////////////////////////////////////

class TBlockStore::TCacheImpl
    : public IBlockCache
{
public:
    TCacheImpl(TIntrusivePtr<TStoreImpl> storeImpl)
        : StoreImpl(storeImpl)
    { }

    void Put(const TBlockId& id, const TSharedRef& data, const Stroka& source)
    {
        StoreImpl->Put(id, data, source);
    }

    TSharedRef Find(const TBlockId& id)
    {
        auto block = StoreImpl->Find(id);
        return block ? block->GetData() : TSharedRef();
    }

private:
    TIntrusivePtr<TStoreImpl> StoreImpl;

};

////////////////////////////////////////////////////////////////////////////////

TBlockStore::TBlockStore(
    TChunkHolderConfigPtr config,
    TChunkRegistryPtr chunkRegistry,
    TReaderCachePtr readerCache)
    : StoreImpl(New<TStoreImpl>(
        config,
        chunkRegistry,
        readerCache))
    , CacheImpl(New<TCacheImpl>(~StoreImpl))
{ }

TBlockStore::~TBlockStore()
{ }

TBlockStore::TAsyncGetBlockResult TBlockStore::GetBlock(const TBlockId& blockId)
{
    return StoreImpl->Get(blockId);
}

TCachedBlockPtr TBlockStore::FindBlock(const TBlockId& blockId)
{
    return StoreImpl->Find(blockId);
}

TCachedBlockPtr TBlockStore::PutBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const Stroka& source)
{
    return StoreImpl->Put(blockId, data, source);
}

i64 TBlockStore::GetPendingReadSize() const
{
    return StoreImpl->GetPendingReadSize();
}

IBlockCachePtr TBlockStore::GetBlockCache()
{
    return CacheImpl;
}

std::vector<TCachedBlockPtr> TBlockStore::GetAllBlocks() const
{
    return StoreImpl->GetAll();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkHolder
} // namespace NYT
