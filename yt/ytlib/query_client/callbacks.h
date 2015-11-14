#pragma once

#include "public.h"

#include <yt/core/actions/future.h>

#include <yt/core/rpc/public.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

typedef std::function<TQueryStatistics(
    const TQueryPtr& query,
    TGuid dataId,
    ISchemafulWriterPtr writer)> TExecuteQuery;

////////////////////////////////////////////////////////////////////////////////

struct IExecutor
    : public virtual TRefCounted
{
    virtual TFuture<TQueryStatistics> Execute(
        TPlanFragmentPtr fragment,
        ISchemafulWriterPtr writer) = 0;

};

DEFINE_REFCOUNTED_TYPE(IExecutor)

struct ISubExecutor
    : public virtual TRefCounted
{
    virtual TFuture<TQueryStatistics> Execute(
        TPlanSubFragmentPtr fragment,
        ISchemafulWriterPtr writer) = 0;

};

DEFINE_REFCOUNTED_TYPE(ISubExecutor)

////////////////////////////////////////////////////////////////////////////////

struct IPrepareCallbacks
{
    virtual ~IPrepareCallbacks()
    { }

    //! Returns an initial split for a given path.
    virtual TFuture<TDataSplit> GetInitialSplit(
        const NYPath::TYPath& path,
        TTimestamp timestamp) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

