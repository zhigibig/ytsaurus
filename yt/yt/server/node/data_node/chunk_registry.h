#pragma once

#include "public.h"

#include <yt/yt/server/node/cluster_node/public.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

//! A facade for locating chunks.
/*!
 *  Chunks stored at node can be registered either in TChunkStore or in TChunkCache.
 *  This class provides a single entry point for locating these chunks.
 *
 *  \note
 *  Thread affinity: any
 */
struct IChunkRegistry
    : public TRefCounted
{
    //! Finds chunk by id. Returns |nullptr| if no chunk exists.
    virtual IChunkPtr FindChunk(
        TChunkId chunkId,
        int mediumIndex = NChunkClient::AllMediaIndex) = 0;

    //! Finds chunk by id. Throws if no chunk exists.
    virtual IChunkPtr GetChunkOrThrow(
        TChunkId chunkId,
        int mediumIndex = NChunkClient::AllMediaIndex) = 0;

    //! Schedules calling #IChunk::TrySweepReader after a configured period of time
    //! (see #TDataNodeDynamicConfig::ChunkReaderRetentionTimeout).
    virtual void ScheduleChunkReaderSweep(IChunkPtr chunk) = 0;
};

DEFINE_REFCOUNTED_TYPE(IChunkRegistry)

IChunkRegistryPtr CreateChunkRegistry(NClusterNode::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode

