#include "in_memory_manager.h"
#include "private.h"
#include "sorted_chunk_store.h"
#include "config.h"
#include "slot_manager.h"
#include "store_manager.h"
#include "tablet.h"
#include "tablet_manager.h"
#include "tablet_slot.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/ytlib/chunk_client/chunk_meta.pb.h>
#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/dispatcher.h>

#include <yt/ytlib/misc/memory_usage_tracker.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/table_client/chunk_lookup_hash_table.h>

#include <yt/core/compression/codec.h>

#include <yt/core/concurrency/async_semaphore.h>
#include <yt/core/concurrency/delayed_executor.h>
#include <yt/core/concurrency/rw_spinlock.h>
#include <yt/core/concurrency/scheduler.h>
#include <yt/core/concurrency/thread_affinity.h>
#include <yt/core/misc/finally.h>

namespace NYT {
namespace NTabletNode {

using namespace NChunkClient::NProto;
using namespace NChunkClient;
using namespace NConcurrency;
using namespace NHydra;
using namespace NNodeTrackerClient;
using namespace NTableClient;
using namespace NTabletClient;

////////////////////////////////////////////////////////////////////////////////

const auto& Logger = TabletNodeLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

void FinalizeChunkData(
    const TInMemoryChunkDataPtr& data,
    const TChunkId& id,
    const TChunkMeta& meta,
    const TTabletSnapshotPtr& tabletSnapshot,
    TNodeMemoryTracker* memoryTracker)
{
    data->ChunkMeta = TCachedVersionedChunkMeta::Create(id, meta, tabletSnapshot->PhysicalSchema, memoryTracker);
    if (tabletSnapshot->HashTableSize > 0) {
        data->LookupHashTable = CreateChunkLookupHashTable(
            data->Blocks,
            data->ChunkMeta,
            tabletSnapshot->RowKeyComparer);
        if (data->LookupHashTable && data->MemoryTrackerGuard) {
            data->MemoryTrackerGuard.UpdateSize(data->LookupHashTable->GetByteSize());
        }
    }
}

} // namespace

EBlockType MapInMemoryModeToBlockType(EInMemoryMode mode)
{
    switch (mode) {
        case EInMemoryMode::Compressed:
            return EBlockType::CompressedData;

        case EInMemoryMode::Uncompressed:
            return EBlockType::UncompressedData;

        case EInMemoryMode::None:
            return EBlockType::None;

        default:
            Y_UNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

class TInMemoryManager::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TInMemoryManagerConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , CompressionInvoker_(CreateFixedPriorityInvoker(
            NChunkClient::TDispatcher::Get()->GetCompressionPoolInvoker(),
            Config_->WorkloadDescriptor.GetPriority()))
        , PreloadSemaphore_(New<TAsyncSemaphore>(Config_->MaxConcurrentPreloads))
    {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->SubscribeScanSlot(BIND(&TImpl::ScanSlot, MakeStrong(this)));
    }

    IBlockCachePtr CreateInterceptingBlockCache(EInMemoryMode mode, ui64 configRevision)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return New<TInterceptingBlockCache>(this, mode, configRevision);
    }

    TInMemoryChunkDataPtr EvictInterceptedChunkData(const TChunkId& chunkId)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TWriterGuard guard(InterceptedDataSpinLock_);

        auto it = ChunkIdToData_.find(chunkId);
        if (it == ChunkIdToData_.end()) {
            return nullptr;
        }

        auto chunkData = std::move(it->second);
        ChunkIdToData_.erase(it);

        LOG_INFO("Intercepted chunk data evicted (ChunkId: %v, Mode: %v, ConfigRevision: %v)",
            chunkId,
            chunkData->InMemoryMode,
            chunkData->InMemoryConfigRevision);

        return chunkData;
    }

