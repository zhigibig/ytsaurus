#pragma once

#include "public.h"

#include "chunk_reader_allowing_repair.h"

#include <yt/library/erasure/impl/codec.h>

#include <yt/core/actions/callback.h>

#include <yt/core/logging/log.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

TFuture<void> RepairErasedParts(
    NErasure::ICodec* codec,
    const NErasure::TPartIndexList& erasedIndices,
    const std::vector<IChunkReaderPtr>& readers,
    const std::vector<IChunkWriterPtr>& writers,
    const TClientBlockReadOptions& options);

TFuture<void> RepairErasedParts(
    NErasure::ICodec* codec,
    const NErasure::TPartIndexList& erasedIndices,
    const std::vector<IChunkReaderAllowingRepairPtr>& readers,
    const std::vector<IChunkWriterPtr>& writers,
    const TClientBlockReadOptions& options);

IChunkReaderPtr CreateRepairingErasureReader(
    TChunkId chunkId,
    NErasure::ICodec* codec,
    const NErasure::TPartIndexList& erasedIndices,
    // This list must consist of readers for all data parts and repair parts sorted by part index.
    const std::vector<IChunkReaderAllowingRepairPtr>& readers,
    const NLogging::TLogger& logger = {});

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
