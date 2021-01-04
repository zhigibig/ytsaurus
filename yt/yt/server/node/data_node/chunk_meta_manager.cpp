#include "chunk_meta_manager.h"
#include "config.h"
#include "private.h"

#include <yt/server/node/cluster_node/bootstrap.h>
#include <yt/server/node/cluster_node/dynamic_config_manager.h>
#include <yt/server/node/cluster_node/config.h>

#include <yt/client/chunk_client/proto/chunk_meta.pb.h>

#include <yt/ytlib/misc/memory_usage_tracker.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>

namespace NYT::NDataNode {

using namespace NChunkClient;
using namespace NTableClient;
using namespace NNodeTrackerClient;
using namespace NClusterNode;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = DataNodeLogger;

////////////////////////////////////////////////////////////////////////////////

TCachedChunkMeta::TCachedChunkMeta(
    TChunkId chunkId,
    TRefCountedChunkMetaPtr meta)
    : TAsyncCacheValueBase(chunkId)
    , Meta_(std::move(meta))
    , Weight_(Meta_->SpaceUsedLong())
{ }

i64 TCachedChunkMeta::GetWeight() const
{
    return Weight_;
}

////////////////////////////////////////////////////////////////////////////////

TCachedBlocksExt::TCachedBlocksExt(
    TChunkId chunkId,
    TRefCountedBlocksExtPtr blocksExt)
    : TAsyncCacheValueBase(chunkId)
    , BlocksExt_(std::move(blocksExt))
    , Weight_(BlocksExt_->SpaceUsedLong())
{ }

i64 TCachedBlocksExt::GetWeight() const
{
    return Weight_;
}

////////////////////////////////////////////////////////////////////////////////

class TChunkMetaManager
    : public IChunkMetaManager
{
public:
    explicit TChunkMetaManager(TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
        , Config_(Bootstrap_->GetConfig()->DataNode)
        , ChunkMetaCache_(New<TChunkMetaCache>(
            Config_->ChunkMetaCache,
            Bootstrap_
                ->GetMemoryUsageTracker()
                ->WithCategory(EMemoryCategory::ChunkMeta),
            DataNodeProfiler.WithPrefix("/chunk_meta_cache")))
        , BlocksExtCache_(New<TBlocksExtCache>(
            Config_->BlocksExtCache,
            Bootstrap_
                ->GetMemoryUsageTracker()
                ->WithCategory(EMemoryCategory::ChunkBlockMeta),
            DataNodeProfiler.WithPrefix("/blocks_ext_cache")))
        , BlockMetaCache_(New<TBlockMetaCache>(
            Config_->BlockMetaCache,
            Bootstrap_
                ->GetMemoryUsageTracker()
                ->WithCategory(EMemoryCategory::ChunkBlockMeta),
            DataNodeProfiler.WithPrefix("/block_meta_cache")))
    {
        YT_VERIFY(Bootstrap_);

        const auto& dynamicConfigManager = Bootstrap_->GetDynamicConfigManager();
        dynamicConfigManager->SubscribeConfigChanged(BIND(&TChunkMetaManager::OnDynamicConfigChanged, MakeWeak(this)));
    }

    virtual const NTableClient::TBlockMetaCachePtr& GetBlockMetaCache() override
    {
        return BlockMetaCache_;
    }

    virtual TRefCountedChunkMetaPtr FindCachedMeta(TChunkId chunkId) override
    {
        auto cachedMeta = ChunkMetaCache_->Find(chunkId);
        return cachedMeta ? cachedMeta->GetMeta() : nullptr;
    }

    virtual void PutCachedMeta(
        TChunkId chunkId,
        TRefCountedChunkMetaPtr meta) override
    {
        auto cookie = BeginInsertCachedMeta(chunkId);
        if (cookie.IsActive()) {
            EndInsertCachedMeta(std::move(cookie), std::move(meta));
        } else {
            YT_LOG_DEBUG("Failed to cache chunk meta due to concurrent read (ChunkId: %v)",
                chunkId);
        }
    }

    virtual TCachedChunkMetaCookie BeginInsertCachedMeta(TChunkId chunkId) override
    {
        return ChunkMetaCache_->BeginInsert(chunkId);
    }

    virtual void EndInsertCachedMeta(
        TCachedChunkMetaCookie&& cookie,
        TRefCountedChunkMetaPtr meta) override
    {
        auto chunkId = cookie.GetKey();
        auto cachedMeta = New<TCachedChunkMeta>(
            chunkId,
            std::move(meta));
        cookie.EndInsert(cachedMeta);

        YT_LOG_DEBUG("Chunk meta is put into cache (ChunkId: %v)",
            chunkId);
    }

    virtual void RemoveCachedMeta(TChunkId chunkId) override
    {
         ChunkMetaCache_->TryRemove(chunkId);
    }


    virtual TRefCountedBlocksExtPtr FindCachedBlocksExt(TChunkId chunkId) override
    {
        auto cachedBlocksExt = BlocksExtCache_->Find(chunkId);
        return cachedBlocksExt ? cachedBlocksExt->GetBlocksExt() : nullptr;
    }

    virtual void PutCachedBlocksExt(TChunkId chunkId, TRefCountedBlocksExtPtr blocksExt) override
    {
        auto cookie = BeginInsertCachedBlocksExt(chunkId);
        if (cookie.IsActive()) {
            EndInsertCachedBlocksExt(std::move(cookie), std::move(blocksExt));
        } else {
            YT_LOG_DEBUG("Failed to cache blocks ext due to concurrent read (ChunkId: %v)",
                chunkId);
        }
    }

    virtual TCachedBlocksExtCookie BeginInsertCachedBlocksExt(TChunkId chunkId) override
    {
        return BlocksExtCache_->BeginInsert(chunkId);
    }

    virtual void EndInsertCachedBlocksExt(
        TCachedBlocksExtCookie&& cookie,
        TRefCountedBlocksExtPtr blocksExt) override
    {
        auto chunkId = cookie.GetKey();
        auto cachedBlocksExt = New<TCachedBlocksExt>(
            chunkId,
            std::move(blocksExt));
        cookie.EndInsert(cachedBlocksExt);

        YT_LOG_DEBUG("Blocks ext is put into cache (ChunkId: %v)",
            chunkId);
    }

    virtual void RemoveCachedBlocksExt(TChunkId chunkId) override
    {
         BlocksExtCache_->TryRemove(chunkId);
    }

private:
    TBootstrap* const Bootstrap_;
    const TDataNodeConfigPtr Config_;

    class TChunkMetaCache
        : public TMemoryTrackingAsyncSlruCacheBase<TChunkId, TCachedChunkMeta>
    {
    public:
        TChunkMetaCache(
            TSlruCacheConfigPtr config,
            IMemoryUsageTrackerPtr memoryTracker,
            NProfiling::TRegistry profiler)
            : TMemoryTrackingAsyncSlruCacheBase(
                std::move(config),
                std::move(memoryTracker),
                std::move(profiler))
        { }

    protected:
        virtual i64 GetWeight(const TCachedChunkMetaPtr& meta) const override
        {
            VERIFY_THREAD_AFFINITY_ANY();

            return meta->GetWeight();
        }
    };

    const TIntrusivePtr<TChunkMetaCache> ChunkMetaCache_;

    class TBlocksExtCache
        : public TMemoryTrackingAsyncSlruCacheBase<TChunkId, TCachedBlocksExt>
    {
    public:
        TBlocksExtCache(
            TSlruCacheConfigPtr config,
            IMemoryUsageTrackerPtr memoryTracker,
            NProfiling::TRegistry profiler)
            : TMemoryTrackingAsyncSlruCacheBase(
                std::move(config),
                std::move(memoryTracker),
                std::move(profiler))
        { }

    protected:
        virtual i64 GetWeight(const TCachedBlocksExtPtr& blocksExt) const override
        {
            VERIFY_THREAD_AFFINITY_ANY();

            return blocksExt->GetWeight();
        }
    };

    const TIntrusivePtr<TBlocksExtCache> BlocksExtCache_;

    const NTableClient::TBlockMetaCachePtr BlockMetaCache_;

    void OnDynamicConfigChanged(
        const NClusterNode::TClusterNodeDynamicConfigPtr& /* oldNodeConfig */,
        const NClusterNode::TClusterNodeDynamicConfigPtr& newNodeConfig)
    {
        const auto& config = newNodeConfig->DataNode;
        ChunkMetaCache_->Reconfigure(config->ChunkMetaCache);
        BlocksExtCache_->Reconfigure(config->BlocksExtCache);
        BlockMetaCache_->Reconfigure(config->BlockMetaCache);
    }
};

IChunkMetaManagerPtr CreateChunkMetaManager(NClusterNode::TBootstrap* bootstrap)
{
    return New<TChunkMetaManager>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
