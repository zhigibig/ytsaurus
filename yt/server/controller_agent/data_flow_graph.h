#pragma once

#include "private.h"
#include "serialize.h"
#include "progress_counter.h"

#include <yt/server/chunk_pools/public.h>

#include <yt/ytlib/table_client/helpers.h>

namespace NYT {
namespace NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

struct TEdgeDescriptor
{
    TEdgeDescriptor() = default;

    NChunkPools::IChunkPoolInput* DestinationPool = nullptr;
    bool RequiresRecoveryInfo = false;
    NTableClient::TTableWriterOptionsPtr TableWriterOptions;
    NTableClient::TTableUploadOptions TableUploadOptions;
    NYson::TYsonString TableWriterConfig;
    TNullable<NTransactionClient::TTimestamp> Timestamp;
    // Cell tag to allocate chunk lists.
    NObjectClient::TCellTag CellTag;
    bool ImmediatelyUnstageChunkLists;

    void Persist(const TPersistenceContext& context);
};

////////////////////////////////////////////////////////////////////////////////

class TDataFlowGraph
{
public:
    TDataFlowGraph() = default;

    void BuildYson(NYson::IYsonConsumer* consumer) const;

    const TProgressCounterPtr& ProgressCounter(EJobType jobType);

    void Persist(const TPersistenceContext& context);

    std::vector<EJobType> GetTopologicalOrder() const;

private:
    yhash<EJobType, TProgressCounterPtr> ProgressCounters_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT