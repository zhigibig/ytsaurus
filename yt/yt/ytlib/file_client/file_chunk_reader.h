#pragma once

#include "public.h"

#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/ytlib/chunk_client/block.h>
#include <yt/yt/ytlib/chunk_client/data_source.h>

#include <yt/yt/client/chunk_client/reader_base.h>

#include <yt/yt/ytlib/node_tracker_client/public.h>

#include <yt/yt/core/compression/public.h>

#include <yt/yt/core/concurrency/throughput_throttler.h>

#include <yt/yt/core/misc/ref.h>

#include <yt/yt/core/rpc/public.h>

namespace NYT::NFileClient {

////////////////////////////////////////////////////////////////////////////////

struct IFileReader
    : public virtual NChunkClient::IReaderBase
{
    virtual bool ReadBlock(NChunkClient::TBlock* block) = 0;
};

DEFINE_REFCOUNTED_TYPE(IFileReader)

////////////////////////////////////////////////////////////////////////////////

NConcurrency::IAsyncZeroCopyInputStreamPtr CreateFileReaderAdapter(IFileReaderPtr underlying);

////////////////////////////////////////////////////////////////////////////////

IFileReaderPtr CreateFileChunkReader(
    NChunkClient::TBlockFetcherConfigPtr config,
    NChunkClient::IChunkReaderPtr chunkReader,
    NChunkClient::IBlockCachePtr blockCache,
    NCompression::ECodec codecId,
    const NChunkClient::TClientChunkReadOptions& chunkReadOptions,
    i64 startOffset,
    i64 endOffset,
    const NChunkClient::TDataSource& dataSource,
    NChunkClient::TChunkReaderMemoryManagerPtr chunkReaderMemoryManager = nullptr);

////////////////////////////////////////////////////////////////////////////////

IFileReaderPtr CreateFileMultiChunkReader(
    NChunkClient::TMultiChunkReaderConfigPtr config,
    NChunkClient::TMultiChunkReaderOptionsPtr options,
    NApi::NNative::IClientPtr client,
    const NNodeTrackerClient::TNodeDescriptor& localDescriptor,
    NChunkClient::IBlockCachePtr blockCache,
    NChunkClient::IClientChunkMetaCachePtr chunkMetaCache,
    const NChunkClient::TClientChunkReadOptions& chunkReadOptions,
    const std::vector<NChunkClient::NProto::TChunkSpec>& chunkSpecs,
    const NChunkClient::TDataSource& dataSource,
    NChunkClient::TTrafficMeterPtr trafficMeter = nullptr,
    NConcurrency::IThroughputThrottlerPtr bandwidthThrottler = NConcurrency::GetUnlimitedThrottler(),
    NConcurrency::IThroughputThrottlerPtr rpsThrottler = NConcurrency::GetUnlimitedThrottler(),
    NChunkClient::IMultiReaderMemoryManagerPtr multiReaderMemoryManager = nullptr);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NFileClient