    void FinalizeChunk(
        const TChunkId& chunkId,
        const TChunkMeta& chunkMeta,
        const TTabletSnapshotPtr& tabletSnapshot)
    {
        TInMemoryChunkDataPtr data;

        {
            TWriterGuard guard(InterceptedDataSpinLock_);
            auto it = ChunkIdToData_.find(chunkId);
            if (it != ChunkIdToData_.end()) {
                data = it->second;
            }
        }

        if (!data) {
            LOG_INFO("Cannot find intercepted chunk data for finalization (ChunkId: %v)", chunkId);
            return;
        }

        FinalizeChunkData(data, chunkId, chunkMeta, tabletSnapshot, Bootstrap_->GetMemoryUsageTracker());
    }

private:
    const TInMemoryManagerConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;

    const IInvokerPtr CompressionInvoker_;

    TAsyncSemaphorePtr PreloadSemaphore_;

    TReaderWriterSpinLock InterceptedDataSpinLock_;
    yhash<TChunkId, TInMemoryChunkDataPtr> ChunkIdToData_;


    void ScanSlot(const TTabletSlotPtr& slot)
    {
        const auto& tabletManager = slot->GetTabletManager();
        for (const auto& pair : tabletManager->Tablets()) {
            auto* tablet = pair.second;
            ScanTablet(slot, tablet);
        }
    }

    void ScanTablet(const TTabletSlotPtr& slot, TTablet* tablet)
    {
        auto state = tablet->GetState();
        if (IsInUnmountWorkflow(state)) {
            return;
        }

        const auto& storeManager = tablet->GetStoreManager();
        auto mode = storeManager->GetInMemoryMode();
        auto configRevision = storeManager->GetInMemoryConfigRevision();

        while (true) {
            auto store = storeManager->PeekStoreForPreload();
            if (!store) {
                break;
            }
            auto guard = TAsyncSemaphoreGuard::TryAcquire(PreloadSemaphore_);
            if (!guard) {
                break;
            }
            auto preloadStoreCallback =
                BIND(
                    &TImpl::PreloadStore,
                    MakeStrong(this),
                    Passed(std::move(guard)),
                    slot,
                    tablet,
                    mode,
                    configRevision,
                    store,
                    storeManager)
                .AsyncVia(tablet->GetEpochAutomatonInvoker());
            storeManager->BeginStorePreload(store, preloadStoreCallback);
        }
    }

    void PreloadStore(
        TAsyncSemaphoreGuard /*guard*/,
        const TTabletSlotPtr& slot,
        TTablet* tablet,
        EInMemoryMode mode,
        ui64 configRevision,
        const IChunkStorePtr& store,
        const IStoreManagerPtr& storeManager)
    {
        NLogging::TLogger Logger(TabletNodeLogger);
        Logger.AddTag("TabletId: %v, StoreId: %v, Mode: %v, ConfigRevision: %v",
            tablet->GetId(),
            store->GetId(),
            mode,
            configRevision);

        try {
            // Fail quickly.
            if (storeManager->GetInMemoryConfigRevision() != configRevision) {
                THROW_ERROR_EXCEPTION("In-memory config revision has changed")
                    << TErrorAttribute("expected", configRevision)
                    << TErrorAttribute("actual", storeManager->GetInMemoryConfigRevision());
            }

            // We can create finalizer after the previous check, because if the check did not succeed,
            // the condition within the finalizer would not succeed either.
            auto finalizer = Finally(
                [&, invoker = tablet->GetEpochAutomatonInvoker()] () {
                    // Finalizer may be invoked from a finalizer thread,
                    // thus we reschedule backoff to a proper thread to avoid unsynchronized access.
                    LOG_WARNING("Backing off tablet store preload");
                    invoker->Invoke(BIND([configRevision, storeManager, store] {
                        if (storeManager->GetInMemoryConfigRevision() == configRevision) {
                            storeManager->BackoffStorePreload(store);
                        }
                    }));
                });

            auto tabletSnapshot = Bootstrap_->GetTabletSlotManager()->FindTabletSnapshot(tablet->GetId());

            if (!tabletSnapshot) {
                THROW_ERROR_EXCEPTION("Tablet snapshot is missing");
            }

            if (tabletSnapshot->Config->InMemoryMode != mode) {
                THROW_ERROR_EXCEPTION("In-memory mode does not match the snapshot")
                    << TErrorAttribute("expected", mode)
                    << TErrorAttribute("actual", tabletSnapshot->Config->InMemoryMode);
            }

            if (tabletSnapshot->InMemoryConfigRevision != configRevision) {
                THROW_ERROR_EXCEPTION("In-memory config revision does not match the snapshot")
                    << TErrorAttribute("expected", mode)
                    << TErrorAttribute("actual", tabletSnapshot->Config->InMemoryMode);
            }

            // This call may suspend the current fiber.
            auto chunkData = PreloadInMemoryStore(
                tabletSnapshot,
                store,
                Bootstrap_->GetMemoryUsageTracker(),
                CompressionInvoker_);
            // Now, check is the revision in still the same.

            if (storeManager->GetInMemoryConfigRevision() != configRevision) {
                THROW_ERROR_EXCEPTION("In-memory config revision has changed")
                    << TErrorAttribute("expected", configRevision)
                    << TErrorAttribute("actual", storeManager->GetInMemoryConfigRevision());
            }

            finalizer.Release();
            store->Preload(std::move(chunkData));
            storeManager->EndStorePreload(store);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error preloading tablet store, backed off");
        }

        const auto& slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->RegisterTabletSnapshot(slot, tablet);
    }

