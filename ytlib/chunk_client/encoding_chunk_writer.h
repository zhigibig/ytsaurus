#pragma once

#include "public.h"
#include "chunk_meta_extensions.h"
#include "data_statistics.h"

#include <yt/core/logging/log.h>

#include <yt/core/misc/error.h>
#include <yt/core/misc/property.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TEncodingChunkWriter
    : public virtual TRefCounted
{
public:
    DEFINE_BYREF_RW_PROPERTY(NProto::TChunkMeta, Meta);
    DEFINE_BYREF_RW_PROPERTY(NProto::TMiscExt, MiscExt);

public:
    TEncodingChunkWriter(
        TEncodingWriterConfigPtr config,
        TEncodingWriterOptionsPtr options,
        IChunkWriterPtr chunkWriter,
        IBlockCachePtr blockCache,
        NLogging::TLogger& logger);

    void WriteBlock(std::vector<TSharedRef> vectorizedBlock);
    void WriteBlock(TSharedRef block);

    void Close();

    TFuture<void> GetReadyEvent() const;
    bool IsReady() const;

    double GetCompressionRatio() const;

    NProto::TDataStatistics GetDataStatistics() const;

private:
    const IChunkWriterPtr ChunkWriter_;
    const TEncodingWriterPtr EncodingWriter_;

    int CurrentBlockIndex_ = 0;
    i64 LargestBlockSize_ = 0;

    bool Closed_ = false;

};

DEFINE_REFCOUNTED_TYPE(TEncodingChunkWriter)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
