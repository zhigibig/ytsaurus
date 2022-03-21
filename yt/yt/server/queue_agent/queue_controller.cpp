#include "queue_controller.h"

#include "consumer_table.h"
#include "snapshot.h"
#include "snapshot_representation.h"
#include "config.h"
#include "helpers.h"

#include <yt/yt/ytlib/hive/cluster_directory.h>

#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/client/tablet_client/table_mount_cache.h>

#include <yt/yt/client/transaction_client/helpers.h>

#include <yt/yt/client/api/rowset.h>

#include <yt/yt/core/concurrency/periodic_executor.h>

#include <library/cpp/iterator/functools.h>

namespace NYT::NQueueAgent {

using namespace NHydra;
using namespace NYTree;
using namespace NConcurrency;
using namespace NHiveClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTransactionClient;
using namespace NYson;
using namespace NTracing;

using namespace std::placeholders;

////////////////////////////////////////////////////////////////////////////////

using TConsumerSnapshotMap = THashMap<TCrossClusterReference, TConsumerSnapshotPtr>;

////////////////////////////////////////////////////////////////////////////////

class TOrderedDynamicTableController
    : public IQueueController
{
public:
    TOrderedDynamicTableController(
        TQueueControllerConfigPtr config,
        TClientDirectoryPtr clientDirectory,
        TCrossClusterReference queueRef,
        TQueueTableRow queueRow,
        TConsumerRowMap consumerRowMap,
        IInvokerPtr invoker)
        : Config_(std::move(config))
        , ClientDirectory_(std::move(clientDirectory))
        , QueueRef_(std::move(queueRef))
        , Invoker_(std::move(invoker))
        , Logger(QueueAgentLogger.WithTag("Queue: %Qv", QueueRef_))
        , PassExecutor_(New<TPeriodicExecutor>(
            Invoker_,
            BIND(&TOrderedDynamicTableController::Pass, MakeWeak(this)),
            Config_->PassPeriod))
    {
        QueueSnapshot_ = New<TQueueSnapshot>();
        QueueSnapshot_->Row = std::move(queueRow);
        QueueSnapshot_->Error = TError("Queue is not processed yet");

        for (auto& [ref, row] : consumerRowMap) {
            auto snapshot = New<TConsumerSnapshot>();
            snapshot->Row = std::move(row);
            snapshot->Error = TError("Consumer is not processed yet");
            ConsumerSnapshots_.emplace(std::move(ref), std::move(snapshot));
        }

        QueueSnapshot_->ConsumerSnapshots = ConsumerSnapshots_;
    }

    EQueueFamily GetQueueFamily() const override
    {
        return EQueueFamily::OrderedDynamicTable;
    }

    void BuildOrchid(TFluentMap fluent) const override
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        YT_LOG_DEBUG("Building queue controller orchid (PassIndex: %v)", PassIndex_ - 1);

        fluent
            .Item("pass_index").Value(PassIndex_)
            .Item("pass_instant").Value(PassInstant_)
            .Item("row").Value(QueueSnapshot_->Row)
            .Item("status").Do(std::bind(BuildQueueStatusYson, QueueSnapshot_, _1))
            .Item("partitions").Do(std::bind(BuildQueuePartitionListYson, QueueSnapshot_, _1));
    }

    void BuildConsumerOrchid(const TCrossClusterReference& consumerRef, TFluentMap fluent) const override
    {
        YT_LOG_DEBUG("Building consumer controller orchid (Consumer: %Qv, PassIndex: %v)", consumerRef, PassIndex_ - 1);

        const auto& consumerSnapshot = GetOrCrash(ConsumerSnapshots_, consumerRef);

        fluent
            .Item("pass_index").Value(PassIndex_)
            .Item("pass_instant").Value(PassInstant_)
            .Item("row").Value(consumerSnapshot->Row)
            .Item("status").Do(std::bind(BuildConsumerStatusYson, consumerSnapshot, _1))
            .Item("partitions").Do(std::bind(BuildConsumerPartitionListYson, consumerSnapshot, _1));
    }

    void Start() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        YT_LOG_INFO("Queue controller started");

        PassExecutor_->Start();
    }

    TFuture<void> Stop() override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        YT_LOG_INFO("Queue controller stopped");

        return PassExecutor_->Stop();
    }

    IInvokerPtr GetInvoker() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return Invoker_;
    }

