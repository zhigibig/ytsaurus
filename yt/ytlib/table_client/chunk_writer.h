#pragma once

#include "public.h"
#include "async_writer.h"

#include "schema.h"
#include "key.h"

#include <ytlib/table_client/table_chunk_meta.pb.h>
#include <ytlib/chunk_holder/chunk.pb.h>

#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_server/chunk_ypath_proxy.h>
#include <ytlib/misc/codec.h>
#include <ytlib/misc/thread_affinity.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class  TChunkWriter
    : public IAsyncWriter
{
public:
    TChunkWriter(
        TChunkWriterConfigPtr config,
        NChunkClient::IAsyncWriterPtr chunkWriter,
        const std::vector<TChannel>& channels,
        const TNullable<TKeyColumns>& keyColumns);

    ~TChunkWriter();

    TAsyncError AsyncOpen();

    TAsyncError AsyncWriteRow(TRow& row, TKey& key);

    TAsyncError AsyncClose();

    TKey& GetLastKey();
    const TNullable<TKeyColumns>& GetKeyColumns() const;
    i64 GetRowCount() const;

    i64 GetCurrentSize() const;
    NChunkHolder::NProto::TChunkMeta GetMasterMeta() const;

private:
    friend class TChunkSequenceWriter;

    TSharedRef PrepareBlock(int channelIndex);
    NProto::TSample MakeSample(TRow& row);

private:
    TChunkWriterConfigPtr Config;
    std::vector<TChannel> Channels;

    ICodec* Codec;

    NChunkClient::IAsyncWriterPtr ChunkWriter;

    //! If not null chunk is expected to be sorted.
    TNullable<TKeyColumns> KeyColumns;

    std::vector<TChannelWriterPtr> ChannelWriters;

    bool IsOpen;
    bool IsClosed;

    //! Stores mapping from all key columns and channel non-range columns to indexes.
    yhash_map<TStringBuf, int> ColumnIndexes;

    int CurrentBlockIndex;

    //! Total size of completed and sent blocks.
    i64 SentSize;

    //! Current size of written data.
    /*!
     *  1. This counter is updated every #AsyncEndRow call.
     *  2. This is an upper bound approximation of the size of written data, because we take 
     *  into account real size of complete blocks and uncompressed size of the incomplete blocks.
     */
    i64 CurrentSize;

    //! Uncompressed size of completed blocks.
    i64 UncompressedSize;

    //! Approximate size of written data, monotonically increases.
    i64 DataOffset;

    TKey LastKey;

    // Different chunk meta extensions.
    NChunkHolder::NProto::TMiscExt MiscExt;
    NProto::TSamplesExt SamplesExt;

    //! Approximate size of collected samples.
    size_t SamplesSize;

    NProto::TChannelsExt ChannelsExt;

    // These are used only for sorted.
    NProto::TBoundaryKeysExt BoundaryKeysExt;

    NProto::TIndexExt IndexExt;
    //! Approximate size of collected index.
    size_t IndexSize;

    DECLARE_THREAD_AFFINITY_SLOT(ClientThread);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