    class TInterceptingBlockCache
        : public IBlockCache
    {
    public:
        TInterceptingBlockCache(TImplPtr owner, EInMemoryMode mode, ui64 configRevision)
            : Owner_(owner)
            , Mode_(mode)
            , ConfigRevision_(configRevision)
            , BlockType_(MapInMemoryModeToBlockType(Mode_))
        { }

        ~TInterceptingBlockCache()
        {
            for (const auto& chunkId : ChunkIds_) {
                TDelayedExecutor::Submit(
                    BIND(IgnoreResult(&TImpl::EvictInterceptedChunkData), Owner_, chunkId),
                    Owner_->Config_->InterceptedDataRetentionTime);
            }
        }

        virtual void Put(
            const TBlockId& id,
            EBlockType type,
            const TBlock& block,
            const TNullable<NNodeTrackerClient::TNodeDescriptor>& /*source*/) override
        {
            if (type != BlockType_) {
                return;
            }

            TGuard<TSpinLock> guard(SpinLock_);

            if (Owner_->IsMemoryLimitExceeded()) {
                Dropped_ = true;
            }

            if (Dropped_) {
                Owner_->DropChunkData(id.ChunkId);
                return;
            }

            auto it = ChunkIds_.find(id.ChunkId);
            TInMemoryChunkDataPtr data;
            if (it == ChunkIds_.end()) {
                data = Owner_->CreateChunkData(id.ChunkId, Mode_, ConfigRevision_);
                YCHECK(ChunkIds_.insert(id.ChunkId).second);
            } else {
                data = Owner_->GetChunkData(id.ChunkId, Mode_, ConfigRevision_);
            }

            if (data->Blocks.size() <= id.BlockIndex) {
                size_t blockCapacity = std::max(data->Blocks.capacity(), static_cast<size_t>(1));
                while (blockCapacity <= id.BlockIndex) {
                    blockCapacity *= 2;
                }
                data->Blocks.reserve(blockCapacity);
                data->Blocks.resize(id.BlockIndex + 1);
            }

            YCHECK(!data->Blocks[id.BlockIndex].Data);
            data->Blocks[id.BlockIndex] = block;
            if (data->MemoryTrackerGuard) {
                data->MemoryTrackerGuard.UpdateSize(block.Size());
            }
            YCHECK(!data->ChunkMeta);
        }

        virtual TBlock Find(const TBlockId& /*id*/, EBlockType /*type*/) override
        {
            return TBlock();
        }

        virtual EBlockType GetSupportedBlockTypes() const override
        {
            return BlockType_;
        }

    private:
        const TImplPtr Owner_;
        const EInMemoryMode Mode_;
        const ui64 ConfigRevision_;
        const EBlockType BlockType_;

        TSpinLock SpinLock_;
        THashSet<TChunkId> ChunkIds_;
        bool Dropped_ = false;
    };

