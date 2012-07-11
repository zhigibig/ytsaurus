﻿#include "stdafx.h"

#include "partition_chunk_writer.h"
#include "private.h"
#include "config.h"
#include "channel_writer.h"
#include "chunk_meta_extensions.h"

#include <ytlib/ytree/lexer.h>
#include <ytlib/chunk_client/async_writer.h>
#include <ytlib/chunk_client/private.h>
#include <ytlib/chunk_client/encoding_writer.h>
#include <ytlib/chunk_holder/chunk_meta_extensions.h>

namespace NYT {
namespace NTableClient {

using namespace NChunkServer;
using namespace NChunkClient;
using namespace NYTree;

static NLog::TLogger& Logger = TableWriterLogger;

////////////////////////////////////////////////////////////////////////////////

bool operator < (const TOwningKey& partitionKey, const TNonOwningKey& key)
{
    return CompareKeys(partitionKey, key) < 0;
}

////////////////////////////////////////////////////////////////////////////////

TPartitionChunkWriter::TPartitionChunkWriter(
    TChunkWriterConfigPtr config,
    NChunkClient::IAsyncWriterPtr chunkWriter,
    const std::vector<TChannel>& channels,
    const TKeyColumns& keyColumns,
    const std::vector<NProto::TKey>& partitionKeys)
    : TChunkWriterBase(chunkWriter, config)
    , Channel(TChannel::CreateUniversal())
    , KeyColumns(keyColumns.size())
    , CurrentSize(0)
    , BasicMetaSize(0)
{
    *ChannelsExt.add_items()->mutable_channel() = Channel.ToProto();

    for (int i = 0; i < KeyColumns.size(); ++i) {
        KeyColumnIndexes[KeyColumns[i]] = i;
    }

    PartitionKeys.reserve(partitionKeys.size());
    FOREACH (const auto& key, partitionKeys) {
        PartitionKeys.push_back(TOwningKey::FromProto(key));
    }

    for (int partitionTag = 0; partitionTag <= PartitionKeys.size(); ++partitionTag) {
        ChannelWriters.push_back(New<TChannelWriter>(0));
        auto* partitionAttributes = PartitionsExt.add_partitions();
        partitionAttributes->set_data_weight(0);
        partitionAttributes->set_row_count(0);
    }

    BasicMetaSize = ChannelsExt.ByteSize() + sizeof(i64) * PartitionKeys.size() + 
        sizeof(NChunkHolder::NProto::TMiscExt) + 
        sizeof(NChunkHolder::NProto::TChunkMeta);
}

TPartitionChunkWriter::~TPartitionChunkWriter()
{ }

bool TPartitionChunkWriter::TryWriteRow(const TRow& row)
{
    YASSERT(!State.IsClosed());

    if (!State.IsActive() || !EncodingWriter->IsReady())
        return false;

    TNonOwningKey key(KeyColumns.size());
    {
        FOREACH (const auto& pair, row) {
            auto it = KeyColumnIndexes.find(pair.first);
            if (it != KeyColumnIndexes.end()) {
                key.SetKeyPart(it->second, pair.second, Lexer);
            }
        }
    }

    auto partitionIt = std::upper_bound(PartitionKeys.begin(), PartitionKeys.end(), key);
    auto partitionTag = std::distance(PartitionKeys.begin(), partitionIt);
    auto& channelWriter = ChannelWriters[partitionTag];

    i64 rowDataWeight = 1;
    FOREACH (const auto& pair, row) {
        channelWriter->WriteRange(pair.first, pair.second);

        rowDataWeight += pair.first.size();
        rowDataWeight += pair.second.size();
        ValueCount += 1;
    }
    channelWriter->EndRow();

    // Update partition counters.
    auto* partitionAttributes = PartitionsExt.mutable_partitions(partitionTag);
    partitionAttributes->set_data_weight(partitionAttributes->data_weight() + rowDataWeight);
    partitionAttributes->set_row_count(partitionAttributes->row_count() + 1);

    // Update global counters.
    DataWeight += rowDataWeight;
    RowCount += 1;

    if (channelWriter->GetCurrentSize() > static_cast<size_t>(Config->BlockSize)) {
        PrepareBlock(partitionTag);
    }

    CurrentSize = EncodingWriter->GetCompressedSize() + channelWriter->GetCurrentSize();
    return true;
}

void TPartitionChunkWriter::PrepareBlock(int partitionTag)
{
    auto& channelWriter = ChannelWriters[partitionTag];
    auto* blockInfo = ChannelsExt.mutable_items(0)->add_blocks();
    blockInfo->set_row_count(channelWriter->GetCurrentRowCount());
    blockInfo->set_partition_tag(partitionTag);
    blockInfo->set_block_index(CurrentBlockIndex);

    ++CurrentBlockIndex;

    EncodingWriter->WriteBlock(channelWriter->FlushBlock());
}

i64 TPartitionChunkWriter::GetMetaSize() const
{
    return BasicMetaSize + CurrentBlockIndex * sizeof(NProto::TBlockInfo);
}

i64 TPartitionChunkWriter::GetCurrentSize() const
{
    return CurrentSize;
}

NChunkHolder::NProto::TChunkMeta TPartitionChunkWriter::GetMasterMeta() const
{
    NChunkHolder::NProto::TChunkMeta meta;
    meta.set_type(EChunkType::Table);
    SetProtoExtension(meta.mutable_extensions(), MiscExt);

    return meta;
}

NChunkHolder::NProto::TChunkMeta TPartitionChunkWriter::GetSchedulerMeta() const
{
    NChunkHolder::NProto::TChunkMeta meta;
    meta.set_type(EChunkType::Table);
    SetProtoExtension(meta.mutable_extensions(), MiscExt);
    SetProtoExtension(meta.mutable_extensions(), PartitionsExt);

    return meta;
}

TAsyncError TPartitionChunkWriter::AsyncClose()
{
    YASSERT(!State.IsClosed());

    State.StartOperation();

    for (int partitionTag = 0; partitionTag <= PartitionKeys.size(); ++partitionTag) {
        auto& channelWriter = ChannelWriters[partitionTag];
        if (channelWriter->GetCurrentRowCount() > 0) {
            PrepareBlock(partitionTag);
        }
    }

    EncodingWriter->AsyncFlush().Subscribe(BIND(
        &TPartitionChunkWriter::OnFinalBlocksWritten,
        MakeWeak(this)).Via(WriterThread->GetInvoker()));

    return State.GetOperationError();
}

void TPartitionChunkWriter::OnFinalBlocksWritten(TError error)
{
    if (!error.IsOK()) {
        State.FinishOperation(error);
        return;
    }

    SetProtoExtension(Meta.mutable_extensions(), PartitionsExt);
    FinalizeWriter();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
