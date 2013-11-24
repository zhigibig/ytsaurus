#pragma once

#include "public.h"
#include "callbacks.h"
#include "plan_fragment.h"

#include <core/concurrency/coroutine.h>

#include <core/logging/tagged_logger.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

// Forward-declare node types.
class TScanOperator;
class TUnionOperator;
class TFilterOperator;
class TProjectOperator;

class TIntegerLiteralExpression;
class TDoubleLiteralExpression;
class TReferenceExpression;
class TFunctionExpression;
class TBinaryOpExpression;

// TODO(sandello): Why refcounted?
class TEvaluateController
    : public TRefCounted
{
public:
    TEvaluateController(
        IEvaluateCallbacks* callbacks,
        const TPlanFragment& fragment,
        IWriterPtr writer);

    ~TEvaluateController();

    TError Run();

    IEvaluateCallbacks* GetCallbacks()
    {
        return Callbacks_;
    }

    TPlanContext* GetContext()
    {
        return Fragment_.GetContext().Get();
    }

    const TOperator* GetHead()
    {
        return Fragment_.GetHead();
    }

private:
    typedef NConcurrency::TCoroutine<void(std::vector<TRow>*)> TProducer;

    TProducer CreateProducer(const TOperator* op);

    void ScanRoutine(
        const TScanOperator* op,
        TProducer& self,
        std::vector<TRow>* rows);
    void UnionRoutine(
        const TUnionOperator* op,
        TProducer& self,
        std::vector<TRow>* rows);
    void FilterRoutine(
        const TFilterOperator* op,
        TProducer& self,
        std::vector<TRow>* rows);
    void ProjectRoutine(
        const TProjectOperator* op,
        TProducer& self,
        std::vector<TRow>* rows);

    TValue EvaluateExpression(
        const TExpression* expr,
        const TRow row) const;
    TValue EvaluateFunctionExpression(
        const TFunctionExpression* expr,
        const TRow row) const;
    TValue EvaluateBinaryOpExpression(
        const TBinaryOpExpression* expr,
        const TRow row) const;

    void SetHead(const TOperator* head)
    {
        Fragment_.SetHead(head);
    }

    template <class TFunctor>
    void Rewrite(const TFunctor& functor)
    {
        SetHead(Apply(GetContext(), GetHead(), functor));
    }

private:
    IEvaluateCallbacks* Callbacks_;
    TPlanFragment Fragment_;
    IWriterPtr Writer_;

    NVersionedTableClient::TNameTablePtr NameTable_;

    NLog::TTaggedLogger Logger;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

