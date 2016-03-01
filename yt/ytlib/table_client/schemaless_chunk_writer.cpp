#include "schemaless_chunk_writer.h"
#include "chunk_meta_extensions.h"
#include "chunk_writer_base.h"
#include "config.h"
#include "name_table.h"
#include "partitioner.h"
#include "schemaless_block_writer.h"
#include "schemaless_row_reorderer.h"
#include "table_ypath_proxy.h"

#include <yt/ytlib/api/client.h>

#include <yt/ytlib/chunk_client/chunk_writer.h>
#include <yt/ytlib/chunk_client/dispatcher.h>
#include <yt/ytlib/chunk_client/encoding_chunk_writer.h>
#include <yt/ytlib/chunk_client/multi_chunk_writer_base.h>

#include <yt/ytlib/cypress_client/cypress_ypath_proxy.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/transaction_client/helpers.h>
#include <yt/ytlib/transaction_client/transaction_listener.h>
#include <yt/ytlib/transaction_client/transaction_manager.h>

#include <yt/ytlib/ypath/rich.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/common.h>

#include <yt/core/rpc/channel.h>

#include <yt/core/ytree/attribute_helpers.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NCypressClient;
using namespace NObjectClient;
using namespace NTableClient::NProto;
using namespace NRpc;
using namespace NApi;
using namespace NTransactionClient;
using namespace NNodeTrackerClient;
using namespace NYPath;
using namespace NYTree;

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

template <class TBase>
class TSchemalessChunkWriter
    : public TBase
    , public ISchemalessChunkWriter
{
public:
    TSchemalessChunkWriter(
        TChunkWriterConfigPtr config,
        TChunkWriterOptionsPtr options,
        TNameTablePtr nameTable,
        IChunkWriterPtr chunkWriter,
        IBlockCachePtr blockCache,
        const TKeyColumns& keyColumns = TKeyColumns());

    virtual bool Write(const std::vector<TUnversionedRow>& rows) override;

    virtual TNameTablePtr GetNameTable() const override;

    virtual bool IsSorted() const override;

private:
    const TNameTablePtr NameTable_;

    THorizontalSchemalessBlockWriter* CurrentBlockWriter_;

    virtual ETableChunkFormat GetFormatVersion() const override;
    virtual IBlockWriter* CreateBlockWriter() override;

    virtual void PrepareChunkMeta() override;

    virtual i64 GetMetaSize() const override;
};

////////////////////////////////////////////////////////////////////////////////

template <class TBase>
TSchemalessChunkWriter<TBase>::TSchemalessChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    TNameTablePtr nameTable,
    IChunkWriterPtr chunkWriter,
    IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns)
    : TBase(
        config,
        options,
        chunkWriter,
        blockCache,
        keyColumns)
    , NameTable_(nameTable)
{ }

template <class TBase>
bool TSchemalessChunkWriter<TBase>::Write(const std::vector<TUnversionedRow>& rows)
{
    YCHECK(CurrentBlockWriter_);

    for (auto row : rows) {
        this->ValidateDuplicateIds(row, NameTable_);
        CurrentBlockWriter_->WriteRow(row);
        this->OnRow(row);
    }

    return TBase::EncodingChunkWriter_->IsReady();
}

template <class TBase>
ETableChunkFormat TSchemalessChunkWriter<TBase>::GetFormatVersion() const
{
    return ETableChunkFormat::SchemalessHorizontal;
}

template <class TBase>
void TSchemalessChunkWriter<TBase>::PrepareChunkMeta()
{
    TBase::PrepareChunkMeta();

    auto& meta = TBase::EncodingChunkWriter_->Meta();
    TNameTableExt nameTableExt;
    ToProto(&nameTableExt, NameTable_);

    SetProtoExtension(meta.mutable_extensions(), nameTableExt);
}

template <class TBase>
IBlockWriter* TSchemalessChunkWriter<TBase>::CreateBlockWriter()
{
    CurrentBlockWriter_ = new THorizontalSchemalessBlockWriter();
    return CurrentBlockWriter_;
}

template <class TBase>
TNameTablePtr TSchemalessChunkWriter<TBase>::GetNameTable() const
{
    return NameTable_;
}

template <class TBase>
bool TSchemalessChunkWriter<TBase>::IsSorted() const
{
    return TBase::IsSorted();
}

