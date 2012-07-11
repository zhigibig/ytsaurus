#pragma once

#include "public.h"
#include "chunk_writer_base.h"
#include "channel_writer.h"

#include "schema.h"
#include "key.h"

#include <ytlib/table_client/table_chunk_meta.pb.h>
#include <ytlib/chunk_holder/chunk.pb.h>

#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_server/chunk_ypath_proxy.h>
#include <ytlib/misc/codec.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/blob_output.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class  TTableChunkWriter
    : public TChunkWriterBase
{
public:
    TTableChunkWriter(
        TChunkWriterConfigPtr config,
        NChunkClient::IAsyncWriterPtr chunkWriter,
        const std::vector<TChannel>& channels,
        const TNullable<TKeyColumns>& keyColumns);

    ~TTableChunkWriter();

    TAsyncError AsyncOpen();

    // Used by client facade (e.g. TableConsumer).
    bool TryWriteRow(const TRow& row);

    // Used internally by jobs that generate sorted output.
    bool TryWriteRowUnsafe(const TRow& row, const TNonOwningKey& key);
    bool TryWriteRowUnsafe(const TRow& row);

    TAsyncError AsyncClose();

    void SetLastKey(const TOwningKey& key);
    const TOwningKey& GetLastKey() const;
    i64 GetRowCount() const;

    i64 GetCurrentSize() const;
    NChunkHolder::NProto::TChunkMeta GetMasterMeta() const;
    NChunkHolder::NProto::TChunkMeta GetSchedulerMeta() const;

    i64 GetMetaSize() const;

private:
    struct TChannelColumn {
        int ColumnIndex;
        TChannelWriterPtr Writer;

        TChannelColumn(const TChannelWriterPtr& channelWriter, int columnIndex) 
            : ColumnIndex(columnIndex)
            , Writer(channelWriter)
        {}
    };

    struct TColumnInfo {
        i64 LastRow;
        int KeyColumnIndex;
        std::vector<TChannelColumn> Channels;

        TColumnInfo()
            : LastRow(-1)
            , KeyColumnIndex(-1)
        { }
    };

    std::vector<TChannel> Channels;
    //! If not null chunk is expected to be sorted.
    TNullable<TKeyColumns> KeyColumns;

    bool IsOpen;

    //! Stores mapping from all key columns and channel non-range columns to indexes.
    yhash_map<TStringBuf, TColumnInfo> ColumnMap;
    std::vector<Stroka> ColumnNames;

    // Used for key creation.
    NYTree::TLexer Lexer;

    TNonOwningKey CurrentKey;
    TOwningKey LastKey;

    //! Approximate size of collected samples.
    i64 SamplesSize;

    //! Approximate size of collected index.
    i64 IndexSize;

    std::vector<TChannelWriterPtr> ChannelWriters;

    i64 BasicMetaSize;

    NProto::TSamplesExt SamplesExt;
    //! Only for sorted tables.
    NProto::TBoundaryKeysExt BoundaryKeysExt;
    NProto::TIndexExt IndexExt;

    void PrepareBlock(int channelIndex);

    void OnFinalBlocksWritten(TError error);

    void EmitIndexEntry();
    void EmitSample(const TRow& row);

    void SelectChannels(const TStringBuf& name, TColumnInfo& columnInfo);
    void FinalizeRow(const TRow& row);
    void ProcessKey();
    TColumnInfo& GetColumnInfo(const TStringBuf& name);
    void WriteValue(const std::pair<TStringBuf, TStringBuf>& value, const TColumnInfo& columnInfo);


    DECLARE_THREAD_AFFINITY_SLOT(ClientThread);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
