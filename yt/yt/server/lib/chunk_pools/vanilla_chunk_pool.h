#pragma once

#include "chunk_pool.h"

namespace NYT::NChunkPools {

////////////////////////////////////////////////////////////////////////////////

struct TVanillaChunkPoolOptions
{
    int JobCount;
    bool RestartCompletedJobs = false;
};

////////////////////////////////////////////////////////////////////////////////

// NB: Vanilla chunk pool implements only IChunkPoolOutput.
IChunkPoolOutputPtr CreateVanillaChunkPool(const TVanillaChunkPoolOptions& options);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkPools
