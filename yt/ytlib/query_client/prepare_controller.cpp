#include "prepare_controller.h"

#include "private.h"
#include "helpers.h"

#include "callbacks.h"

#include "plan_node.h"
#include "plan_visitor.h"
#include "plan_helpers.h"

#include "lexer.h"
#include "parser.hpp"

#include <ytlib/table_client/chunk_meta_extensions.h>
#include <ytlib/new_table_client/chunk_meta_extensions.h>
#include <ytlib/new_table_client/schema.h>

#include <core/concurrency/fiber.h>

namespace NYT {
namespace NQueryClient {

using namespace NYT::NYPath;
using namespace NYT::NConcurrency;

static auto& Logger = QueryClientLogger;

////////////////////////////////////////////////////////////////////////////////

TPrepareController::TPrepareController(
    IPrepareCallbacks* callbacks,
    const Stroka& source)
    : Callbacks_(callbacks)
    , Source_(source)
    , Context_(New<TPlanContext>())
    , Head_(nullptr)
{ }

TPrepareController::~TPrepareController()
{ }

namespace {
class TCheckAndPruneReferences
    : public TPlanVisitor
{
public:
    explicit TCheckAndPruneReferences(TPrepareController* controller)
        : Controller_(controller)
    {
        LiveColumns_.resize(Controller_->GetContext()->GetTableCount());
    }

    virtual bool Visit(const TScanOperator* op) override
    {
        // Scan operators are always visited in the end,
        // because they are leaf nodes.
        auto tableSchema = GetProtoExtension<NVersionedTableClient::NProto::TTableSchemaExt>(op->DataSplit().chunk_meta().extensions());
        auto& liveColumns = LiveColumns_[op->GetTableIndex()];

        {
            NVersionedTableClient::NProto::TTableSchemaExt filteredTableSchema;
            for (const auto& columnSchema : tableSchema.columns()) {
                if (liveColumns.find(columnSchema.name()) != liveColumns.end()) {
                    LOG_DEBUG("Keeping column %s in the schema", ~columnSchema.name().Quote());
                    filteredTableSchema.add_columns()->CopyFrom(columnSchema);
                } else {
                    LOG_DEBUG("Prunning column %s from the schema", ~columnSchema.name().Quote());
                }
            }
            SetProtoExtension<NVersionedTableClient::NProto::TTableSchemaExt>(
                op->AsMutable<TScanOperator>()->DataSplit().mutable_chunk_meta()->mutable_extensions(),
                filteredTableSchema);
        }

        return true;
    }

    virtual bool Visit(const TFilterOperator* op) override
    {
        Traverse(this, op->GetPredicate());
        return true;
    }

    virtual bool Visit(const TProjectOperator* op) override
    {
        for (auto& projection : op->Projections()) {
            Traverse(this, projection);
        }
        return true;
    }

    virtual bool Visit(const TReferenceExpression* expr) override
    {
        auto& descriptor = Controller_->GetContext()
            ->GetTableDescriptorByIndex(expr->GetTableIndex());

        TScanOperator* op = reinterpret_cast<TScanOperator*>(descriptor.Opaque);
        YCHECK(op);

        auto* mutableExpr = expr->AsMutable<TReferenceExpression>();

        const auto keyColumns = GetKeyColumnsFromDataSplit(op->DataSplit());
        const auto tableSchema = GetTableSchemaFromDataSplit(op->DataSplit());

        {
            auto column = tableSchema.FindColumn(expr->GetColumnName());

            if (!column) {
                THROW_ERROR_EXCEPTION(
                    "Table %s does not have column %s in its schema",
                    ~descriptor.Path,
                    ~expr->GetColumnName().Quote());
            }

            mutableExpr->SetCachedType(column->Type);
        }

        {
            auto it = std::find_if(
                keyColumns.begin(),
                keyColumns.end(),
                [&expr] (const Stroka& name) {
                    return expr->GetColumnName() == name;
                });

            if (it != keyColumns.end()) {
                mutableExpr->SetCachedKeyIndex(std::distance(keyColumns.begin(), it));
            } else {
                mutableExpr->SetCachedKeyIndex(-1);
            }
        }

        LiveColumns_[expr->GetTableIndex()].insert(expr->GetColumnName());

        return true;
    }

private:
    TPrepareController* Controller_;
    std::vector<std::set<Stroka>> LiveColumns_;

};
} // anonymous namespace

TPlanFragment TPrepareController::Run()
{
    ParseSource();
    GetInitialSplits();
    CheckAndPruneReferences();
    TypecheckExpressions();
    return TPlanFragment(std::move(Context_), Head_);
}

void TPrepareController::ParseSource()
{
    // Hook up with debug information for better error messages.
    GetContext()->SetDebugInformation(TDebugInformation(Source_));

    TLexer lexer(GetContext(), Source_);
    TParser parser(lexer, GetContext(), &Head_);

    int result = parser.parse();
    if (result != 0) {
        THROW_ERROR_EXCEPTION("Failed to parse query");
    }
}

void TPrepareController::GetInitialSplits()
{
    Visit(Head_, [this] (const TOperator* op)
    {
        if (auto* scanOp = op->AsMutable<TScanOperator>()) {
            auto tableIndex = scanOp->GetTableIndex();
            auto& tableDescriptor = GetContext()->GetTableDescriptorByIndex(tableIndex);
            LOG_DEBUG("Getting initial data split for %s", ~tableDescriptor.Path);
            // XXX(sandello): We have just one table at the moment.
            // Will put TParallelAwaiter here in case of multiple tables.
            auto dataSplitOrError = WaitFor(
                GetCallbacks()->GetInitialSplit(tableDescriptor.Path));
            THROW_ERROR_EXCEPTION_IF_FAILED(
                dataSplitOrError,
                "Failed to get initial data split for table %s",
                ~tableDescriptor.Path);
            scanOp->DataSplit() = dataSplitOrError.GetValue();
        }
    });
}

void TPrepareController::CheckAndPruneReferences()
{
    TCheckAndPruneReferences visitor(this);
    Traverse(&visitor, Head_);
}

void TPrepareController::TypecheckExpressions()
{
    Visit(Head_, [this] (const TOperator* op)
    {
        if (auto* typedOp = op->As<TFilterOperator>()) {
            auto actualType = typedOp->GetPredicate()->GetType();
            auto expectedType = EValueType(EValueType::Integer);
            if (actualType != expectedType) {
                THROW_ERROR_EXCEPTION("WHERE-clause is not of valid type")
                    << TErrorAttribute("actual_type", actualType)
                    << TErrorAttribute("expected_type", expectedType);
            }
        }
        if (auto* typedOp = op->As<TProjectOperator>()) {
            for (auto& projection : typedOp->Projections()) {
                projection->GetType(); // Force typechecking.
            }
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

