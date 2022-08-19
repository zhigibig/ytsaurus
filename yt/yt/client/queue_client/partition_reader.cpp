#include "partition_reader.h"
#include "private.h"
#include "queue_rowset.h"
#include "common.h"
#include "consumer_client.h"

#include <yt/yt/client/api/transaction.h>

#include <yt/yt/client/ypath/rich.h>

#include <yt/yt/core/profiling/timing.h>

namespace NYT::NQueueClient {

using namespace NApi;
using namespace NConcurrency;
using namespace NProfiling;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NYPath;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TPartitionReader
    : public IPartitionReader
{
public:
    TPartitionReader(
        TPartitionReaderConfigPtr config,
        IClientPtr client,
        TYPath consumerPath,
        int partitionIndex)
        : Config_(std::move(config))
        , Client_(std::move(client))
        , ConsumerPath_(std::move(consumerPath))
        , PartitionIndex_(partitionIndex)
        , Logger(QueueClientLogger.WithTag("Consumer: %Qv, Partition: %v", ConsumerPath_, PartitionIndex_))
        , RowBatchReadOptions_({Config_->MaxRowCount, Config_->MaxDataWeight, Config_->DataWeightPerRowHint})
    { }

    TFuture<void> Open() override
    {
        return BIND(&TPartitionReader::DoOpen, MakeStrong(this))
            .AsyncVia(GetCurrentInvoker())
            .Run();
    }

    TFuture<IPersistentQueueRowsetPtr> Read() override
    {
        YT_VERIFY(Opened_);

        return BIND(&TPartitionReader::DoRead, MakeStrong(this))
            .AsyncVia(GetCurrentInvoker())
            .Run();
    }

private:
    const TPartitionReaderConfigPtr Config_;
    const IClientPtr Client_;
    TYPath ConsumerPath_;
    int PartitionIndex_;
    NLogging::TLogger Logger;

    bool Opened_ = false;

    TYPath QueuePath_;
    i64 ApproximateDataWeightPerRow_ = 0;
    IConsumerClientPtr ConsumerClient_;
    TQueueRowBatchReadOptions RowBatchReadOptions_;

    class TPersistentQueueRowset
        : public IPersistentQueueRowset
    {
    public:
        TPersistentQueueRowset(IQueueRowsetPtr rowset, TWeakPtr<TPartitionReader> partitionReader, i64 currentOffset)
            : Rowset_(std::move(rowset))
            , PartitionReader_(std::move(partitionReader))
            , CurrentOffset_(currentOffset)
        { }

        const NTableClient::TTableSchemaPtr& GetSchema() const override
        {
            return Rowset_->GetSchema();
        }

        const NTableClient::TNameTablePtr& GetNameTable() const override
        {
            return Rowset_->GetNameTable();
        }

        TRange<NTableClient::TUnversionedRow> GetRows() const override
        {
            return Rowset_->GetRows();
        }

        TSharedRange<NTableClient::TUnversionedRow> GetSharedRange() const override
        {
            return Rowset_->GetSharedRange();
        }

        i64 GetStartOffset() const override
        {
            return Rowset_->GetStartOffset();
        }

        i64 GetFinishOffset() const override
        {
            return Rowset_->GetFinishOffset();
        }

        void Commit(const NApi::ITransactionPtr& transaction) override
        {
            YT_VERIFY(transaction);

            if (auto partitionReader = PartitionReader_.Lock()) {
                // TODO(achulkov2): Check that this is the first uncommitted batch returned to the user and crash otherwise.
                // Will be much easier to do once we figure out how prefetching & batch storage will look like.

                // TODO(achulkov2): Mark this batch as committed in the partition reader.

                transaction->AdvanceConsumer(
                    partitionReader->ConsumerPath_,
                    partitionReader->PartitionIndex_,
                    CurrentOffset_,
                    GetFinishOffset());
            } else {
                THROW_ERROR_EXCEPTION("Partition reader destroyed");
            }
        }

    private:
        IQueueRowsetPtr Rowset_;
        TWeakPtr<TPartitionReader> PartitionReader_;
        i64 CurrentOffset_;
    };

    IPersistentQueueRowsetPtr DoRead()
    {
        YT_LOG_DEBUG("Reading rowset");
        TWallTimer timer;

        if (!Config_->DataWeightPerRowHint && ApproximateDataWeightPerRow_) {
            RowBatchReadOptions_.DataWeightPerRowHint = ApproximateDataWeightPerRow_;
        }

        auto currentOffset = FetchCurrentOffset();

        // TODO(achulkov2): Log the options in the RPC method (via SetRequestInfo) instead?
        YT_LOG_DEBUG(
            "Pulling from queue (Offset: %v, MaxRowCount: %v, MaxDataWeight: %v, DataWeightPerRowHint: %v)",
            currentOffset,
            RowBatchReadOptions_.MaxRowCount,
            RowBatchReadOptions_.MaxDataWeight,
            RowBatchReadOptions_.DataWeightPerRowHint);
        auto rowset = WaitFor(Client_->PullQueue(QueuePath_, currentOffset, PartitionIndex_, RowBatchReadOptions_))
            .ValueOrThrow();

        HandleRowset(rowset);

        YT_LOG_DEBUG("Rowset read (WallTime: %v)", timer.GetElapsedTime());

        return New<TPersistentQueueRowset>(rowset, MakeWeak(this), currentOffset);

    }

    void HandleRowset(const IQueueRowsetPtr& rowset)
    {
        // TODO(achulkov2): When prefetching is implemented we'll have some sort of struct for holding the batch + stats.
        // TODO(achulkov2): Don't burn CPU here and get the data weight from PullQueue somehow.
        auto dataWeight = static_cast<i64>(GetDataWeight(rowset->GetRows()));
        i64 rowCount = std::ssize(rowset->GetRows());

        RecomputeApproximateDataWeightPerRow(dataWeight, rowCount);

        YT_LOG_DEBUG(
            "Rowset obtained (RowCount: %v, DataWeight: %v, StartOffset: %v, FinishOffset: %v)",
            rowCount,
            dataWeight,
            rowset->GetStartOffset(),
            rowset->GetFinishOffset());
    }

    void RecomputeApproximateDataWeightPerRow(i64 dataWeight, i64 rowCount)
    {
        if (rowCount == 0) {
            return;
        }

        auto newDataWeightPerRowHint = dataWeight / rowCount;

        if (ApproximateDataWeightPerRow_) {
            // TODO(achulkov2): Anything better? Variable coefficient?
            ApproximateDataWeightPerRow_ = (ApproximateDataWeightPerRow_ + newDataWeightPerRowHint) / 2;
        } else {
            ApproximateDataWeightPerRow_ = newDataWeightPerRowHint;
        }

        YT_LOG_DEBUG("Recomputed approximate data weight per row (ApproximateDataWeightPerRow: %v)", ApproximateDataWeightPerRow_);
    }

    // NB: Can throw.
    i64 FetchCurrentOffset() const
    {
        TWallTimer timer;

        std::vector<int> partitionIndexesToFetch{PartitionIndex_};
        auto partitions = WaitFor(ConsumerClient_->CollectPartitions(Client_, partitionIndexesToFetch))
            .ValueOrThrow();

        YT_VERIFY(partitions.size() <= 1);

        i64 currentOffset = 0;

        if (!partitions.empty()) {
            YT_VERIFY(partitions[0].PartitionIndex == PartitionIndex_);
            currentOffset = partitions[0].NextRowIndex;
        }

        YT_LOG_DEBUG("Fetched current offset (Offset: %v, WallTime: %v)", currentOffset, timer.GetElapsedTime());
        return currentOffset;
    }

    void DoOpen()
    {
        YT_LOG_DEBUG("Opening partition reader");

        ConsumerClient_ = CreateConsumerClient(Client_, ConsumerPath_);

        QueuePath_ = WaitFor(ConsumerClient_->FetchTargetQueue(Client_))
            .ValueOrThrow().Path;

        Logger.AddTag("Queue: %Qv", QueuePath_);

        auto partitionStatistics = WaitFor(ConsumerClient_->FetchPartitionStatistics(Client_, QueuePath_, PartitionIndex_))
            .ValueOrThrow();

        RecomputeApproximateDataWeightPerRow(partitionStatistics.FlushedDataWeight, partitionStatistics.FlushedRowCount);

        Opened_ = true;

        YT_LOG_DEBUG("Partition reader opened");
    }
};

IPartitionReaderPtr CreatePartitionReader(
    TPartitionReaderConfigPtr config,
    IClientPtr client,
    TYPath path,
    int partitionIndex)
{
    return New<TPartitionReader>(std::move(config), std::move(client), std::move(path), partitionIndex);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueueClient
