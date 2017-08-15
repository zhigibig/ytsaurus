#pragma once


namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TAuth;
struct TExecuteBatchOptions;

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

struct IRetryPolicy;
class TRawBatchRequest;

////////////////////////////////////////////////////////////////////////////////

//
// marks `batchRequest' as executed
void ExecuteBatch(
    const TAuth& auth,
    TRawBatchRequest& batchRequest,
    const TExecuteBatchOptions& options,
    IRetryPolicy& retryPolicy);

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NYT
