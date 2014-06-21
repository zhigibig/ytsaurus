#include "stdafx.h"
#include "block_store.h"
#include "private.h"
#include "chunk.h"
#include "config.h"
#include "chunk_registry.h"
#include "blob_reader_cache.h"
#include "location.h"

#include <ytlib/chunk_client/file_reader.h>
#include <ytlib/chunk_client/block_cache.h>
#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/data_node_service_proxy.h>
#include <ytlib/chunk_client/chunk_meta.pb.h>

#include <server/cell_node/bootstrap.h>

#include <core/concurrency/parallel_awaiter.h>

namespace NYT {
namespace NDataNode {

using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NCellNode;

using NChunkClient::NProto::TChunkMeta;
using NChunkClient::NProto::TBlocksExt;

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = DataNodeLogger;
static auto& Profiler = DataNodeProfiler;

static NProfiling::TRateCounter CacheReadThroughputCounter("/cache_read_throughput");

////////////////////////////////////////////////////////////////////////////////

TCachedBlock::TCachedBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const TNullable<TNodeDescriptor>& source)
    : TCacheValueBase<TBlockId, TCachedBlock>(blockId)
    , Data_(data)
    , Source_(source)
{ }

TCachedBlock::~TCachedBlock()
{
    LOG_DEBUG("Cached block purged (BlockId: %s)", ~ToString(GetKey()));
}

////////////////////////////////////////////////////////////////////////////////

class TBlockStore::TStoreImpl
    : public TWeightLimitedCache<TBlockId, TCachedBlock>
{
public:
    TStoreImpl(
        TDataNodeConfigPtr config,
        TBootstrap* bootstrap)
        : TWeightLimitedCache<TBlockId, TCachedBlock>(config->BlockCacheSize)
        , Config_(config)
        , Bootstrap_(bootstrap)
        , PendingReadSize_(0)
    { }

    void Initialize()
    {
        auto result = Bootstrap_->GetMemoryUsageTracker()->TryAcquire(
            NCellNode::EMemoryConsumer::BlockCache,
            Config_->BlockCacheSize);
        THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error reserving memory for block cache");
    }

    void PutBlock(
        const TBlockId& blockId,
        const TSharedRef& data,
        const TNullable<TNodeDescriptor>& source)
    {
        while (true) {
            TInsertCookie cookie(blockId);
            if (BeginInsert(&cookie)) {
                auto block = New<TCachedBlock>(blockId, data, source);
                cookie.EndInsert(block);

                LOG_DEBUG("Block is put into cache (BlockId: %s, Size: %" PRISZT ", SourceAddress: %s)",
                    ~ToString(blockId),
                    data.Size(),
                    ~ToString(source));
                return;
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

            if (!TRef::AreBitwiseEqual(data, block->GetData())) {
                LOG_FATAL("Trying to cache block %s for which a different cached copy already exists",
                    ~ToString(blockId));
            }

            LOG_DEBUG("Block is resurrected in cache (BlockId: %s)",
                ~ToString(blockId));
            return;
        }
    }

    TFuture<TGetBlockResult> GetBlock(
        const TChunkId& chunkId,
        int blockIndex,
        i64 priority,
        bool enableCaching)
    {
        // During block peering, data nodes exchange individual blocks.
        // Thus the cache may contain a block not bound to any chunk in the registry.
        // Handle these "unbounded" blocks first.
        // If none is found then look for the owning chunk.
        TBlockId blockId(chunkId, blockIndex);
        auto cachedBlock = FindBlock(blockId);
        if (cachedBlock) {
            return MakeFuture<TGetBlockResult>(cachedBlock->GetData());
        }
        
        TInsertCookie cookie(blockId);
        if (enableCaching) {
            if (!BeginInsert(&cookie)) {
                auto this_ = MakeStrong(this);
                return cookie.GetValue().Apply(BIND([this, this_] (TErrorOr<TCachedBlockPtr> result) -> TGetBlockResult {
                    if (!result.IsOK()) {
                        return TError(result);
                    }
                    auto cachedBlock = result.Value();
                    LogCacheHit(cachedBlock);
                    return cachedBlock->GetData();
                }));
            }
        }

        auto chunkRegistry = Bootstrap_->GetChunkRegistry();
        auto chunk = chunkRegistry->FindChunk(chunkId);
        if (!chunk) {
            return MakeFuture<TGetBlockResult>(TSharedRef());
        }

        if (!chunk->TryAcquireReadLock()) {
            return MakeFuture<TGetBlockResult>(TError(
                "Cannot read chunk %s since it is scheduled for removal",
                ~ToString(chunkId)));
        }

        return chunk
            ->ReadBlocks(blockIndex, 1, priority)
            .Apply(BIND([=] (IChunk::TReadBlocksResult result) -> TGetBlockResult {
                chunk->ReleaseReadLock();
                if (!result.IsOK()) {
                    return TError(result);
                }
                return result.Value()[0];
            }));
    }

    TFuture<TGetBlocksResult> GetBlocks(
        const TChunkId& chunkId,
        int firstBlockIndex,
        int blockCount,
        i64 priority)
    {
        // NB: Range requests bypass block cache.

        auto chunkRegistry = Bootstrap_->GetChunkRegistry();
        auto chunk = chunkRegistry->FindChunk(chunkId);
        if (!chunk) {
            return MakeFuture<TGetBlocksResult>(std::vector<TSharedRef>());
        }

        if (!chunk->TryAcquireReadLock()) {
            return MakeFuture<TGetBlocksResult>(TError(
                "Cannot read chunk %s since it is scheduled for removal",
                ~ToString(chunkId)));
        }

        return chunk
            ->ReadBlocks(firstBlockIndex, blockCount, priority)
            .Apply(BIND([=] (IChunk::TReadBlocksResult result) -> TGetBlocksResult {
                chunk->ReleaseReadLock();
                return result;
            }));
    }

    TCachedBlockPtr FindBlock(const TBlockId& id)
    {
        auto block = TWeightLimitedCache::Find(id);
        if (block) {
            LogCacheHit(block);
        }
        return block;
    }

    i64 GetPendingReadSize() const
    {
        return PendingReadSize_;
    }

    void UpdatePendingReadSize(i64 delta)
    {
        i64 result = (PendingReadSize_ += delta);
        LOG_DEBUG("Pending read size updated (PendingReadSize: %" PRId64 ", Delta: %" PRId64")",
            result,
            delta);
    }

private:
    friend class TGetBlocksSession;

    TDataNodeConfigPtr Config_;
    TBootstrap* Bootstrap_;

    std::atomic<i64> PendingReadSize_;


    virtual i64 GetWeight(TCachedBlock* block) const override
    {
        return block->GetData().Size();
    }

    void LogCacheHit(TCachedBlockPtr block)
    {
        Profiler.Increment(CacheReadThroughputCounter, block->GetData().Size());
        LOG_DEBUG("Block cache hit (BlockId: %s)",
            ~ToString(block->GetKey()));
    }

};

////////////////////////////////////////////////////////////////////////////////

class TBlockStore::TCacheImpl
    : public IBlockCache
{
public:
    explicit TCacheImpl(TIntrusivePtr<TStoreImpl> storeImpl)
        : StoreImpl_(storeImpl)
    { }

    virtual void Put(
        const TBlockId& id,
        const TSharedRef& data,
        const TNullable<TNodeDescriptor>& source) override
    {
        StoreImpl_->PutBlock(id, data, source);
    }

    virtual TSharedRef Find(const TBlockId& id) override
    {
        auto block = StoreImpl_->Find(id);
        return block ? block->GetData() : TSharedRef();
    }

private:
    TIntrusivePtr<TStoreImpl> StoreImpl_;

};

////////////////////////////////////////////////////////////////////////////////

TBlockStore::TBlockStore(
    TDataNodeConfigPtr config,
    TBootstrap* bootstrap)
    : StoreImpl_(New<TStoreImpl>(config, bootstrap))
    , CacheImpl_(New<TCacheImpl>(StoreImpl_))
{ }

void TBlockStore::Initialize()
{
    StoreImpl_->Initialize();
}

TBlockStore::~TBlockStore()
{ }

TFuture<TBlockStore::TGetBlockResult> TBlockStore::GetBlock(
    const TChunkId& chunkId,
    int blockIndex,
    i64 priority,
    bool enableCaching)
{
    return StoreImpl_->GetBlock(
        chunkId,
        blockIndex,
        priority,
        enableCaching);
}

TFuture<TBlockStore::TGetBlocksResult> TBlockStore::GetBlocks(
    const TChunkId& chunkId,
    int firstBlockIndex,
    int blockCount,
    i64 priority)
{
    return StoreImpl_->GetBlocks(
        chunkId,
        firstBlockIndex,
        blockCount,
        priority);
}

void TBlockStore::PutBlock(
    const TBlockId& blockId,
    const TSharedRef& data,
    const TNullable<TNodeDescriptor>& source)
{
    StoreImpl_->PutBlock(blockId, data, source);
}

i64 TBlockStore::GetPendingReadSize() const
{
    return StoreImpl_->GetPendingReadSize();
}

void TBlockStore::UpdatePendingReadSize(i64 delta)
{
    StoreImpl_->UpdatePendingReadSize(delta);
}

IBlockCachePtr TBlockStore::GetBlockCache()
{
    return CacheImpl_;
}

std::vector<TCachedBlockPtr> TBlockStore::GetAllBlocks() const
{
    return StoreImpl_->GetAll();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT
