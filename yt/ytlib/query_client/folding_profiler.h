#pragma once

#include "plan_fragment.h"

#include <util/generic/hash_set.h>
#include <util/generic/noncopyable.h>

#include <llvm/ADT/FoldingSet.h>

#include <limits>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

typedef std::function<TCGQueryCallback()> TCGQueryCallbackGenerator;
typedef std::function<TCGExpressionCallback()> TCGExpressionCallbackGenerator;

TCGQueryCallbackGenerator Profile(
    const TConstQueryPtr& query,
    llvm::FoldingSetNodeID* id,
    TCGVariables* variables,
    yhash_set<Stroka>* references);

TCGExpressionCallbackGenerator Profile(
    const TConstExpressionPtr& expr,
    const TTableSchema& schema,
    llvm::FoldingSetNodeID* id,
    TCGVariables* variables,
    yhash_set<Stroka>* references);

void Profile(
    const TTableSchema& tableSchema,
    int keySize,
    llvm::FoldingSetNodeID* id);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

////////////////////////////////////////////////////////////////////////////////

// A hasher for llvm::FoldingSetNodeID
template<>
struct hash<llvm::FoldingSetNodeID>
{
    inline size_t operator()(const llvm::FoldingSetNodeID& id) const
    {
        return id.ComputeHash();
    }
};

////////////////////////////////////////////////////////////////////////////////
