#include "stdafx.h"
#include "versioned_chunk_writer.h"

#include "chunk_meta_extensions.h"
#include "config.h"
#include "versioned_block_writer.h"
#include "versioned_writer.h"
#include "unversioned_row.h"

#include <ytlib/chunk_client/async_writer.h>
#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/encoding_chunk_writer.h>
#include <ytlib/chunk_client/encoding_writer.h>

#include <ytlib/table_client/chunk_meta_extensions.h> // TODO(babenko): remove after migration

namespace NYT {
namespace NVersionedTableClient {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NTableClient::NProto;
using namespace NVersionedTableClient::NProto;

////////////////////////////////////////////////////////////////////////////////

template <class TBlockWriter>
class TVersionedChunkWriter
    : public IVersionedChunkWriter
{
public:
    TVersionedChunkWriter(
        TChunkWriterConfigPtr config,
        TChunkWriterOptionsPtr options,
        const TTableSchema& schema,
        const TKeyColumns& keyColumns,
        const IAsyncWriterPtr& asyncWriter);

    virtual TAsyncError Open() override;

    virtual bool Write(const std::vector<TVersionedRow>& rows) override;

    virtual TAsyncError Close() override;

    virtual TAsyncError GetReadyEvent() override;

    virtual IVersionedWriter* GetFacade() override;

    virtual i64 GetMetaSize() const override;
    virtual i64 GetDataSize() const override;

    virtual TChunkMeta GetMasterMeta() const override;
    virtual TChunkMeta GetSchedulerMeta() const override;

    virtual NProto::TBoundaryKeysExt GetBoundaryKeys() const override;
    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;

private:
    TChunkWriterConfigPtr Config_;
    TTableSchema Schema_;
    TKeyColumns KeyColumns_;

    TEncodingChunkWriterPtr EncodingChunkWriter_;

    TOwningKey LastKey_;
    std::unique_ptr<TBlockWriter> BlockWriter_;

    TBlockMetaExt BlockMetaExt_;
    i64 BlockMetaExtSize_;

    TBlockIndexExt BlockIndexExt_;
    i64 BlockIndexExtSize_;

    TSamplesExt SamplesExt_;
    i64 SamplesExtSize_;
    double AverageSampleSize_;

    TBoundaryKeysExt BoundaryKeysExt_;

    i64 RowCount_;

    TTimestamp MinTimestamp_;
    TTimestamp MaxTimestamp_;

    void WriteRow(
        TVersionedRow row,
        const TUnversionedValue* beginPreviousKey,
        const TUnversionedValue* endPreviousKey);

    void EmitSample(TVersionedRow row);

    void FinishBlockIfLarge(TVersionedRow row);
    void FinishBlock();

    TError DoClose();
    void FillCommonMeta(TChunkMeta* meta) const;

    i64 GetUncompressedSize() const;

};

////////////////////////////////////////////////////////////////////////////////

template <class TBlockWriter>
TVersionedChunkWriter<TBlockWriter>::TVersionedChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns,
    const IAsyncWriterPtr& asyncWriter)
    : Config_(config)
    , Schema_(schema)
    , KeyColumns_(keyColumns)
    , EncodingChunkWriter_(New<TEncodingChunkWriter>(config, options, asyncWriter))
    , LastKey_(static_cast<TUnversionedValue*>(nullptr), static_cast<TUnversionedValue*>(nullptr))
    , BlockWriter_(new TBlockWriter(Schema_, KeyColumns_))
    , BlockMetaExtSize_(0)
    , BlockIndexExtSize_(0)
    , SamplesExtSize_(0)
    , AverageSampleSize_(0.0)
    , RowCount_(0)
    , MinTimestamp_(MaxTimestamp)
    , MaxTimestamp_(MinTimestamp)
{
    YCHECK(Schema_.Columns().size() > 0);
    YCHECK(KeyColumns_.size() > 0);
    YCHECK(Schema_.CheckKeyColumns(KeyColumns_).IsOK());
}

template <class TBlockWriter>
TAsyncError TVersionedChunkWriter<TBlockWriter>::Open()
{
    return MakeFuture(TError());
}

template <class TBlockWriter>
bool TVersionedChunkWriter<TBlockWriter>::Write(const std::vector<TVersionedRow>& rows)
{
    YCHECK(rows.size() > 0);

    if (RowCount_ == 0) {
        ToProto(
            BoundaryKeysExt_.mutable_min(),
            TOwningKey(rows.front().BeginKeys(), rows.front().EndKeys()));
        EmitSample(rows.front());
    }

    WriteRow(rows.front(), LastKey_.Begin(), LastKey_.End());
    FinishBlockIfLarge(rows.front());

    for (int i = 1; i < rows.size(); ++i) {
        WriteRow(rows[i], rows[i - 1].BeginKeys(), rows[i - 1].EndKeys());
        FinishBlockIfLarge(rows[i]);
    }

    LastKey_ = TOwningKey(rows.back().BeginKeys(), rows.back().EndKeys());
    return EncodingChunkWriter_->IsReady();
}

template <class TBlockWriter>
TAsyncError TVersionedChunkWriter<TBlockWriter>::Close()
{
    if (RowCount_ == 0) {
        // Empty chunk.
        return MakeFuture(TError());
    }

    return BIND(&TVersionedChunkWriter<TBlockWriter>::DoClose, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();
}

template <class TBlockWriter>
TAsyncError TVersionedChunkWriter<TBlockWriter>::GetReadyEvent()
{
    return EncodingChunkWriter_->GetReadyEvent();
}

template <class TBlockWriter>
IVersionedWriter* TVersionedChunkWriter<TBlockWriter>::GetFacade()
{
    return this;
}

template <class TBlockWriter>
i64 TVersionedChunkWriter<TBlockWriter>::GetMetaSize() const
{
    // Other meta parts are negligible.
    return BlockIndexExtSize_ + BlockMetaExtSize_;
}

template <class TBlockWriter>
i64 TVersionedChunkWriter<TBlockWriter>::GetDataSize() const
{
    return EncodingChunkWriter_->GetDataStatistics().compressed_data_size() +
        (BlockWriter_ ? BlockWriter_->GetBlockSize() : 0);
}

template <class TBlockWriter>
TChunkMeta TVersionedChunkWriter<TBlockWriter>::GetMasterMeta() const
{
    TChunkMeta meta;
    FillCommonMeta(&meta);
    SetProtoExtension(meta.mutable_extensions(), EncodingChunkWriter_->MiscExt());
    return meta;
}

template <class TBlockWriter>
TChunkMeta TVersionedChunkWriter<TBlockWriter>::GetSchedulerMeta() const
{
    return GetMasterMeta();
}

template <class TBlockWriter>
TBoundaryKeysExt TVersionedChunkWriter<TBlockWriter>::GetBoundaryKeys() const
{
    return BoundaryKeysExt_;
}

template <class TBlockWriter>
TDataStatistics TVersionedChunkWriter<TBlockWriter>::GetDataStatistics() const
{
    return EncodingChunkWriter_->GetDataStatistics();
}

template <class TBlockWriter>
void TVersionedChunkWriter<TBlockWriter>::WriteRow(
    TVersionedRow row,
    const TUnversionedValue* beginPreviousKey,
    const TUnversionedValue* endPreviousKey)
{
    double sampleProbability =
        Config_->SampleRate * GetUncompressedSize() *
        EncodingChunkWriter_->GetCompressionRatio() / AverageSampleSize_;

    if (RandomNumber<double>() < sampleProbability) {
        EmitSample(row);
    }

    ++RowCount_;
    BlockWriter_->WriteRow(row, beginPreviousKey, endPreviousKey);
}

template <class TBlockWriter>
void TVersionedChunkWriter<TBlockWriter>::EmitSample(TVersionedRow row)
{
    auto entry = SerializeToString(row.BeginKeys(), row.EndKeys());
    SamplesExt_.add_entries(entry);
    SamplesExtSize_ += entry.length();
    AverageSampleSize_ = static_cast<double>(SamplesExtSize_) / SamplesExt_.entries_size();
}

template <class TBlockWriter>
void TVersionedChunkWriter<TBlockWriter>::FinishBlockIfLarge(TVersionedRow row)
{
    if (BlockWriter_->GetBlockSize() < Config_->BlockSize) {
        return;
    }

    // Emit block index
    ToProto(BlockIndexExt_.add_entries(), row.BeginKeys(), row.EndKeys());
    BlockIndexExtSize_ = BlockIndexExt_.ByteSize();

    FinishBlock();
    BlockWriter_.reset(new TBlockWriter(Schema_, KeyColumns_));
}

template <class TBlockWriter>
void TVersionedChunkWriter<TBlockWriter>::FinishBlock()
{
    auto block = BlockWriter_->FlushBlock();
    block.Meta.set_chunk_row_count(RowCount_);

    BlockMetaExtSize_ += block.Meta.ByteSize();

    BlockMetaExt_.add_entries()->Swap(&block.Meta);
    EncodingChunkWriter_->WriteBlock(std::move(block.Data));

    MaxTimestamp_ = std::max(MaxTimestamp_, BlockWriter_->GetMaxTimestamp());
    MinTimestamp_ = std::min(MinTimestamp_, BlockWriter_->GetMinTimestamp());
}

template <class TBlockWriter>
TError TVersionedChunkWriter<TBlockWriter>::DoClose()
{
    using NYT::ToProto;

    if (BlockWriter_->GetRowCount() > 0) {
        FinishBlock();
    }

    ToProto(BoundaryKeysExt_.mutable_max(), LastKey_);

    auto& meta = EncodingChunkWriter_->Meta();
    FillCommonMeta(&meta);

    SetProtoExtension(meta.mutable_extensions(), ToProto<TTableSchemaExt>(Schema_));

    TKeyColumnsExt keyColumnsExt;
    for (auto name : KeyColumns_) {
        keyColumnsExt.add_names(name);
    }
    SetProtoExtension(meta.mutable_extensions(), keyColumnsExt);

    SetProtoExtension(meta.mutable_extensions(), BlockMetaExt_);
    SetProtoExtension(meta.mutable_extensions(), BlockIndexExt_);
    SetProtoExtension(meta.mutable_extensions(), SamplesExt_);

    auto& miscExt = EncodingChunkWriter_->MiscExt();
    miscExt.set_sorted(true);
    miscExt.set_row_count(RowCount_);
    miscExt.set_min_timestamp(MinTimestamp_);
    miscExt.set_max_timestamp(MaxTimestamp_);

    return EncodingChunkWriter_->Close();
}

template <class TBlockWriter>
void TVersionedChunkWriter<TBlockWriter>::FillCommonMeta(TChunkMeta* meta) const
{
    meta->set_type(EChunkType::Table);
    meta->set_version(TBlockWriter::FormatVersion);

    SetProtoExtension(meta->mutable_extensions(), BoundaryKeysExt_);
}

template <class TBlockWriter>
i64 TVersionedChunkWriter<TBlockWriter>::GetUncompressedSize() const 
{
    i64 size = EncodingChunkWriter_->GetDataStatistics().uncompressed_data_size();
    if (BlockWriter_) {
        size += BlockWriter_->GetBlockSize();
    }
    return size;
}

////////////////////////////////////////////////////////////////////////////////

TVersionedChunkWriterProvider::TVersionedChunkWriterProvider(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns)
    : Config_(config)
    , Options_(options)
    , Schema_(schema)
    , KeyColumns_(keyColumns)
    , CreatedWriterCount_(0)
    , FinishedWriterCount_(0)
{
    BoundaryKeysExt_.mutable_min();
    BoundaryKeysExt_.mutable_max();
}

IVersionedChunkWriterPtr TVersionedChunkWriterProvider::CreateChunkWriter(IAsyncWriterPtr asyncWriter)
{
    YCHECK(FinishedWriterCount_ == CreatedWriterCount_);

    CurrentWriter_ = CreateVersionedChunkWriter(
        Config_,
        Options_,
        Schema_,
        KeyColumns_,
        asyncWriter);

    ++CreatedWriterCount_;

    TGuard<TSpinLock> guard(SpinLock_);
    YCHECK(ActiveWriters_.insert(CurrentWriter_).second);

    return CurrentWriter_;
}

void TVersionedChunkWriterProvider::OnChunkFinished()
{
    ++FinishedWriterCount_;
    YCHECK(FinishedWriterCount_ == CreatedWriterCount_);

    if (FinishedWriterCount_ == 1) {
        BoundaryKeysExt_ = CurrentWriter_->GetBoundaryKeys();
    } else {
        auto boundaryKeys = CurrentWriter_->GetBoundaryKeys();
        BoundaryKeysExt_.set_max(boundaryKeys.min());
    }
}

void TVersionedChunkWriterProvider::OnChunkClosed(IVersionedChunkWriterPtr writer)
{
    TGuard<TSpinLock> guard(SpinLock_);
    DataStatistics_ += writer->GetDataStatistics();
    YCHECK(ActiveWriters_.erase(writer) == 1);
}

TDataStatistics TVersionedChunkWriterProvider::GetDataStatistics() const
{
    TGuard<TSpinLock> guard(SpinLock_);
    auto result = DataStatistics_;
    for (const auto& writer : ActiveWriters_) {
        result += writer->GetDataStatistics();
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

IVersionedChunkWriterPtr CreateVersionedChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns,
    IAsyncWriterPtr asyncWriter)
{
    return New<TVersionedChunkWriter<TSimpleVersionedBlockWriter>>(
        config,
        options,
        schema, 
        keyColumns,
        asyncWriter);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