private:
    const TQueueControllerConfigPtr Config_;
    const TClientDirectoryPtr ClientDirectory_;
    const TCrossClusterReference QueueRef_;

    TQueueSnapshotPtr QueueSnapshot_;
    TConsumerSnapshotMap ConsumerSnapshots_;

    const IInvokerPtr Invoker_;

    const NLogging::TLogger Logger;

    const TPeriodicExecutorPtr PassExecutor_;

    i64 PassIndex_ = 0;
    TInstant PassInstant_ = TInstant::Zero();

    void Pass()
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        auto traceContextGuard = TTraceContextGuard(TTraceContext::NewRoot("QueueController"));

        const auto& Logger = QueueAgentLogger;

        auto nextPassInstant = TInstant::Now();
        auto nextPassIndex = PassIndex_ + 1;

        YT_LOG_INFO("Controller pass started (NextPassIndex: %v)", nextPassIndex);
        auto logFinally = Finally([&] {
            YT_LOG_INFO("Controller pass finished (PassIndex: %v -> %v)", PassIndex_, nextPassIndex);
            PassIndex_ = nextPassIndex;
            PassInstant_ = nextPassInstant;
        });

        // First, update queue snapshot.
        {
            auto nextQueueSnapshot = New<TQueueSnapshot>();
            nextQueueSnapshot->Row = std::move(QueueSnapshot_->Row);
            GuardedUpdateQueueSnapshot(nextQueueSnapshot);
            QueueSnapshot_ = std::move(nextQueueSnapshot);
        }

        // Second, update consumer snapshots.

        {
            std::vector<TFuture<void>> consumerSnapshotUpdateFutures;
            TConsumerSnapshotMap nextConsumerSnapshots;

            auto consumerCount = ConsumerSnapshots_.size();
            consumerSnapshotUpdateFutures.reserve(consumerCount);
            nextConsumerSnapshots.reserve(consumerCount);

            for (const auto& [consumerRef, consumerSnapshot] : ConsumerSnapshots_) {
                auto& nextConsumerSnapshot = nextConsumerSnapshots[consumerRef];
                nextConsumerSnapshot = New<TConsumerSnapshot>();
                nextConsumerSnapshot->Row = std::move(consumerSnapshot->Row);
                consumerSnapshotUpdateFutures.emplace_back(
                    BIND(&TOrderedDynamicTableController::GuardedUpdateConsumerSnapshot, MakeWeak(this), consumerRef, nextConsumerSnapshot)
                        .AsyncVia(Invoker_)
                        .Run());
            }

            // None of snapshot update methods may throw.
            YT_VERIFY(WaitFor(AllSucceeded(consumerSnapshotUpdateFutures)).IsOK());

            ConsumerSnapshots_.swap(nextConsumerSnapshots);
        }

        // Connect queue snapshot to consumer snapshots with pointers.

        QueueSnapshot_->ConsumerSnapshots = ConsumerSnapshots_;
    }

    void GuardedUpdateQueueSnapshot(const TQueueSnapshotPtr& snapshot)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        try {
            UpdateQueueSnapshot(snapshot);
        } catch (const std::exception& ex) {
            auto error = TError(ex);
            YT_LOG_DEBUG(error, "Error updating queue snapshot");
            snapshot->Error = std::move(error);
        }
    }

    void UpdateQueueSnapshot(const TQueueSnapshotPtr& snapshot)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        YT_LOG_DEBUG("Updating queue snapshot");

        snapshot->Family = GetQueueFamily();
        auto client = ClientDirectory_->GetClientOrThrow(QueueRef_.Cluster);
        const auto& tableMountCache = client->GetTableMountCache();

        // Fetch partition count (which is equal to tablet count).

        auto tableInfo = WaitFor(tableMountCache->GetTableInfo(QueueRef_.Path))
            .ValueOrThrow();

        YT_LOG_DEBUG("Table info collected (TabletCount: %v)", tableInfo->Tablets.size());

        auto& partitionCount = snapshot->PartitionCount;
        partitionCount = tableInfo->Tablets.size();

        auto& partitionSnapshots = snapshot->PartitionSnapshots;
        partitionSnapshots.resize(partitionCount);
        for (auto& partitionSnapshot : partitionSnapshots) {
            partitionSnapshot = New<TQueuePartitionSnapshot>();
        }

        // Fetch tablet infos.

        std::vector<int> tabletIndexes;
        tabletIndexes.reserve(partitionCount);
        for (int index = 0; index < partitionCount; ++index) {
            const auto& tabletInfo = tableInfo->Tablets[index];
            if (tabletInfo->State != ETabletState::Mounted) {
                partitionSnapshots[index]->Error = TError("Tablet %v is not mounted", tabletInfo->TabletId)
                    << TErrorAttribute("state", tabletInfo->State);
            } else {
                tabletIndexes.push_back(index);
            }
        }

        auto tabletInfos = WaitFor(client->GetTabletInfos(QueueRef_.Path, tabletIndexes))
            .ValueOrThrow();

        YT_VERIFY(std::ssize(tabletInfos) == std::ssize(tabletIndexes));

        // Fill partition snapshots from tablet infos.

        for (int index = 0; index < std::ssize(tabletInfos); ++index) {
            const auto& partitionSnapshot = partitionSnapshots[tabletIndexes[index]];
            const auto& tabletInfo = tabletInfos[index];
            partitionSnapshot->UpperRowIndex = tabletInfo.TotalRowCount;
            partitionSnapshot->LowerRowIndex = tabletInfo.TrimmedRowCount;
            partitionSnapshot->AvailableRowCount = partitionSnapshot->UpperRowIndex - partitionSnapshot->LowerRowIndex;
            partitionSnapshot->LastRowCommitTime = TimestampToInstant(tabletInfo.LastWriteTimestamp).first;
            partitionSnapshot->CommitIdleTime = TInstant::Now() - partitionSnapshot->LastRowCommitTime;
        }

        YT_LOG_DEBUG("Queue snapshot updated");
    }

    void GuardedUpdateConsumerSnapshot(const TCrossClusterReference& consumerRef, const TConsumerSnapshotPtr& consumerSnapshot)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        consumerSnapshot->TargetQueue = QueueRef_;
        consumerSnapshot->Vital = consumerSnapshot->Row.Vital.value_or(false);
        consumerSnapshot->Owner = *consumerSnapshot->Row.Owner;

        try {
            UpdateConsumerSnapshot(consumerRef, consumerSnapshot);
        } catch (const std::exception& ex) {
            auto error = TError(ex);
            YT_LOG_DEBUG(error, "Error updating consumer snapshot (Consumer: %Qv)", consumerRef);
            consumerSnapshot->Error = std::move(error);
        }
    }

    void UpdateConsumerSnapshot(const TCrossClusterReference& consumerRef, const TConsumerSnapshotPtr& consumerSnapshot)
    {
        VERIFY_INVOKER_AFFINITY(Invoker_);

        YT_LOG_DEBUG("Updating consumer snapshot (Consumer: %Qv)", consumerRef);

        if (!consumerSnapshot->Row.Schema) {
            THROW_ERROR_EXCEPTION("Consumer schema is not known yet");
        }

        // Assume partition count to be the same as the partition count in the current queue snapshot.
        auto partitionCount = QueueSnapshot_->PartitionCount;
        consumerSnapshot->PartitionCount = partitionCount;

        // Allocate partition snapshots.
        consumerSnapshot->PartitionSnapshots.resize(consumerSnapshot->PartitionCount);
        for (auto& consumerPartitionSnapshot : consumerSnapshot->PartitionSnapshots) {
            consumerPartitionSnapshot = New<TConsumerPartitionSnapshot>();
            consumerPartitionSnapshot->NextRowIndex = 0;
        }

        // Collect partition infos from the consumer table.
        {
            auto client = ClientDirectory_->GetClientOrThrow(consumerRef.Cluster);
            auto consumerTable = CreateConsumerTable(client, consumerRef.Path, *consumerSnapshot->Row.Schema);

            auto consumerPartitionInfos = WaitFor(consumerTable->CollectPartitions(partitionCount, /*withLastConsumeTime*/ true))
                .ValueOrThrow();

            for (const auto& consumerPartitionInfo : consumerPartitionInfos) {
                auto partitionIndex = consumerPartitionInfo.PartitionIndex;

                auto& consumerPartitionSnapshot = consumerSnapshot->PartitionSnapshots[partitionIndex];

                consumerPartitionSnapshot->NextRowIndex = consumerPartitionInfo.NextRowIndex;
                consumerPartitionSnapshot->LastConsumeTime = consumerPartitionInfo.LastConsumeTime;
            }
        }

        // Construct the QL expression for extracting commit timestamps by tablet indices and row indices.
        TStringBuilder nextRowCommitTimeQuery;
        nextRowCommitTimeQuery.AppendFormat(
            "[$tablet_index], [$timestamp] from [%v] where ([$tablet_index], [$row_index]) in (",
            QueueSnapshot_->Row.Queue.Path);
        bool isFirstTuple = true;

        for (const auto& [partitionIndex, consumerPartitionSnapshot] : Enumerate(consumerSnapshot->PartitionSnapshots)) {
            consumerPartitionSnapshot->ConsumeIdleTime = TInstant::Now() - consumerPartitionSnapshot->LastConsumeTime;

            const auto& queuePartitionSnapshot = QueueSnapshot_->PartitionSnapshots[partitionIndex];
            if (queuePartitionSnapshot->Error.IsOK()) {
                // NB: may be negative if the consumer is ahead of the partition.
                consumerPartitionSnapshot->UnreadRowCount = queuePartitionSnapshot->UpperRowIndex - consumerPartitionSnapshot->NextRowIndex;

                if (consumerPartitionSnapshot->UnreadRowCount < 0) {
                    consumerPartitionSnapshot->Disposition = EConsumerPartitionDisposition::Ahead;
                } else if (consumerPartitionSnapshot->UnreadRowCount == 0) {
                    consumerPartitionSnapshot->Disposition = EConsumerPartitionDisposition::UpToDate;
                } else if (consumerPartitionSnapshot->UnreadRowCount <= queuePartitionSnapshot->AvailableRowCount) {
                    consumerPartitionSnapshot->Disposition = EConsumerPartitionDisposition::PendingConsumption;
                } else if (consumerPartitionSnapshot->UnreadRowCount > queuePartitionSnapshot->AvailableRowCount) {
                    consumerPartitionSnapshot->Disposition = EConsumerPartitionDisposition::Expired;
                } else {
                    Y_UNREACHABLE();
                }

                if (consumerPartitionSnapshot->Disposition == EConsumerPartitionDisposition::PendingConsumption) {
                    if (!isFirstTuple) {
                        nextRowCommitTimeQuery.AppendString(", ");
                    }
                    nextRowCommitTimeQuery.AppendFormat("(%vu, %vu)", partitionIndex, consumerPartitionSnapshot->NextRowIndex);
                    isFirstTuple = false;
                }
            } else {
                consumerPartitionSnapshot->Error = queuePartitionSnapshot->Error;
            }
        }

        nextRowCommitTimeQuery.AppendString(")");

        // TODO(max42): perform action below only if $timestamp is present in queue's schema.
        // Calculate next row commit times and processing lags.
        if (!isFirstTuple) {
            const auto& client = ClientDirectory_->GetClientOrThrow(QueueRef_.Cluster);
            auto query = nextRowCommitTimeQuery.Flush();
            YT_LOG_TRACE("Executing query for next row commit times (Query: %Qv)", query);
            auto result = WaitFor(client->SelectRows(query))
                .ValueOrThrow();

            for (const auto& row : result.Rowset->GetRows()) {
                YT_VERIFY(row.GetCount() == 2);

                const auto& tabletIndexValue = row[0];
                YT_VERIFY(tabletIndexValue.Type == EValueType::Int64);
                auto tabletIndex = tabletIndexValue.Data.Int64;

                const auto& commitTimestampValue = row[1];
                YT_VERIFY(commitTimestampValue.Type == EValueType::Uint64);
                auto commitTimestamp = commitTimestampValue.Data.Uint64;

                auto& consumerPartitionSnapshot = consumerSnapshot->PartitionSnapshots[tabletIndex];
                consumerPartitionSnapshot->NextRowCommitTime = TimestampToInstant(commitTimestamp).first;
            }

            for (const auto& consumerPartitionSnapshot : consumerSnapshot->PartitionSnapshots) {
                if (consumerPartitionSnapshot->NextRowCommitTime) {
                    // If consumer has read all rows in the partition, we assume its processing lag to be zero;
                    // otherwise the processing lag is defined as the duration since the commit time of the next
                    // row in the partition to be read by the consumer.
                    consumerPartitionSnapshot->ProcessingLag = consumerPartitionSnapshot->UnreadRowCount == 0
                        ? TDuration::Zero()
                        : TInstant::Now() - *consumerPartitionSnapshot->NextRowCommitTime;
                }
            }
        }

        YT_LOG_DEBUG("Consumer snapshot updated (Consumer: %Qv)", consumerRef);
    }
};

DEFINE_REFCOUNTED_TYPE(TOrderedDynamicTableController)

////////////////////////////////////////////////////////////////////////////////

IQueueControllerPtr CreateQueueController(
    TQueueControllerConfigPtr config,
    NHiveClient::TClientDirectoryPtr clientDirectory,
    TCrossClusterReference queueRef,
    EQueueFamily queueFamily,
    TQueueTableRow queueRow,
    THashMap<TCrossClusterReference, TConsumerTableRow> consumerRefToRow,
    IInvokerPtr invoker)
{
    switch (queueFamily) {
        case EQueueFamily::OrderedDynamicTable:
            return New<TOrderedDynamicTableController>(
                std::move(config),
                std::move(clientDirectory),
                std::move(queueRef),
                std::move(queueRow),
                std::move(consumerRefToRow),
                std::move(invoker));
        default:
            YT_ABORT();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueAgent
