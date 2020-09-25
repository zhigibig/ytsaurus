#pragma once

#include "private.h"
#include "chunk_pool.h"
#include "input_stream.h"

namespace NYT::NChunkPools {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EUnorderedChunkPoolMode,
    (Normal)
    (AutoMerge)
);

struct TUnorderedChunkPoolOptions
{
    EUnorderedChunkPoolMode Mode = EUnorderedChunkPoolMode::Normal;
    TJobSizeAdjusterConfigPtr JobSizeAdjusterConfig = nullptr;
    NControllerAgent::IJobSizeConstraintsPtr JobSizeConstraints = nullptr;
    //! Minimum uncompressed size to be teleported.
    i64 MinTeleportChunkSize = std::numeric_limits<i64>::max() / 4;
    //! Minimum data weight to be teleported.
    i64 MinTeleportChunkDataWeight = std::numeric_limits<i64>::max() / 4;
    bool SliceErasureChunksByParts = false;
    // TODO(max42): YT-13335.
    NTableClient::TRowBufferPtr RowBuffer;
    NScheduler::TOperationId OperationId;
    TString Name;

    void Persist(const TPersistenceContext& context);
};

IChunkPoolPtr CreateUnorderedChunkPool(
    const TUnorderedChunkPoolOptions& options,
    TInputStreamDirectory dataSourceDirectory);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkPools
