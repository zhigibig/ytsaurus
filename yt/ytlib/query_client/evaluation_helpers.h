#pragma once

#include "public.h"
#include "callbacks.h"
#include "function_context.h"

#include <yt/ytlib/api/rowset.h>

#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/core/codegen/function.h>

#include <yt/core/misc/chunked_memory_pool.h>

#include <deque>
#include <unordered_map>
#include <unordered_set>

#include <sparsehash/dense_hash_set>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

static const size_t InitialGroupOpHashtableCapacity = 1024;

using THasherFunction = ui64(TRow);
using TComparerFunction = char(TRow, TRow);

struct TExecutionContext;

using TJoinEvaluator = std::function<void(
    TExecutionContext* executionContext,
    THasherFunction* hasher,
    TComparerFunction* comparer,
    TSharedRange<TRow> keys,
    TSharedRange<TRow> allRows,
    // TODO(babenko): TSharedRange?
    std::vector<TRow>* joinedRows)>;

struct TExpressionContext
{
#ifndef NDEBUG
    size_t StackSizeGuardHelper;
#endif
    const TTableSchema* Schema;

    const std::vector<TSharedRange<TRow>>* LiteralRows;

    TRowBufferPtr IntermediateBuffer;
};

struct TExecutionContext
    : public TExpressionContext
{
    ISchemafulReaderPtr Reader;
    ISchemafulWriterPtr Writer;

    TRowBufferPtr PermanentBuffer;
    TRowBufferPtr OutputBuffer;

    // Rows stored in OutputBuffer
    std::vector<TRow>* OutputRowsBatch;

    TQueryStatistics* Statistics;

    // These limits prevent full scan.
    i64 InputRowLimit;
    i64 OutputRowLimit;
    i64 GroupRowLimit;
    i64 JoinRowLimit;

    // Limit from LIMIT clause.
    i64 Limit;

    // "char" type is to due LLVM interop.
    char StopFlag = false;

    std::vector<TJoinEvaluator> JoinEvaluators;
    TExecuteQuery ExecuteCallback;

    std::deque<TFunctionContext> FunctionContexts;
};

namespace NDetail {
struct TGroupHasher
{
    THasherFunction* Ptr_;
    TGroupHasher(THasherFunction* ptr)
        : Ptr_(ptr)
    { }

    ui64 operator () (TRow row) const
    {
        return Ptr_(row);
    }
};

struct TRowComparer
{
public:
    TRowComparer(TComparerFunction* ptr)
        : Ptr_(ptr)
    { }

    bool operator () (TRow a, TRow b) const
    {
        return a.GetHeader() == b.GetHeader() || a.GetHeader() && b.GetHeader() && Ptr_(a, b);
    }

private:
    TComparerFunction* Ptr_;
};
} // namespace NDetail

using TLookupRows = google::sparsehash::dense_hash_set<
    TRow,
    NDetail::TGroupHasher,
    NDetail::TRowComparer>;

using TJoinLookupRows = std::unordered_multiset<
    TRow,
    NDetail::TGroupHasher,
    NDetail::TRowComparer>;

class TTopCollector
{
    class TComparer
    {
    public:
        explicit TComparer(TComparerFunction* ptr)
            : Ptr_(ptr)
        { }

        bool operator() (const std::pair<TRow, int>& lhs, const std::pair<TRow, int>& rhs) const
        {
            return (*this)(lhs.first, rhs.first);
        }

        bool operator () (TRow a, TRow b) const
        {
            return Ptr_(a, b);
        }

    private:
        TComparerFunction* const Ptr_;
    };

public:
    TTopCollector(i64 limit, TComparerFunction* comparer);

    // TODO(babenko): TSharedRange?
    std::vector<TRow> GetRows(int rowSize) const
    {
        std::vector<TRow> result;
        result.reserve(Rows_.size());
        for (const auto& pair : Rows_) {
            result.push_back(pair.first);
        }
        std::sort(result.begin(), result.end(), Comparer_);
        for (auto& row : result) {
            row.SetCount(rowSize);
        }
        return result;
    }

    void AddRow(TRow row);

private:
    // GarbageMemorySize <= AllocatedMemorySize <= TotalMemorySize
    size_t TotalMemorySize_ = 0;
    size_t AllocatedMemorySize_ = 0;
    size_t GarbageMemorySize_ = 0;

    TComparer Comparer_;

    std::vector<TRowBufferPtr> Buffers_;
    std::vector<int> EmptyBufferIds_;
    std::vector<std::pair<TRow, int>> Rows_;
    
    std::pair<TRow, int> Capture(TRow row);

    void AccountGarbage(TRow row);

};

struct TCGVariables
{
    TRowBuilder ConstantsRowBuilder;
    std::vector<TSharedRange<TRow>> LiteralRows;
    std::vector<TJoinEvaluator> JoinEvaluators;
};

typedef void (TCGQuerySignature)(TRow, TExecutionContext*, TFunctionContext**);
typedef void (TCGExpressionSignature)(TValue*, TRow, TRow, TExpressionContext*, TFunctionContext**);
typedef void (TCGAggregateInitSignature)(TExecutionContext*, TValue*);
typedef void (TCGAggregateUpdateSignature)(TExecutionContext*, TValue*, TValue*, TValue*);
typedef void (TCGAggregateMergeSignature)(TExecutionContext*, TValue*, TValue*, TValue*);
typedef void (TCGAggregateFinalizeSignature)(TExecutionContext*, TValue*, TValue*);

using TCGQueryCallback = NCodegen::TCGFunction<TCGQuerySignature>;
using TCGExpressionCallback = NCodegen::TCGFunction<TCGExpressionSignature>;
using TCGAggregateInitCallback = NCodegen::TCGFunction<TCGAggregateInitSignature>;
using TCGAggregateUpdateCallback = NCodegen::TCGFunction<TCGAggregateUpdateSignature>;
using TCGAggregateMergeCallback = NCodegen::TCGFunction<TCGAggregateMergeSignature>;
using TCGAggregateFinalizeCallback = NCodegen::TCGFunction<TCGAggregateFinalizeSignature>;

struct TCGAggregateCallbacks
{
    TCGAggregateInitCallback Init;
    TCGAggregateUpdateCallback Update;
    TCGAggregateMergeCallback Merge;
    TCGAggregateFinalizeCallback Finalize;
};

////////////////////////////////////////////////////////////////////////////////

bool UpdateAndCheckRowLimit(i64* limit, char* flag);

TJoinEvaluator GetJoinEvaluator(
    const TJoinClause& joinClause,
    TConstExpressionPtr predicate,
    const TTableSchema& selfTableSchema,
    const TColumnEvaluatorCachePtr evaluatorCache);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