template <class TBase>
i64 TSchemalessChunkWriter<TBase>::GetMetaSize() const
{
    return NameTable_->GetByteSize() + TBase::GetMetaSize();
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessChunkWriterPtr CreateSchemalessChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    TNameTablePtr nameTable,
    const TKeyColumns& keyColumns,
    IChunkWriterPtr chunkWriter,
    IBlockCachePtr blockCache)
{
    if (keyColumns.empty()) {
        return New<TSchemalessChunkWriter<TSequentialChunkWriterBase>>(
            config,
            options,
            nameTable,
            chunkWriter,
            blockCache);
    } else {
        return New<TSchemalessChunkWriter<TSortedChunkWriterBase>>(
            config,
            options,
            nameTable,
            chunkWriter,
            blockCache,
            keyColumns);
    }
}

////////////////////////////////////////////////////////////////////////////////

class TPartitionChunkWriter
    : public ISchemalessChunkWriter
    , public TChunkWriterBase
{
public:
    TPartitionChunkWriter(
        TChunkWriterConfigPtr config,
        TChunkWriterOptionsPtr options,
        TNameTablePtr nameTable,
        IChunkWriterPtr chunkWriter,
        IBlockCachePtr blockCache,
        const TKeyColumns& keyColumns,
        IPartitioner* partitioner);

    virtual bool Write(const std::vector<TUnversionedRow>& rows) override;

    virtual TNameTablePtr GetNameTable() const override;

    virtual i64 GetDataSize() const override;

    virtual TChunkMeta GetSchedulerMeta() const override;

    virtual i64 GetMetaSize() const override;

    virtual bool IsSorted() const override;

private:
    const TNameTablePtr NameTable_;
    const TKeyColumns KeyColumns_;

    TPartitionsExt PartitionsExt_;

    IPartitioner* Partitioner_;

    std::vector<std::unique_ptr<THorizontalSchemalessBlockWriter>> BlockWriters_;

    i64 CurrentBufferCapacity_ = 0;

    int LargestPartitionIndex_ = 0;
    i64 LargestPartitionSize_ = 0;

    i64 BlockReserveSize_;

    i64 FlushedRowCount_ = 0;


    void WriteRow(TUnversionedRow row);

    void InitLargestPartition();
    void FlushBlock(int partitionIndex);

    virtual void DoClose() override;
    virtual void PrepareChunkMeta() override;

    virtual ETableChunkFormat GetFormatVersion() const override;

};

////////////////////////////////////////////////////////////////////////////////

TPartitionChunkWriter::TPartitionChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    TNameTablePtr nameTable,
    IChunkWriterPtr chunkWriter,
    IBlockCachePtr blockCache,
    const TKeyColumns& keyColumns,
    IPartitioner* partitioner)
    : TChunkWriterBase(
        config,
        options,
        chunkWriter,
        blockCache)
    , NameTable_(nameTable)
    , KeyColumns_(keyColumns)
    , Partitioner_(partitioner)
{
    int partitionCount = Partitioner_->GetPartitionCount();
    BlockWriters_.reserve(partitionCount);

    BlockReserveSize_ = Config_->MaxBufferSize / partitionCount;

    for (int partitionIndex = 0; partitionIndex < partitionCount; ++partitionIndex) {
        BlockWriters_.emplace_back(new THorizontalSchemalessBlockWriter(BlockReserveSize_));
        CurrentBufferCapacity_ += BlockWriters_.back()->GetCapacity();

        auto* partitionAttributes = PartitionsExt_.add_partitions();
        partitionAttributes->set_row_count(0);
        partitionAttributes->set_uncompressed_data_size(0);
    }
}

bool TPartitionChunkWriter::Write(const std::vector<TUnversionedRow>& rows)
{
    for (auto& row : rows) {
        this->ValidateDuplicateIds(row, NameTable_);
        WriteRow(row);
    }

    return EncodingChunkWriter_->IsReady();
}