    TInMemoryChunkDataPtr GetChunkData(const TChunkId& chunkId, EInMemoryMode mode, ui64 configRevision)
    {
        TReaderGuard guard(InterceptedDataSpinLock_);

        auto it = ChunkIdToData_.find(chunkId);
        YCHECK(it != ChunkIdToData_.end());

        auto chunkData = it->second;
        YCHECK(chunkData->InMemoryMode == mode);
        YCHECK(chunkData->InMemoryConfigRevision == configRevision);

        return chunkData;
    }

    TInMemoryChunkDataPtr CreateChunkData(const TChunkId& chunkId, EInMemoryMode mode, ui64 configRevision)
    {
        TWriterGuard guard(InterceptedDataSpinLock_);

        auto chunkData = New<TInMemoryChunkData>();
        chunkData->InMemoryMode = mode;
        chunkData->InMemoryConfigRevision = configRevision;
        chunkData->MemoryTrackerGuard = NCellNode::TNodeMemoryTrackerGuard::Acquire(
            Bootstrap_->GetMemoryUsageTracker(),
            EMemoryCategory::TabletStatic,
            0,
            MemoryUsageGranularity);

        // Replace the old data, if any, by a new one.
        ChunkIdToData_[chunkId] = chunkData;

        LOG_INFO("Intercepted chunk data created (ChunkId: %v, Mode: %v, ConfigRevision: %v)",
            chunkId,
            mode,
            configRevision);

        return chunkData;
    }

    void DropChunkData(const TChunkId& chunkId)
    {
        TWriterGuard guard(InterceptedDataSpinLock_);

        if (ChunkIdToData_.erase(chunkId) == 1) {
            LOG_WARNING("Intercepted chunk data dropped due to memory pressure (ChunkId: %v)",
                chunkId);
        }
    }

    bool IsMemoryLimitExceeded() const
    {
        const auto* tracker = Bootstrap_->GetMemoryUsageTracker();
        return tracker->IsExceeded(EMemoryCategory::TabletStatic);
    }
};

////////////////////////////////////////////////////////////////////////////////

TInMemoryManager::TInMemoryManager(
    TInMemoryManagerConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

TInMemoryManager::~TInMemoryManager() = default;

IBlockCachePtr TInMemoryManager::CreateInterceptingBlockCache(EInMemoryMode mode, ui64 configRevision)
{
    return Impl_->CreateInterceptingBlockCache(mode, configRevision);
}

TInMemoryChunkDataPtr TInMemoryManager::EvictInterceptedChunkData(const TChunkId& chunkId)
{
    return Impl_->EvictInterceptedChunkData(chunkId);
}

void TInMemoryManager::FinalizeChunk(
    const TChunkId& chunkId,
    const TChunkMeta& chunkMeta,
    const TTabletSnapshotPtr& tabletSnapshot)
{
    Impl_->FinalizeChunk(chunkId, chunkMeta, tabletSnapshot);
}

////////////////////////////////////////////////////////////////////////////////

TInMemoryChunkDataPtr PreloadInMemoryStore(
    const TTabletSnapshotPtr& tabletSnapshot,
    const IChunkStorePtr& store,
    TMemoryUsageTracker<EMemoryCategory>* memoryUsageTracker,
    const IInvokerPtr& compressionInvoker)
{
    auto mode = tabletSnapshot->Config->InMemoryMode;
    auto configRevision = tabletSnapshot->InMemoryConfigRevision;

    NLogging::TLogger Logger(TabletNodeLogger);
    Logger.AddTag(
        "TabletId: %v, StoreId: %v, Mode: %v, ConfigRevision: %v",
        tabletSnapshot->TabletId, store->GetId(), mode, configRevision);

    LOG_INFO("Store preload started");

    auto reader = store->GetChunkReader();
    auto workloadDescriptor = TWorkloadDescriptor(EWorkloadCategory::SystemTabletPreload);

    auto meta = WaitFor(reader->GetMeta(TWorkloadDescriptor(workloadDescriptor)))
        .ValueOrThrow();

    auto miscExt = GetProtoExtension<TMiscExt>(meta.extensions());
    auto blocksExt = GetProtoExtension<TBlocksExt>(meta.extensions());

    auto erasureCodec = NErasure::ECodec(miscExt.erasure_codec());
    if (erasureCodec != NErasure::ECodec::None) {
        THROW_ERROR_EXCEPTION("Could not preload erasure coded store %v", store->GetId());
    }

    auto codecId = NCompression::ECodec(miscExt.compression_codec());
    auto* codec = NCompression::GetCodec(codecId);

    int startBlockIndex = 0;
    int totalBlockCount = blocksExt.blocks_size();

    i64 preallocatedMemory = 0;
    i64 allocatedMemory = 0;
    for (int i = 0; i < totalBlockCount; ++i) {
        preallocatedMemory += blocksExt.blocks(i).size();
    }

    if (memoryUsageTracker && memoryUsageTracker->GetFree(EMemoryCategory::TabletStatic) < preallocatedMemory) {
        THROW_ERROR_EXCEPTION("Preload is cancelled due to memory pressure");
    }

    auto chunkData = New<TInMemoryChunkData>();
    chunkData->InMemoryMode = mode;
    chunkData->InMemoryConfigRevision = configRevision;
    if (memoryUsageTracker) {
        chunkData->MemoryTrackerGuard = NCellNode::TNodeMemoryTrackerGuard::Acquire(
            memoryUsageTracker,
            EMemoryCategory::TabletStatic,
            preallocatedMemory,
            MemoryUsageGranularity);
    }
    chunkData->Blocks.reserve(totalBlockCount);

    while (startBlockIndex < totalBlockCount) {
        LOG_DEBUG("Started reading chunk blocks (FirstBlock: %v)",
            startBlockIndex);

        auto compressedBlocks = WaitFor(reader->ReadBlocks(
            workloadDescriptor,
            startBlockIndex,
            totalBlockCount - startBlockIndex))
            .ValueOrThrow();

        int readBlockCount = compressedBlocks.size();
        LOG_DEBUG("Finished reading chunk blocks (Blocks: %v-%v)",
            startBlockIndex,
            startBlockIndex + readBlockCount - 1);

        std::vector<TBlock> cachedBlocks;
        switch (mode) {
            case EInMemoryMode::Compressed: {
                cachedBlocks = std::move(compressedBlocks);
                break;
            }

            case EInMemoryMode::Uncompressed: {
                LOG_DEBUG("Started decompressing chunk blocks (Blocks: %v-%v)",
                    startBlockIndex,
                    startBlockIndex + readBlockCount - 1);

                std::vector<TFuture<TSharedRef>> asyncUncompressedBlocks;
                for (auto& compressedBlock : compressedBlocks) {
                    asyncUncompressedBlocks.push_back(
                        BIND(
                            static_cast<TSharedRef(NCompression::ICodec::*)(const TSharedRef&)>(&NCompression::ICodec::Decompress),
                            Unretained(codec),
                            std::move(compressedBlock.Data))
                            .AsyncVia(compressionInvoker)
                            .Run());
                }

                cachedBlocks = TBlock::Wrap(WaitFor(Combine(asyncUncompressedBlocks))
                    .ValueOrThrow());

                break;
            }

            default:
                Y_UNREACHABLE();
        }

        for (const auto& cachedBlock : cachedBlocks) {
            allocatedMemory += cachedBlock.Size();
        }

        chunkData->Blocks.insert(
            chunkData->Blocks.end(),
            std::make_move_iterator(cachedBlocks.begin()),
            std::make_move_iterator(cachedBlocks.end()));

        startBlockIndex += readBlockCount;
    }

    if (chunkData->MemoryTrackerGuard) {
        chunkData->MemoryTrackerGuard.UpdateSize(allocatedMemory - preallocatedMemory);
    }

    FinalizeChunkData(chunkData, store->GetId(), meta, tabletSnapshot, memoryUsageTracker);

    LOG_INFO(
        "Store preload completed (MemoryUsage: %v, LookupHashTable: %v)",
        allocatedMemory,
        static_cast<bool>(chunkData->LookupHashTable));

    return chunkData;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
