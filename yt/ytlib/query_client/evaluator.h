#pragma once

#include "public.h"
#include "callbacks.h"

#include <yt/core/profiling/profiler.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

class TEvaluator
    : public TIntrinsicRefCounted
{
public:
    explicit TEvaluator(
        TExecutorConfigPtr config,
        const NProfiling::TProfiler& profiler = NProfiling::TProfiler());

    TQueryStatistics Run(
        TConstBaseQueryPtr fragment,
        ISchemafulReaderPtr reader,
        ISchemafulWriterPtr writer,
        TJoinSubqueryProfiler joinProfiler,
        TConstFunctionProfilerMapPtr functionProfilers,
        TConstAggregateProfilerMapPtr aggregateProfilers,
        const TQueryBaseOptions& options);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TEvaluator)

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