void TPartitionChunkWriter::WriteRow(TUnversionedRow row)
{
    ++RowCount_;

    i64 weight = GetDataWeight(row);
    ValidateRowWeight(weight);
    DataWeight_ += weight;

    auto partitionIndex = Partitioner_->GetPartitionIndex(row);
    auto& blockWriter = BlockWriters_[partitionIndex];

    CurrentBufferCapacity_ -= blockWriter->GetCapacity();
    i64 oldSize = blockWriter->GetBlockSize();

    blockWriter->WriteRow(row);

    CurrentBufferCapacity_ += blockWriter->GetCapacity();
    i64 newSize = blockWriter->GetBlockSize();

    auto* partitionAttributes = PartitionsExt_.mutable_partitions(partitionIndex);
    partitionAttributes->set_row_count(partitionAttributes->row_count() + 1);
    partitionAttributes->set_uncompressed_data_size(partitionAttributes->uncompressed_data_size() + newSize - oldSize);

    if (newSize > LargestPartitionSize_) {
        LargestPartitionIndex_ = partitionIndex;
        LargestPartitionSize_ = newSize;
    }

    if (LargestPartitionSize_ >= Config_->BlockSize || CurrentBufferCapacity_ >= Config_->MaxBufferSize) {
        CurrentBufferCapacity_ -= BlockWriters_[LargestPartitionIndex_]->GetCapacity();

        FlushBlock(LargestPartitionIndex_);
        BlockWriters_[LargestPartitionIndex_].reset(new THorizontalSchemalessBlockWriter(BlockReserveSize_));
        CurrentBufferCapacity_ += BlockWriters_[LargestPartitionIndex_]->GetCapacity();

        InitLargestPartition();
    }
}

void TPartitionChunkWriter::FlushBlock(int partitionIndex)
{
    auto& blockWriter = BlockWriters_[partitionIndex];
    auto block = blockWriter->FlushBlock();
    block.Meta.set_partition_index(partitionIndex);
    FlushedRowCount_ += block.Meta.row_count();
    block.Meta.set_chunk_row_count(FlushedRowCount_);

    RegisterBlock(block);
}

void TPartitionChunkWriter::InitLargestPartition()
{
    LargestPartitionIndex_ = 0;
    LargestPartitionSize_ = BlockWriters_.front()->GetBlockSize();
    for (int partitionIndex = 1; partitionIndex < BlockWriters_.size(); ++partitionIndex) {
        auto& blockWriter = BlockWriters_[partitionIndex];
        if (blockWriter->GetBlockSize() > LargestPartitionSize_) {
            LargestPartitionSize_ = blockWriter->GetBlockSize();
            LargestPartitionIndex_ = partitionIndex;
        }
    }
}

i64 TPartitionChunkWriter::GetDataSize() const
{
    return TChunkWriterBase::GetDataSize() + CurrentBufferCapacity_;
}

TChunkMeta TPartitionChunkWriter::GetSchedulerMeta() const
{
    auto meta = TChunkWriterBase::GetSchedulerMeta();
    SetProtoExtension(meta.mutable_extensions(), PartitionsExt_);
    return meta;
}

i64 TPartitionChunkWriter::GetMetaSize() const
{
    return TChunkWriterBase::GetMetaSize() + 2 * sizeof(i64) * BlockWriters_.size();
}

TNameTablePtr TPartitionChunkWriter::GetNameTable() const
{
    return NameTable_;
}

void TPartitionChunkWriter::DoClose()
{
    for (int partitionIndex = 0; partitionIndex < BlockWriters_.size(); ++partitionIndex) {
        if (BlockWriters_[partitionIndex]->GetRowCount() > 0) {
            FlushBlock(partitionIndex);
        }
    }

    TChunkWriterBase::DoClose();
}

void TPartitionChunkWriter::PrepareChunkMeta()
{
    TChunkWriterBase::PrepareChunkMeta();

    LOG_DEBUG("Partition totals: %v", PartitionsExt_.DebugString());

    auto& meta = EncodingChunkWriter_->Meta();

    SetProtoExtension(meta.mutable_extensions(), PartitionsExt_);

    TKeyColumnsExt keyColumnsExt;
    ToProto(keyColumnsExt.mutable_names(), KeyColumns_);
    SetProtoExtension(meta.mutable_extensions(), keyColumnsExt);

    TNameTableExt nameTableExt;
    ToProto(&nameTableExt, NameTable_);
    SetProtoExtension(meta.mutable_extensions(), nameTableExt);
}

ETableChunkFormat TPartitionChunkWriter::GetFormatVersion() const
{
    return ETableChunkFormat::SchemalessHorizontal;
}

bool TPartitionChunkWriter::IsSorted() const
{
    return false;
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessChunkWriterPtr CreatePartitionChunkWriter(
    TChunkWriterConfigPtr config,
    TChunkWriterOptionsPtr options,
    TNameTablePtr nameTable,
    const TKeyColumns& keyColumns,
    IChunkWriterPtr chunkWriter,
    IPartitioner* partitioner,
    IBlockCachePtr blockCache)
{
    return New<TPartitionChunkWriter>(
        config,
        options,
        nameTable,
        chunkWriter,
        blockCache,
        keyColumns,
        partitioner);
}

////////////////////////////////////////////////////////////////////////////////

struct TReorderingSchemalessWriterPoolTag { };

class TReorderingSchemalessMultiChunkWriter
    : public ISchemalessMultiChunkWriter
{
public:
    TReorderingSchemalessMultiChunkWriter(
        const TKeyColumns& keyColumns,
        TNameTablePtr nameTable,
        TOwningKey lastKey,
        ISchemalessMultiChunkWriterPtr underlyingWriter);

    virtual bool Write(const std::vector<TUnversionedRow>& rows) override;

    virtual TNameTablePtr GetNameTable() const override;

    virtual bool IsSorted() const override;

private:
    TChunkedMemoryPool MemoryPool_;
    TSchemalessRowReorderer RowReorderer_;
    ISchemalessMultiChunkWriterPtr UnderlyingWriter_;

    TOwningKey LastKey_;
    int KeyColumnCount_;
    TError Error_;


    bool CheckSortOrder(TUnversionedRow lhs, TUnversionedRow rhs);

    virtual TFuture<void> Open() override;
    virtual TFuture<void> GetReadyEvent() override;
    virtual TFuture<void> Close() override;

    virtual void SetProgress(double progress) override;
    virtual const std::vector<TChunkSpec>& GetWrittenChunksMasterMeta() const override;
    virtual const std::vector<TChunkSpec>& GetWrittenChunksFullMeta() const override;
    virtual TNodeDirectoryPtr GetNodeDirectory() const override;
    virtual TDataStatistics GetDataStatistics() const override;

};

////////////////////////////////////////////////////////////////////////////////

TReorderingSchemalessMultiChunkWriter::TReorderingSchemalessMultiChunkWriter(
    const TKeyColumns& keyColumns,
    TNameTablePtr nameTable,
    TOwningKey lastKey,
    ISchemalessMultiChunkWriterPtr underlyingWriter)
    : MemoryPool_(TReorderingSchemalessWriterPoolTag())
    , RowReorderer_(nameTable, keyColumns)
    , UnderlyingWriter_(underlyingWriter)
    , LastKey_(lastKey)
    , KeyColumnCount_(keyColumns.size())
{ }

bool TReorderingSchemalessMultiChunkWriter::CheckSortOrder(TUnversionedRow lhs, TUnversionedRow rhs)
{
    try {
        if (CompareRows(lhs, rhs, KeyColumnCount_) <= 0) {
            return true;
        }
        TUnversionedOwningRowBuilder leftBuilder, rightBuilder;
        for (int i = 0; i < KeyColumnCount_; ++i) {
            leftBuilder.AddValue(lhs[i]);
            rightBuilder.AddValue(rhs[i]);
        }

        Error_ = TError(
            EErrorCode::SortOrderViolation,
            "Sort order violation: %v > %v",
            leftBuilder.FinishRow().Get(),
            rightBuilder.FinishRow().Get());
    } catch (const std::exception& ex) {
        // NB: e.g. incomparable type.
        Error_ = TError(ex);
    }
    return false;
}

bool TReorderingSchemalessMultiChunkWriter::Write(const std::vector<TUnversionedRow>& rows)
{
    std::vector<TUnversionedRow> reorderedRows;
    reorderedRows.reserve(rows.size());

    for (const auto& row : rows) {
        reorderedRows.push_back(RowReorderer_.ReorderRow(row, &MemoryPool_));
    }

    if (IsSorted() && !reorderedRows.empty()) {
        if (!CheckSortOrder(LastKey_.Get(), reorderedRows.front())) {
            return false;
        }

        for (int i = 1; i < reorderedRows.size(); ++i) {
            if (!CheckSortOrder(reorderedRows[i-1], reorderedRows[i])) {
              return false;
            }
        }

        const auto& lastKey = reorderedRows.back();
        TUnversionedOwningRowBuilder keyBuilder;
        for (int i = 0; i < KeyColumnCount_; ++i) {
            keyBuilder.AddValue(lastKey[i]);
        }
        LastKey_ = keyBuilder.FinishRow();
    }

    auto result = UnderlyingWriter_->Write(reorderedRows);
    MemoryPool_.Clear();

    return result;
}

TFuture<void> TReorderingSchemalessMultiChunkWriter::Open()
{
    return UnderlyingWriter_->Open();
}

TFuture<void> TReorderingSchemalessMultiChunkWriter::GetReadyEvent()
{
    if (Error_.IsOK()) {
        return UnderlyingWriter_->GetReadyEvent();
    } else {
        return MakeFuture(Error_);
    }
}

TFuture<void> TReorderingSchemalessMultiChunkWriter::Close()
{
    return UnderlyingWriter_->Close();
}

void TReorderingSchemalessMultiChunkWriter::SetProgress(double progress)
{
    UnderlyingWriter_->SetProgress(progress);
}

const std::vector<TChunkSpec>& TReorderingSchemalessMultiChunkWriter::GetWrittenChunksMasterMeta() const
{
    return UnderlyingWriter_->GetWrittenChunksMasterMeta();
}

const std::vector<TChunkSpec>& TReorderingSchemalessMultiChunkWriter::GetWrittenChunksFullMeta() const
{
    return GetWrittenChunksMasterMeta();
}

TNodeDirectoryPtr TReorderingSchemalessMultiChunkWriter::GetNodeDirectory() const
{
    return UnderlyingWriter_->GetNodeDirectory();
}

TDataStatistics TReorderingSchemalessMultiChunkWriter::GetDataStatistics() const
{
    return UnderlyingWriter_->GetDataStatistics();
}

TNameTablePtr TReorderingSchemalessMultiChunkWriter::GetNameTable() const
{
    return UnderlyingWriter_->GetNameTable();
}

bool TReorderingSchemalessMultiChunkWriter::IsSorted() const
{
    return UnderlyingWriter_->IsSorted();
}

////////////////////////////////////////////////////////////////////////////////

template <class TBase>
class TSchemalessMultiChunkWriter
    : public TBase
{
public:
    TSchemalessMultiChunkWriter(
        TMultiChunkWriterConfigPtr config,
        TMultiChunkWriterOptionsPtr options,
        IClientPtr client,
        const TTransactionId& transactionId,
        const TChunkListId& parentChunkListId,
        std::function<ISchemalessChunkWriterPtr(IChunkWriterPtr)> createChunkWriter,
        TNameTablePtr nameTable,
        bool isSorted,
        IThroughputThrottlerPtr throttler,
        IBlockCachePtr blockCache)
        : TBase(
            config,
            options,
            client,
            transactionId,
            parentChunkListId,
            createChunkWriter,
            throttler,
            blockCache)
        , NameTable_(nameTable)
        , IsSorted_(isSorted)
    { }

    virtual TNameTablePtr GetNameTable() const override
    {
        return NameTable_;
    }

    virtual bool IsSorted() const override
    {
        return IsSorted_;
    }

private:
    const TNameTablePtr NameTable_;
    const bool IsSorted_;

};

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkWriterPtr CreateSchemalessMultiChunkWriter(
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    TNameTablePtr nameTable,
    const TKeyColumns& keyColumns,
    TOwningKey lastKey,
    IClientPtr client,
    const TTransactionId& transactionId,
    const TChunkListId& parentChunkListId,
    bool reorderValues,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
{
    typedef TMultiChunkWriterBase<
        ISchemalessMultiChunkWriter,
        ISchemalessChunkWriter,
        const std::vector<TUnversionedRow>&> TSchemalessMultiChunkWriterBase;
    typedef TSchemalessMultiChunkWriter<TSchemalessMultiChunkWriterBase> TWriter;

    auto createChunkWriter = [=] (IChunkWriterPtr underlyingWriter) {
        return CreateSchemalessChunkWriter(
            config,
            options,
            nameTable,
            keyColumns,
            underlyingWriter,
            blockCache);
    };

    bool isSorted = !keyColumns.empty();
    auto writer = New<TWriter>(
        config,
        options,
        client,
        transactionId,
        parentChunkListId,
        createChunkWriter,
        nameTable,
        isSorted,
        throttler,
        blockCache);

    if (reorderValues && isSorted) {
        return New<TReorderingSchemalessMultiChunkWriter>(
            keyColumns,
            nameTable,
            lastKey,
            writer);
    } else {
        return writer;
    }
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessMultiChunkWriterPtr CreatePartitionMultiChunkWriter(
    TTableWriterConfigPtr config,
    TTableWriterOptionsPtr options,
    TNameTablePtr nameTable,
    const TKeyColumns& keyColumns,
    IClientPtr client,
    const TTransactionId& transactionId,
    const TChunkListId& parentChunkListId,
    std::unique_ptr<IPartitioner> partitioner,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
{
    YCHECK(!keyColumns.empty());

    typedef TMultiChunkWriterBase<
        ISchemalessMultiChunkWriter,
        ISchemalessChunkWriter,
        const std::vector<TUnversionedRow>&> TPartitionMultiChunkWriterBase;

    typedef TSchemalessMultiChunkWriter<TPartitionMultiChunkWriterBase> TWriter;

    // TODO(babenko): consider making IPartitioner ref-counted.
    auto createChunkWriter = [=, partitioner = std::shared_ptr<IPartitioner>(std::move(partitioner))] (IChunkWriterPtr underlyingWriter) {
        return CreatePartitionChunkWriter(
            config,
            options,
            nameTable,
            keyColumns,
            underlyingWriter,
            partitioner.get(),
            blockCache);
    };

    auto writer = New<TWriter>(
        config,
        options,
        client,
        transactionId,
        parentChunkListId,
        createChunkWriter,
        nameTable,
        false,
        throttler,
        blockCache);

    return New<TReorderingSchemalessMultiChunkWriter>(
        keyColumns,
        nameTable,
        TOwningKey(),
        writer);
}

////////////////////////////////////////////////////////////////////////////////

class TSchemalessTableWriter
    : public ISchemalessWriter
    , public TTransactionListener
{
public:
    TSchemalessTableWriter(
        TTableWriterConfigPtr config,
        TRemoteWriterOptionsPtr options,
        const TRichYPath& richPath,
        TNameTablePtr nameTable,
        const TKeyColumns& keyColumns,
        IClientPtr client,
        TTransactionPtr transaction,
        IThroughputThrottlerPtr throttler,
        IBlockCachePtr blockCache);

    virtual TFuture<void> Open() override;
    virtual bool Write(const std::vector<TUnversionedRow>& rows) override;
    virtual TFuture<void> GetReadyEvent() override;
    virtual TFuture<void> Close() override;
    virtual TNameTablePtr GetNameTable() const override;
    virtual bool IsSorted() const override;

private:
    NLogging::TLogger Logger;

    const TTableWriterConfigPtr Config_;
    const TTableWriterOptionsPtr Options_;
    const TRichYPath RichPath_;
    const TNameTablePtr NameTable_;
    const TKeyColumns KeyColumns_;
    const IClientPtr Client_;
    const TTransactionPtr Transaction_;
    const TTransactionManagerPtr TransactionManager_;
    const IThroughputThrottlerPtr Throttler_;
    const IBlockCachePtr BlockCache_;

    TTransactionId TransactionId_;

    TTransactionPtr UploadTransaction_;
    TChunkListId ChunkListId_;

    TOwningKey LastKey_;

    ISchemalessWriterPtr UnderlyingWriter_;


    void DoOpen();
    void FetchTableInfo();
    void CreateUploadTransaction();

    void DoClose();

};

////////////////////////////////////////////////////////////////////////////////

TSchemalessTableWriter::TSchemalessTableWriter(
    TTableWriterConfigPtr config,
    TRemoteWriterOptionsPtr options,
    const TRichYPath& richPath,
    TNameTablePtr nameTable,
    const TKeyColumns& keyColumns,
    IClientPtr client,
    TTransactionPtr transaction,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
    : Logger(TableClientLogger)
    , Config_(config)
    , Options_(New<TTableWriterOptions>())
    , RichPath_(richPath)
    , NameTable_(nameTable)
    , KeyColumns_(keyColumns)
    , Client_(client)
    , Transaction_(transaction)
    , Throttler_(throttler)
    , BlockCache_(blockCache)
    , TransactionId_(transaction ? transaction->GetId() : NullTransactionId)
{
    Logger.AddTag("Path: %v, TransactionId: %v",
        RichPath_.GetPath(),
        TransactionId_);
}

TFuture<void> TSchemalessTableWriter::Open()
{
    LOG_INFO("Opening table writer");

    return BIND(&TSchemalessTableWriter::DoOpen, MakeStrong(this))
        .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
        .Run();
}

bool TSchemalessTableWriter::Write(const std::vector<TUnversionedRow>& rows)
{
    YCHECK(UnderlyingWriter_);
    if (IsAborted()) {
        return false;
    }

    return UnderlyingWriter_->Write(rows);
}

TFuture<void> TSchemalessTableWriter::GetReadyEvent()
{
    if (IsAborted()) {
        return MakeFuture(TError("Transaction %v aborted",
            TransactionId_));
    }

    return UnderlyingWriter_->GetReadyEvent();
}

TFuture<void> TSchemalessTableWriter::Close()
{
    return BIND(&TSchemalessTableWriter::DoClose, MakeStrong(this))
        .AsyncVia(NChunkClient::TDispatcher::Get()->GetWriterInvoker())
        .Run();
}

void TSchemalessTableWriter::CreateUploadTransaction()
{
    LOG_INFO("Creating upload transaction");

    NTransactionClient::TTransactionStartOptions options;
    options.ParentId = TransactionId_;
    options.EnableUncommittedAccounting = false;

    auto attributes = CreateEphemeralAttributes();
    attributes->Set("title", Format("Table upload to %v", RichPath_.GetPath()));
    options.Attributes = std::move(attributes);

    auto transactionOrError = WaitFor(Client_->GetTransactionManager()->Start(
        ETransactionType::Master,
        options));

    THROW_ERROR_EXCEPTION_IF_FAILED(
        transactionOrError,
        "Error creating upload transaction");

    UploadTransaction_ = transactionOrError.Value();
    ListenTransaction(UploadTransaction_);

    LOG_INFO("Upload transaction created (TransactionId: %v)",
        UploadTransaction_->GetId());
}

void TSchemalessTableWriter::FetchTableInfo()
{
    LOG_INFO("Requesting table info");

    auto path = RichPath_.GetPath();
    bool append = RichPath_.GetAppend();
    bool sorted = !KeyColumns_.empty();

    auto channel = Client_->GetMasterChannel(EMasterChannelKind::Leader);
    TObjectServiceProxy objectProxy(channel);
    auto batchReq = objectProxy.ExecuteBatch();

    {
        auto req = TCypressYPathProxy::Get(path);
        SetTransactionId(req, UploadTransaction_);
        TAttributeFilter attributeFilter(EAttributeFilterMode::MatchingOnly);
        attributeFilter.Keys.push_back("type");
        attributeFilter.Keys.push_back("replication_factor");
        attributeFilter.Keys.push_back("compression_codec");
        attributeFilter.Keys.push_back("erasure_codec");
        attributeFilter.Keys.push_back("account");
        attributeFilter.Keys.push_back("vital");

        if (sorted) {
            attributeFilter.Keys.push_back("row_count");
            attributeFilter.Keys.push_back("sorted_by");
        }

        ToProto(req->mutable_attribute_filter(), attributeFilter);
        batchReq->AddRequest(req, "get_attributes");
    }

    {
        auto req = TTableYPathProxy::PrepareForUpdate(path);
        SetTransactionId(req, UploadTransaction_);
        GenerateMutationId(req);
        req->set_update_mode(static_cast<int>(append ? EUpdateMode::Append : EUpdateMode::Overwrite));
        req->set_lock_mode(static_cast<int>((append && !sorted) ? ELockMode::Shared : ELockMode::Exclusive));
        if (append && sorted) {
            req->set_fetch_last_key(true);
        }
        batchReq->AddRequest(req, "prepare_for_update");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        GetCumulativeError(batchRspOrError),
        "Error requesting table info for %v",
        path);
    const auto& batchRsp = batchRspOrError.Value();

    {
        auto rspOrError = batchRsp->GetResponse<TYPathProxy::TRspGet>("get_attributes");
        auto node = ConvertToNode(TYsonString(rspOrError.Value()->value()));
        const auto& attributes = node->Attributes();

        auto type = attributes.Get<EObjectType>("type");
        if (type != EObjectType::Table) {
            THROW_ERROR_EXCEPTION(
                "Invalid type of %v: expected %Qlv, actual %Qlv",
                path,
                EObjectType::Table,
                type);
        }

        if (append && sorted && attributes.Get<i64>("row_count") > 0) {
            auto tableKeyColumns = attributes.Get<TKeyColumns>("sorted_by", TKeyColumns());

            bool areKeyColumnsCompatible = true;
            if (tableKeyColumns.size() < KeyColumns_.size()) {
                areKeyColumnsCompatible = false;
            } else {
                for (int i = 0; i < KeyColumns_.size(); ++i) {
                    if (tableKeyColumns[i] != KeyColumns_[i]) {
                        areKeyColumnsCompatible = false;
                        break;
                    }
                }
            }

            if (!areKeyColumnsCompatible) {
                THROW_ERROR_EXCEPTION(
                    "Key columns mismatch while trying to append sorted data into a non-empty table %v", path)
                    << TErrorAttribute("append_key_columns", KeyColumns_)
                    << TErrorAttribute("current_key_columns", tableKeyColumns);
            }
        }

        Options_->ReplicationFactor = attributes.Get<int>("replication_factor");
        Options_->CompressionCodec = attributes.Get<NCompression::ECodec>("compression_codec");
        Options_->ErasureCodec = attributes.Get<NErasure::ECodec>("erasure_codec");
        Options_->Account = attributes.Get<Stroka>("account");
        Options_->ChunksVital = attributes.Get<bool>("vital");
    }

    {
        auto rspOrError = batchRsp->GetResponse<TTableYPathProxy::TRspPrepareForUpdate>("prepare_for_update");
        ChunkListId_ = FromProto<TChunkListId>(rspOrError.Value()->chunk_list_id());

        if (append && sorted) {
            auto lastKey = FromProto<TOwningKey>(rspOrError.Value()->last_key());
            if (lastKey) {
                YCHECK(lastKey.GetCount() >= KeyColumns_.size());
                LastKey_ = TOwningKey(lastKey.Begin(), lastKey.Begin() + KeyColumns_.size());
            }
        }
    }

    LOG_INFO("Table info received (ChunkListId: %v)",
        ChunkListId_);
}

void TSchemalessTableWriter::DoOpen()
{
    CreateUploadTransaction();
    FetchTableInfo();

    UnderlyingWriter_ = CreateSchemalessMultiChunkWriter(
        Config_,
        Options_,
        NameTable_,
        KeyColumns_,
        LastKey_,
        Client_,
        UploadTransaction_->GetId(),
        ChunkListId_,
        true,
        Throttler_,
        BlockCache_);

    auto error = WaitFor(UnderlyingWriter_->Open());
    THROW_ERROR_EXCEPTION_IF_FAILED(
        error,
        "Error opening table chunk writer");

    if (Transaction_) {
        ListenTransaction(Transaction_);
    }
}

void TSchemalessTableWriter::DoClose()
{
    const auto& path = RichPath_.GetPath();

    LOG_INFO("Closing table writer");
    {
        auto error = WaitFor(UnderlyingWriter_->Close());
        THROW_ERROR_EXCEPTION_IF_FAILED(error, "Error closing chunk writer");
    }
    LOG_INFO("Chunk writer closed");

    if (!KeyColumns_.empty()) {
        LOG_INFO("Marking table as sorted by %v",
            ConvertToYsonString(KeyColumns_, NYson::EYsonFormat::Text).Data());

        auto req = TTableYPathProxy::SetSorted(path);
        SetTransactionId(req, UploadTransaction_);
        GenerateMutationId(req);
        ToProto(req->mutable_key_columns(), KeyColumns_);

        auto channel = Client_->GetMasterChannel(EMasterChannelKind::Leader);
        TObjectServiceProxy objectProxy(channel);
        auto rspOrError = WaitFor(objectProxy.Execute(req));

        THROW_ERROR_EXCEPTION_IF_FAILED(
            rspOrError,
            "Error marking table %v as sorted",
            path);

        LOG_INFO("Table is marked as sorted");
    }

    LOG_INFO("Committing upload transaction");
    {
        auto error = WaitFor(UploadTransaction_->Commit());
        THROW_ERROR_EXCEPTION_IF_FAILED(
            error,
            "Error committing upload transaction",
            RichPath_.GetPath());
    }
    LOG_INFO("Upload transaction committed");

    LOG_INFO("Table writer closed");
}

TNameTablePtr TSchemalessTableWriter::GetNameTable() const
{
    return NameTable_;
}

bool TSchemalessTableWriter::IsSorted() const
{
    return UnderlyingWriter_->IsSorted();
}

////////////////////////////////////////////////////////////////////////////////

ISchemalessWriterPtr CreateSchemalessTableWriter(
    TTableWriterConfigPtr config,
    TRemoteWriterOptionsPtr options,
    const TRichYPath& richPath,
    TNameTablePtr nameTable,
    const TKeyColumns& keyColumns,
    IClientPtr client,
    TTransactionPtr transaction,
    IThroughputThrottlerPtr throttler,
    IBlockCachePtr blockCache)
{
    return New<TSchemalessTableWriter>(
        config,
        options,
        richPath,
        nameTable,
        keyColumns,
        client,
        transaction,
        throttler,
        blockCache);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
