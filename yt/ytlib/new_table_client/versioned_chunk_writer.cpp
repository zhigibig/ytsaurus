#include "stdafx.h"
#include "versioned_chunk_writer.h"

#include "chunk_meta_extensions.h"
#include "config.h"
#include "versioned_block_writer.h"
#include "versioned_writer.h"
#include "unversioned_row.h"

#include <ytlib/chunk_client/chunk_writer.h>
#include <ytlib/chunk_client/dispatcher.h>
#include <ytlib/chunk_client/encoding_chunk_writer.h>
#include <ytlib/chunk_client/encoding_writer.h>
#include <ytlib/chunk_client/multi_chunk_writer_base.h>

#include <ytlib/table_client/chunk_meta_extensions.h> // TODO(babenko): remove after migration

namespace NYT {
namespace NVersionedTableClient {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NRpc;
using namespace NTableClient::NProto;
using namespace NTransactionClient;
using namespace NVersionedTableClient::NProto;

////////////////////////////////////////////////////////////////////////////////

class TVersionedChunkWriter
    : public IVersionedChunkWriter
{
public:
    TVersionedChunkWriter(
        TChunkWriterConfigPtr config,
        TChunkWriterOptionsPtr options,
        const TTableSchema& schema,
        const TKeyColumns& keyColumns,
        const IChunkWriterPtr& asyncWriter);

    virtual TFuture<void> Open() override;

    virtual bool Write(const std::vector<TVersionedRow>& rows) override;

    virtual TFuture<void> Close() override;

    virtual TFuture<void> GetReadyEvent() override;

    virtual i64 GetMetaSize() const override;
    virtual i64 GetDataSize() const override;

    virtual TChunkMeta GetMasterMeta() const override;
    virtual TChunkMeta GetSchedulerMeta() const override;

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const override;

private:
    TChunkWriterConfigPtr Config_;
    TTableSchema Schema_;
    TKeyColumns KeyColumns_;

    TEncodingChunkWriterPtr EncodingChunkWriter_;

    TOwningKey LastKey_;
    std::unique_ptr<TSimpleVersionedBlockWriter> BlockWriter_;

    TBlockMetaExt BlockMetaExt_;
    i64 BlockMetaExtSize_ = 0;

    TBlockIndexExt BlockIndexExt_;
    i64 BlockIndexExtSize_ = 0;

    TSamplesExt SamplesExt_;
    i64 SamplesExtSize_ = 0;
    double AverageSampleSize_ = 0.0;

    i64 DataWeight_ = 0;

    TBoundaryKeysExt BoundaryKeysExt_;

    i64 RowCount_ = 0;

    TTimestamp MinTimestamp_;
    TTimestamp MaxTimestamp_;

    void WriteRow(
        TVersionedRow row,
        const TUnversionedValue* beginPreviousKey,
        const TUnversionedValue* endPreviousKey);

    void EmitSample(TVersionedRow row);

    void FinishBlockIfLarge(TVersionedRow row);
    void FinishBlock();

    void DoClose();
    void FillCommonMeta(TChunkMeta* meta) const;

    i64 GetUncompressedSize() const;

};

////////////////////////////////////////////////////////////////////////////////

TVersionedChunkWriter::TVersionedChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns,
    const IChunkWriterPtr& asyncWriter)
    : Config_(config)
    , Schema_(schema)
    , KeyColumns_(keyColumns)
    , EncodingChunkWriter_(New<TEncodingChunkWriter>(config, options, asyncWriter))
    , LastKey_(static_cast<TUnversionedValue*>(nullptr), static_cast<TUnversionedValue*>(nullptr))
    , BlockWriter_(new TSimpleVersionedBlockWriter(Schema_, KeyColumns_))
    , MinTimestamp_(MaxTimestamp)
    , MaxTimestamp_(MinTimestamp)
{ }

TFuture<void> TVersionedChunkWriter::Open()
{
    try {
        ValidateTableSchemaAndKeyColumns(Schema_, KeyColumns_);
    } catch (const std::exception& ex) {
        return MakeFuture<void>(ex);
    }
    return VoidFuture;
}

bool TVersionedChunkWriter::Write(const std::vector<TVersionedRow>& rows)
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

TFuture<void> TVersionedChunkWriter::Close()
{
    if (RowCount_ == 0) {
        // Empty chunk.
        return VoidFuture;
    }

    return BIND(&TVersionedChunkWriter::DoClose, MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();
}

TFuture<void> TVersionedChunkWriter::GetReadyEvent()
{
    return EncodingChunkWriter_->GetReadyEvent();
}

i64 TVersionedChunkWriter::GetMetaSize() const
{
    // Other meta parts are negligible.
    return BlockIndexExtSize_ + BlockMetaExtSize_ + SamplesExtSize_;
}

i64 TVersionedChunkWriter::GetDataSize() const
{
    return EncodingChunkWriter_->GetDataStatistics().compressed_data_size() +
        (BlockWriter_ ? BlockWriter_->GetBlockSize() : 0);
}

TChunkMeta TVersionedChunkWriter::GetMasterMeta() const
{
    TChunkMeta meta;
    FillCommonMeta(&meta);
    SetProtoExtension(meta.mutable_extensions(), EncodingChunkWriter_->MiscExt());
    return meta;
}

TChunkMeta TVersionedChunkWriter::GetSchedulerMeta() const
{
    return GetMasterMeta();
}

TDataStatistics TVersionedChunkWriter::GetDataStatistics() const
{
    return EncodingChunkWriter_->GetDataStatistics();
}

void TVersionedChunkWriter::WriteRow(
    TVersionedRow row,
    const TUnversionedValue* beginPreviousKey,
    const TUnversionedValue* endPreviousKey)
{
    double avgRowSize = EncodingChunkWriter_->GetCompressionRatio() * GetUncompressedSize() / RowCount_;
    double sampleProbability = Config_->SampleRate * avgRowSize / AverageSampleSize_;

    if (RandomNumber<double>() < sampleProbability) {
        EmitSample(row);
    }

    ++RowCount_;
    DataWeight_ += GetDataWeight(row);
    BlockWriter_->WriteRow(row, beginPreviousKey, endPreviousKey);
}

void TVersionedChunkWriter::EmitSample(TVersionedRow row)
{
    auto entry = SerializeToString(row.BeginKeys(), row.EndKeys());
    SamplesExt_.add_entries(entry);
    SamplesExtSize_ += entry.length();
    AverageSampleSize_ = static_cast<double>(SamplesExtSize_) / SamplesExt_.entries_size();
}

void TVersionedChunkWriter::FinishBlockIfLarge(TVersionedRow row)
{
    if (BlockWriter_->GetBlockSize() < Config_->BlockSize) {
        return;
    }

    // Emit block index
    ToProto(BlockIndexExt_.add_entries(), row.BeginKeys(), row.EndKeys());
    BlockIndexExtSize_ = BlockIndexExt_.ByteSize();

    FinishBlock();
    BlockWriter_.reset(new TSimpleVersionedBlockWriter(Schema_, KeyColumns_));
}

void TVersionedChunkWriter::FinishBlock()
{
    auto block = BlockWriter_->FlushBlock();
    block.Meta.set_chunk_row_count(RowCount_);
    block.Meta.set_block_index(BlockMetaExt_.entries_size());

    BlockMetaExtSize_ += block.Meta.ByteSize();

    BlockMetaExt_.add_blocks()->Swap(&block.Meta);
    EncodingChunkWriter_->WriteBlock(std::move(block.Data));

    MaxTimestamp_ = std::max(MaxTimestamp_, BlockWriter_->GetMaxTimestamp());
    MinTimestamp_ = std::min(MinTimestamp_, BlockWriter_->GetMinTimestamp());
}

void TVersionedChunkWriter::DoClose()
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
    for (const auto& name : KeyColumns_) {
        keyColumnsExt.add_names(name);
    }
    SetProtoExtension(meta.mutable_extensions(), keyColumnsExt);

    SetProtoExtension(meta.mutable_extensions(), BlockMetaExt_);
    SetProtoExtension(meta.mutable_extensions(), BlockIndexExt_);
    SetProtoExtension(meta.mutable_extensions(), SamplesExt_);

    auto& miscExt = EncodingChunkWriter_->MiscExt();
    miscExt.set_sorted(true);
    miscExt.set_row_count(RowCount_);
    miscExt.set_data_weight(DataWeight_);
    miscExt.set_min_timestamp(MinTimestamp_);
    miscExt.set_max_timestamp(MaxTimestamp_);

    EncodingChunkWriter_->Close();
}

void TVersionedChunkWriter::FillCommonMeta(TChunkMeta* meta) const
{
    meta->set_type(static_cast<int>(EChunkType::Table));
    meta->set_version(static_cast<int>(TSimpleVersionedBlockWriter::FormatVersion));

    SetProtoExtension(meta->mutable_extensions(), BoundaryKeysExt_);
}

i64 TVersionedChunkWriter::GetUncompressedSize() const 
{
    i64 size = EncodingChunkWriter_->GetDataStatistics().uncompressed_data_size();
    if (BlockWriter_) {
        size += BlockWriter_->GetBlockSize();
    }
    return size;
}

////////////////////////////////////////////////////////////////////////////////

IVersionedChunkWriterPtr CreateVersionedChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns,
    IChunkWriterPtr asyncWriter)
{
    return New<TVersionedChunkWriter>(config, options, schema, keyColumns, asyncWriter);
}

////////////////////////////////////////////////////////////////////////////////

class TVersionedMultiChunkWriter
    : public TMultiChunkWriterBase<IVersionedChunkWriter>
    , public IVersionedMultiChunkWriter
{
public:
    TVersionedMultiChunkWriter(
        TTableWriterConfigPtr config,
        TTableWriterOptionsPtr options,
        const TTableSchema& schema,
        const TKeyColumns& keyColumns,
        IChannelPtr masterChannel,
        const TTransactionId& transactionId,
        const TChunkListId& parentChunkListId = NChunkClient::NullChunkListId);

    virtual bool Write(const std::vector<TVersionedRow>& rows) override;

private:
    TTableWriterConfigPtr Config_;
    TTableWriterOptionsPtr Options_;
    TTableSchema Schema_;
    TKeyColumns KeyColumns_;

    IVersionedWriter* CurrentWriter_;


    virtual IChunkWriterBasePtr CreateChunkWriter(IChunkWriterPtr underlyingWriter) override;

};

////////////////////////////////////////////////////////////////////////////////

TVersionedMultiChunkWriter::TVersionedMultiChunkWriter(
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns,
    IChannelPtr masterChannel,
    const TTransactionId& transactionId,
    const TChunkListId& parentChunkListId)
    : TMultiChunkWriterBase<IVersionedChunkWriter>(
          config,
          options,
          masterChannel,
          transactionId,
          parentChunkListId)
    , Config_(config)
    , Options_(options)
    , Schema_(schema)
    , KeyColumns_(keyColumns)
{ }

bool TVersionedMultiChunkWriter::Write(const std::vector<TVersionedRow> &rows)
{
    if (!VerifyActive()) {
        return false;
    }

    // Return true if current writer is ready for more data and
    // we didn't switch to the next chunk.
    return CurrentWriter_->Write(rows) && !TrySwitchSession();
}

IVersionedChunkWriterBasePtr TVersionedMultiChunkWriter::CreateChunkWriter(IChunkWriterPtr underlyingWriter)
{
    return CreateVersionedChunkWriter(Config_, Options_, Schema_, KeyColumns_, underlyingWriter);
}

////////////////////////////////////////////////////////////////////////////////

IVersionedMultiChunkWriterPtr CreateVersionedMultiChunkWriter(
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    const TTableSchema& schema,
    const TKeyColumns& keyColumns,
    NRpc::IChannelPtr masterChannel,
    const NTransactionClient::TTransactionId& transactionId,
    const TChunkListId& parentChunkListId)
{
    return New<TVersionedMultiChunkWriter>(config, options, schema, keyColumns, masterChannel, transactionId, parentChunkListId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
