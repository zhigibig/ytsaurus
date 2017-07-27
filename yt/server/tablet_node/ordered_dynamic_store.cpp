#include "ordered_dynamic_store.h"
#include "tablet.h"
#include "transaction.h"
#include "automaton.h"
#include "config.h"

#include <yt/core/ytree/fluent.h>

#include <yt/core/misc/chunked_memory_pool.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>
#include <yt/ytlib/table_client/schemaful_chunk_reader.h>
#include <yt/ytlib/table_client/schemaful_writer_adapter.h>
#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/schemaful_writer.h>
#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/cached_versioned_chunk_meta.h>

#include <yt/ytlib/chunk_client/config.h>
#include <yt/ytlib/chunk_client/memory_reader.h>
#include <yt/ytlib/chunk_client/memory_writer.h>
#include <yt/ytlib/chunk_client/chunk_spec.pb.h>
#include <yt/ytlib/chunk_client/chunk_meta.pb.h>

namespace NYT {
namespace NTabletNode {

using namespace NYTree;
using namespace NYson;
using namespace NTableClient;
using namespace NTransactionClient;
using namespace NChunkClient;
using namespace NConcurrency;

using NChunkClient::NProto::TChunkSpec;
using NChunkClient::NProto::TChunkMeta;
using NChunkClient::NProto::TDataStatistics;

////////////////////////////////////////////////////////////////////////////////

static const size_t ReaderPoolSize = (size_t) 16 * KB;
static const int SnapshotRowsPerRead = 1024;

struct TOrderedDynamicStoreReaderPoolTag
{ };

////////////////////////////////////////////////////////////////////////////////

class TOrderedDynamicStore::TReader
    : public ISchemafulReader
{
public:
    TReader(
        TOrderedDynamicStorePtr store,
        int tabletIndex,
        i64 lowerRowIndex,
        i64 upperRowIndex,
        const TNullable<TColumnFilter>& maybeColumnFilter)
        : Store_(std::move(store))
        , TabletIndex_(tabletIndex)
        , UpperRowIndex_(std::min(upperRowIndex, Store_->GetStartingRowIndex() + Store_->GetRowCount()))
        , MaybeColumnFilter_(maybeColumnFilter)
        , CurrentRowIndex_(std::max(lowerRowIndex, Store_->GetStartingRowIndex()))
    { }

    void Initialize()
    {
        if (!MaybeColumnFilter_) {
            // For flushes and snapshots only.
            return;
        }

        if (MaybeColumnFilter_->All) {
            MaybeColumnFilter_->All = false;
            MaybeColumnFilter_->Indexes.clear();
            // +2 is for (tablet_index, row_index).
            for (int id = 0; id < static_cast<int>(Store_->Schema_.Columns().size()) + 2; ++id) {
                MaybeColumnFilter_->Indexes.push_back(id);
            }
        }

        Pool_ = std::make_unique<TChunkedMemoryPool>(TOrderedDynamicStoreReaderPoolTag(), ReaderPoolSize);
    }

    virtual bool Read(std::vector<TUnversionedRow>* rows) override
    {
        rows->clear();
        while (rows->size() < rows->capacity() && CurrentRowIndex_ < UpperRowIndex_) {
            rows->push_back(CaptureRow(Store_->GetRow(CurrentRowIndex_)));
            ++CurrentRowIndex_;
            ++RowCount_;
            DataWeight_ += GetDataWeight(rows->back());
        }
        return !rows->empty();
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        Y_UNREACHABLE();
    }

    virtual TDataStatistics GetDataStatistics() const override
    {
        TDataStatistics dataStatistics;
        dataStatistics.set_row_count(RowCount_);
        dataStatistics.set_data_weight(DataWeight_);
        return dataStatistics;
    }

private:
    const TOrderedDynamicStorePtr Store_;
    const int TabletIndex_;
    const i64 UpperRowIndex_;
    TNullable<TColumnFilter> MaybeColumnFilter_;

    std::unique_ptr<TChunkedMemoryPool> Pool_;

    i64 CurrentRowIndex_;
    i64 RowCount_ = 0;
    i64 DataWeight_ = 0;


    TUnversionedRow CaptureRow(TOrderedDynamicRow dynamicRow)
    {
        if (!MaybeColumnFilter_) {
            // For flushes and snapshots only.
            return dynamicRow;
        }

        int columnCount = static_cast<int>(MaybeColumnFilter_->Indexes.size());
        auto row = TMutableUnversionedRow::Allocate(Pool_.get(), columnCount);
        for (int index = 0; index < columnCount; ++index) {
            int id = MaybeColumnFilter_->Indexes[index];
            auto& dstValue = row[index];
            if (id == 0) {
                dstValue = MakeUnversionedInt64Value(TabletIndex_, id);
            } else if (id == 1) {
                dstValue = MakeUnversionedInt64Value(CurrentRowIndex_, id);
            } else {
                dstValue = dynamicRow[id - 2];
                dstValue.Id = id;
            }
        }
        return row;
    }
};

////////////////////////////////////////////////////////////////////////////////

namespace {

TNullable<int> GetTimestampColumnId(const TTableSchema& schema)
{
    const auto* column = schema.FindColumn(TimestampColumnName);
    if (!column) {
        return Null;
    }
    return schema.GetColumnIndex(*column);
}

} // namespace

TOrderedDynamicStore::TOrderedDynamicStore(
    TTabletManagerConfigPtr config,
    const TStoreId& id,
    TTablet* tablet)
    : TStoreBase(config, id, tablet)
    , TDynamicStoreBase(config, id, tablet)
    , TOrderedStoreBase(config, id, tablet)
    , TimestampColumnId_(GetTimestampColumnId(Schema_))
{
    AllocateCurrentSegment(InitialOrderedDynamicSegmentIndex);

    LOG_DEBUG("Ordered dynamic store created");
}

TOrderedDynamicStore::~TOrderedDynamicStore()
{
    LOG_DEBUG("Ordered dynamic memory store destroyed");
}

ISchemafulReaderPtr TOrderedDynamicStore::CreateFlushReader()
{
    YCHECK(FlushRowCount_ != -1);
    return DoCreateReader(
        -1,
        StartingRowIndex_,
        StartingRowIndex_ + FlushRowCount_,
        Null);
}

ISchemafulReaderPtr TOrderedDynamicStore::CreateSnapshotReader()
{
    return DoCreateReader(
        -1,
        StartingRowIndex_,
        StartingRowIndex_ + GetRowCount(),
        Null);
}

TOrderedDynamicRow TOrderedDynamicStore::WriteRow(
    TUnversionedRow row,
    TWriteContext* context)
{
    Y_ASSERT(context->Phase == EWritePhase::Commit);

    int columnCount = static_cast<int>(Schema_.Columns().size());
    auto dynamicRow = RowBuffer_->AllocateUnversioned(columnCount);

    for (int index = 0; index < columnCount; ++index) {
        dynamicRow[index] = MakeUnversionedSentinelValue(EValueType::Null, index);
    }

    for (const auto& srcValue : row) {
        auto& dstValue = dynamicRow[srcValue.Id];
        dstValue = RowBuffer_->Capture(srcValue);
    }

    if (TimestampColumnId_) {
        dynamicRow[*TimestampColumnId_] = MakeUnversionedUint64Value(context->CommitTimestamp, *TimestampColumnId_);
    }

    CommitRow(dynamicRow);
    UpdateTimestampRange(context->CommitTimestamp);
    OnMemoryUsageUpdated();

    ++PerformanceCounters_->DynamicRowWriteCount;
    ++context->RowCount;
    context->ByteSize += GetDataWeight(row);

    return dynamicRow;
}

TOrderedDynamicRow TOrderedDynamicStore::GetRow(i64 rowIndex)
{
    rowIndex -= StartingRowIndex_;
    Y_ASSERT(rowIndex >= 0 && rowIndex < StoreRowCount_);
    int segmentIndex;
    i64 segmentRowIndex;
    if (rowIndex < (1ULL << InitialOrderedDynamicSegmentIndex)) {
        segmentIndex = InitialOrderedDynamicSegmentIndex;
        segmentRowIndex = rowIndex;
    } else {
        segmentIndex = 64 - __builtin_clzl(rowIndex);
        segmentRowIndex = rowIndex - (1ULL << (segmentIndex - 1));
    }
    return TOrderedDynamicRow((*Segments_[segmentIndex])[segmentRowIndex]);
}

std::vector<TOrderedDynamicRow> TOrderedDynamicStore::GetAllRows()
{
    std::vector<TOrderedDynamicRow> rows;
    for (i64 index = StartingRowIndex_; index < StartingRowIndex_ + StoreRowCount_; ++index) {
        rows.push_back(GetRow(index));
    }
    return rows;
}

EStoreType TOrderedDynamicStore::GetType() const
{
    return EStoreType::OrderedDynamic;
}

i64 TOrderedDynamicStore::GetRowCount() const
{
    return StoreRowCount_;
}

TCallback<void(TSaveContext&)> TOrderedDynamicStore::AsyncSave()
{
    using NYT::Save;

    auto tableReader = CreateSnapshotReader();

    return BIND([=, this_ = MakeStrong(this)] (TSaveContext& context) {
        LOG_DEBUG("Store snapshot serialization started");

        auto chunkWriter = New<TMemoryWriter>();
        auto tableWriterConfig = New<TChunkWriterConfig>();
        auto tableWriterOptions = New<TChunkWriterOptions>();
        tableWriterOptions->OptimizeFor = EOptimizeFor::Scan;

        auto schemalessTableWriter = CreateSchemalessChunkWriter(
            tableWriterConfig,
            tableWriterOptions,
            Schema_,
            chunkWriter);
        auto tableWriter = CreateSchemafulWriterAdapter(schemalessTableWriter);

        LOG_DEBUG("Opening table writer");
        WaitFor(schemalessTableWriter->Open())
            .ThrowOnError();

        std::vector<TUnversionedRow> rows;
        rows.reserve(SnapshotRowsPerRead);

        LOG_DEBUG("Serializing store snapshot");

        i64 rowCount = 0;
        while (tableReader->Read(&rows)) {
            if (rows.empty()) {
                LOG_DEBUG("Waiting for table reader");
                WaitFor(tableReader->GetReadyEvent())
                    .ThrowOnError();
                continue;
            }

            rowCount += rows.size();
            if (!tableWriter->Write(rows)) {
                LOG_DEBUG("Waiting for table writer");
                WaitFor(tableWriter->GetReadyEvent())
                    .ThrowOnError();
            }
        }

        // pushsin@ forbids empty chunks.
        if (rowCount == 0) {
            Save(context, false);
            return;
        }

        Save(context, true);

        // NB: This also closes chunkWriter.
        LOG_DEBUG("Closing table writer");
        WaitFor(tableWriter->Close())
            .ThrowOnError();

        Save(context, chunkWriter->GetChunkMeta());

        auto blocks = TBlock::Unwrap(chunkWriter->GetBlocks());
        LOG_DEBUG("Writing store blocks (RowCount: %v, BlockCount: %v, ByteSize: %v)",
            rowCount,
            blocks.size());

        Save(context, blocks);

        LOG_DEBUG("Store snapshot serialization complete");
    });
}

void TOrderedDynamicStore::AsyncLoad(TLoadContext& context)
{
    using NYT::Load;

    if (Load<bool>(context)) {
        auto chunkMeta = Load<TChunkMeta>(context);
        auto blocks = Load<std::vector<TSharedRef>>(context);

        auto chunkReader = CreateMemoryReader(chunkMeta, TBlock::Wrap(blocks));
        auto tableReader = CreateSchemafulChunkReader(
            New<TChunkReaderConfig>(),
            chunkReader,
            GetNullBlockCache(),
            Schema_,
            TKeyColumns(),
            chunkMeta,
            TReadRange());

        std::vector<TUnversionedRow> rows;
        rows.reserve(SnapshotRowsPerRead);

        while (tableReader->Read(&rows)) {
            if (rows.empty()) {
                WaitFor(tableReader->GetReadyEvent())
                    .ThrowOnError();
                continue;
            }

            for (auto row : rows) {
                LoadRow(row);
            }
        }
    }

    // Cf. YT-4534
    if (StoreState_ == EStoreState::PassiveDynamic ||
        StoreState_ == EStoreState::RemovePrepared)
    {
        // NB: No more changes are possible after load.
        YCHECK(FlushRowCount_ == -1);
        FlushRowCount_ = GetRowCount();
    }

    OnMemoryUsageUpdated();
}

TOrderedDynamicStorePtr TOrderedDynamicStore::AsOrderedDynamic()
{
    return this;
}

ISchemafulReaderPtr TOrderedDynamicStore::CreateReader(
    const TTabletSnapshotPtr& /*tabletSnapshot*/,
    int tabletIndex,
    i64 lowerRowIndex,
    i64 upperRowIndex,
    const TColumnFilter& columnFilter,
    const TWorkloadDescriptor& /*workloadDescriptor*/)
{
    return DoCreateReader(
        tabletIndex,
        lowerRowIndex,
        upperRowIndex,
        columnFilter);
}

void TOrderedDynamicStore::OnSetPassive()
{
    YCHECK(FlushRowCount_ == -1);
    FlushRowCount_ = GetRowCount();
}

void TOrderedDynamicStore::AllocateCurrentSegment(int index)
{
    CurrentSegmentIndex_ = index;
    CurrentSegmentCapacity_ = 1LL << (index - (index == InitialOrderedDynamicSegmentIndex ? 0 : 1));
    CurrentSegmentSize_ = 0;
    Segments_[CurrentSegmentIndex_] = std::make_unique<TOrderedDynamicRowSegment>(CurrentSegmentCapacity_);
}

void TOrderedDynamicStore::OnMemoryUsageUpdated()
{
    SetMemoryUsage(GetUncompressedDataSize());
}

void TOrderedDynamicStore::CommitRow(TOrderedDynamicRow row)
{
    if (CurrentSegmentSize_ == CurrentSegmentCapacity_) {
        AllocateCurrentSegment(CurrentSegmentIndex_ + 1);
    }
    (*Segments_[CurrentSegmentIndex_])[CurrentSegmentSize_] = row.GetHeader();
    ++CurrentSegmentSize_;
    StoreRowCount_ += 1;
    StoreValueCount_ += row.GetCount();
}

void TOrderedDynamicStore::LoadRow(TUnversionedRow row)
{
    CommitRow(RowBuffer_->Capture(row, true));
}

ISchemafulReaderPtr TOrderedDynamicStore::DoCreateReader(
    int tabletIndex,
    i64 lowerRowIndex,
    i64 upperRowIndex,
    const TNullable<TColumnFilter>& maybeColumnFilter)
{
    auto reader = New<TReader>(
        this,
        tabletIndex,
        lowerRowIndex,
        upperRowIndex,
        maybeColumnFilter);
    reader->Initialize();
    return reader;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
