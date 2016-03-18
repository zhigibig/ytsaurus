#include "store_flusher.h"
#include "private.h"
#include "sorted_chunk_store.h"
#include "config.h"
#include "sorted_dynamic_store.h"
#include "in_memory_manager.h"
#include "slot_manager.h"
#include "store_manager.h"
#include "tablet.h"
#include "tablet_manager.h"
#include "tablet_slot.h"
#include "chunk_writer_pool.h"

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/server/hive/hive_manager.h>

#include <yt/server/hydra/mutation.h>

#include <yt/server/misc/memory_usage_tracker.h>

#include <yt/server/tablet_node/tablet_manager.pb.h>

#include <yt/server/tablet_server/tablet_manager.pb.h>

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/chunk_client/chunk_writer.h>
#include <yt/ytlib/chunk_client/replication_writer.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/table_client/unversioned_row.h>
#include <yt/ytlib/table_client/versioned_chunk_writer.h>
#include <yt/ytlib/table_client/versioned_reader.h>
#include <yt/ytlib/table_client/versioned_row.h>
#include <yt/ytlib/table_client/versioned_writer.h>

#include <yt/core/concurrency/thread_pool.h>
#include <yt/core/concurrency/async_semaphore.h>
#include <yt/core/concurrency/scheduler.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/address.h>

#include <yt/core/ytree/attribute_helpers.h>

namespace NYT {
namespace NTabletNode {

using namespace NConcurrency;
using namespace NYTree;
using namespace NApi;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NNodeTrackerClient;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NHydra;
using namespace NTabletServer::NProto;
using namespace NTabletNode::NProto;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletNodeLogger;
static const size_t MaxRowsPerRead = 1024;

////////////////////////////////////////////////////////////////////////////////

class TStoreFlusher
    : public TRefCounted
{
public:
    TStoreFlusher(
        TTabletNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , ThreadPool_(New<TThreadPool>(Config_->StoreFlusher->ThreadPoolSize, "StoreFlush"))
        , Semaphore_(New<TAsyncSemaphore>(Config_->StoreFlusher->MaxConcurrentFlushes))
    {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        slotManager->SubscribeBeginSlotScan(BIND(&TStoreFlusher::OnBeginSlotScan, MakeStrong(this)));
        slotManager->SubscribeScanSlot(BIND(&TStoreFlusher::OnScanSlot, MakeStrong(this)));
        slotManager->SubscribeEndSlotScan(BIND(&TStoreFlusher::OnEndSlotScan, MakeStrong(this)));
    }

private:
    const TTabletNodeConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;

    TThreadPoolPtr ThreadPool_;
    TAsyncSemaphorePtr Semaphore_;

    struct TForcedRotationCandidate
    {
        i64 MemoryUsage;
        TTabletId TabletId;
        TTabletSlotPtr Slot;
    };

    TSpinLock SpinLock_;
    i64 PassiveMemoryUsage_;
    std::vector<TForcedRotationCandidate> ForcedRotationCandidates_;


    void OnBeginSlotScan()
    {
        // NB: Strictly speaking, this locking is redundant.
        TGuard<TSpinLock> guard(SpinLock_);
        PassiveMemoryUsage_ = 0;
        ForcedRotationCandidates_.clear();
    }

    void OnScanSlot(TTabletSlotPtr slot)
    {
        if (slot->GetAutomatonState() != EPeerState::Leading) {
            return;
        }

        auto tabletManager = slot->GetTabletManager();
        for (const auto& pair : tabletManager->Tablets()) {
            auto* tablet = pair.second;
            ScanTablet(slot, tablet);
        }
    }

    void OnEndSlotScan()
    {
        std::vector<TForcedRotationCandidate> candidates;

        // NB: Strictly speaking, this locking is redundant.
        {
            TGuard<TSpinLock> guard(SpinLock_);
            ForcedRotationCandidates_.swap(candidates);
        }

        // Order candidates by increasing memory usage.
        std::sort(
            candidates. begin(),
            candidates.end(),
            [] (const TForcedRotationCandidate& lhs, const TForcedRotationCandidate& rhs) {
                return lhs.MemoryUsage < rhs.MemoryUsage;
            });
        
        // Pick the heaviest candidates until no more rotations are needed.
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        while (slotManager->IsRotationForced(PassiveMemoryUsage_) && !candidates.empty()) {
            auto candidate = candidates.back();
            candidates.pop_back();

            auto tabletId = candidate.TabletId;
            auto tabletSnapshot = slotManager->FindTabletSnapshot(tabletId);
            if (!tabletSnapshot)
                continue;

            const auto* tracker = Bootstrap_->GetMemoryUsageTracker();
            LOG_INFO("Scheduling store rotation due to memory pressure condition (TabletId: %v, "
                "TotalMemoryUsage: %v, TabletMemoryUsage: %v, "
                "MemoryLimit: %v)",
                candidate.TabletId,
                tracker->GetUsed(EMemoryCategory::TabletDynamic),
                candidate.MemoryUsage,
                tracker->GetLimit(EMemoryCategory::TabletDynamic));

            const auto& slot = candidate.Slot;
            auto invoker = slot->GetGuardedAutomatonInvoker();
            invoker->Invoke(BIND([slot, tabletId] () {
                auto tabletManager = slot->GetTabletManager();
                auto* tablet = tabletManager->FindTablet(tabletId);
                if (!tablet)
                    return;
                tabletManager->ScheduleStoreRotation(tablet);
            }));

            PassiveMemoryUsage_ += candidate.MemoryUsage;
        }
    }

    void ScanTablet(TTabletSlotPtr slot, TTablet* tablet)
    {
        auto tabletManager = slot->GetTabletManager();
        const auto& storeManager = tablet->GetStoreManager();

        if (storeManager->IsOverflowRotationNeeded()) {
            LOG_DEBUG("Scheduling store rotation due to overflow (TabletId: %v)",
                tablet->GetId());
            tabletManager->ScheduleStoreRotation(tablet);
        }

        if (storeManager->IsPeriodicRotationNeeded()) {
            LOG_INFO("Scheduling periodic store rotation (TabletId: %v)",
                tablet->GetId());
            tabletManager->ScheduleStoreRotation(tablet);
        }

        for (const auto& pair : tablet->Stores()) {
            const auto& store = pair.second;
            ScanStore(slot, tablet, store);
            if (store->GetStoreState() == EStoreState::PassiveDynamic) {
                TGuard<TSpinLock> guard(SpinLock_);
                PassiveMemoryUsage_ += store->GetMemoryUsage();
            }
        }

        {
            TGuard<TSpinLock> guard(SpinLock_);
            const auto& storeManager = tablet->GetStoreManager();
            if (storeManager->IsForcedRotationPossible()) {
                const auto& store = tablet->GetActiveStore();
                i64 memoryUsage = store->GetMemoryUsage();
                if (storeManager->IsRotationScheduled()) {
                    PassiveMemoryUsage_ += memoryUsage;
                } else if (store->GetUncompressedDataSize() >= Config_->StoreFlusher->MinForcedFlushDataSize) {
                    ForcedRotationCandidates_.push_back({
                        memoryUsage,
                        tablet->GetId(),
                        slot
                    });
                }
            }
        }
    }

    void ScanStore(TTabletSlotPtr slot, TTablet* tablet, const IStorePtr& store)
    {
        if (!store->IsDynamic()) {
            return;
        }

        auto dynamicStore = store->AsDynamic();
        const auto& storeManager = tablet->GetStoreManager();
        if (!storeManager->IsStoreFlushable(dynamicStore)) {
            return;
        }

        auto guard = TAsyncSemaphoreGuard::TryAcquire(Semaphore_);
        if (!guard) {
            return;
        }

        storeManager->BeginStoreFlush(dynamicStore);

        tablet->GetEpochAutomatonInvoker()->Invoke(BIND(
            &TStoreFlusher::FlushStore,
            MakeStrong(this),
            Passed(std::move(guard)),
            slot,
            tablet,
            dynamicStore));
    }


    void FlushStore(
        TAsyncSemaphoreGuard /*guard*/,
        TTabletSlotPtr slot,
        TTablet* tablet,
        IDynamicStorePtr store)
    {
        // Capture everything needed below.
        // NB: Avoid accessing tablet from pool invoker.
        auto hydraManager = slot->GetHydraManager();
        auto tabletManager = slot->GetTabletManager();
        auto storeManager = tablet->GetStoreManager();
        auto tabletId = tablet->GetId();
        auto mountRevision = tablet->GetMountRevision();
        auto keyColumns = tablet->KeyColumns();
        auto schema = tablet->Schema();
        auto mountConfig = tablet->GetConfig();
        auto writerOptions = CloneYsonSerializable(tablet->GetWriterOptions());
        writerOptions->ChunksEden = true;

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        auto tabletSnapshot = slotManager->FindTabletSnapshot(tabletId);
        if (!tabletSnapshot)
            return;

        NLogging::TLogger Logger(TabletNodeLogger);
        Logger.AddTag("TabletId: %v, StoreId: %v",
            tabletId,
            store->GetId());

        auto automatonInvoker = tablet->GetEpochAutomatonInvoker();
        auto poolInvoker = ThreadPool_->GetInvoker();

        try {
            LOG_INFO("Store flush started");

            // XXX(babenko): generalize
            auto reader = store->AsSortedDynamic()->CreateFlushReader();

            // NB: Memory store reader is always synchronous.
            YCHECK(reader->Open().Get().IsOK());

            SwitchTo(poolInvoker);

            ITransactionPtr transaction;
            {
                LOG_INFO("Creating store flush transaction");

                TTransactionStartOptions options;
                options.AutoAbort = false;
                auto attributes = CreateEphemeralAttributes();
                attributes->Set("title", Format("Flushing store %v, tablet %v",
                    store->GetId(),
                    tabletId));
                options.Attributes = std::move(attributes);

                auto asyncTransaction = Bootstrap_->GetMasterClient()->StartTransaction(
                    NTransactionClient::ETransactionType::Master,
                    options);
                transaction = WaitFor(asyncTransaction)
                    .ValueOrThrow();

                LOG_INFO("Store flush transaction created (TransactionId: %v)",
                    transaction->GetId());
            }

            TChunkWriterPool writerPool(
                Bootstrap_->GetInMemoryManager(),
                tabletSnapshot,
                1,
                Config_->ChunkWriter,
                writerOptions,
                mountConfig,
                schema,
                Bootstrap_->GetMasterClient(),
                transaction->GetId());
            auto writer = writerPool.AllocateWriter();

            WaitFor(writer->Open())
                .ThrowOnError();
        
            std::vector<TVersionedRow> rows;
            rows.reserve(MaxRowsPerRead);

            while (true) {
                // NB: Memory store reader is always synchronous.
                reader->Read(&rows);
                if (rows.empty())
                    break;
                if (!writer->Write(rows)) {
                    WaitFor(writer->GetReadyEvent())
                        .ThrowOnError();
                }
            }

            WaitFor(writer->Close())
                .ThrowOnError();

            SwitchTo(automatonInvoker);

            storeManager->EndStoreFlush(store);

            TReqCommitTabletStoresUpdate hydraRequest;
            ToProto(hydraRequest.mutable_tablet_id(), tabletId);
            hydraRequest.set_mount_revision(mountRevision);
            ToProto(hydraRequest.mutable_transaction_id(), transaction->GetId());
            {
                auto* descriptor = hydraRequest.add_stores_to_remove();
                ToProto(descriptor->mutable_store_id(), store->GetId());
            }

            std::vector<TChunkId> chunkIds;
            for (const auto& chunkSpec : writer->GetWrittenChunksMasterMeta()) {
                chunkIds.push_back(FromProto<TChunkId>(chunkSpec.chunk_id()));
                auto* descriptor = hydraRequest.add_stores_to_add();
                // XXX(babenko): generalize
                descriptor->set_store_type(static_cast<int>(EStoreType::SortedChunk));
                descriptor->mutable_store_id()->CopyFrom(chunkSpec.chunk_id());
                descriptor->mutable_chunk_meta()->CopyFrom(chunkSpec.chunk_meta());
                ToProto(descriptor->mutable_backing_store_id(), store->GetId());
            }

            CreateMutation(slot->GetHydraManager(), hydraRequest)
                ->CommitAndLog(Logger);

            LOG_INFO("Store flush completed (ChunkIds: %v)",
                chunkIds);

            // Just abandon the transaction, hopefully it won't expire before the chunk is attached.
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error flushing tablet store, backing off");
        
            SwitchTo(automatonInvoker);

            storeManager->BackoffStoreFlush(store);
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

void StartStoreFlusher(
    TTabletNodeConfigPtr config,
    NCellNode::TBootstrap* bootstrap)
{
    if (config->EnableStoreFlusher) {
        New<TStoreFlusher>(config, bootstrap);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
