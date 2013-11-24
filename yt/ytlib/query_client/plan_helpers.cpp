#include "plan_helpers.h"
#include "plan_node.h"

#include "private.h"
#include "helpers.h"

#include <ytlib/new_table_client/schema.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

const TDataSplit& GetHeaviestSplit(const TOperator* op)
{
    switch (op->GetKind()) {
    case EOperatorKind::Scan:
            return op->As<TScanOperator>()->DataSplit();
        case EOperatorKind::Filter:
            return GetHeaviestSplit(op->As<TFilterOperator>()->GetSource());
        case EOperatorKind::Project:
            return GetHeaviestSplit(op->As<TProjectOperator>()->GetSource());
        default:
            YUNREACHABLE();
    }
}

TTableSchema InferTableSchema(const TOperator* op)
{
    switch (op->GetKind()) {
        case EOperatorKind::Scan:
            return GetTableSchemaFromDataSplit(op->As<TScanOperator>()->DataSplit());
        case EOperatorKind::Filter:
            return InferTableSchema(op->As<TFilterOperator>()->GetSource());
        case EOperatorKind::Project: {
            TTableSchema result;
            auto* typedOp = op->As<TProjectOperator>();
            for (const auto& projection : typedOp->Projections()) {
                result.Columns().emplace_back(
                    InferName(projection),
                    InferType(projection));
            }
            return result;
        }
        case EOperatorKind::Union: {
            TTableSchema result;
            bool didChooseTableSchema = false;
            auto* typedOp = op->As<TUnionOperator>();
            for (const auto& source : typedOp->Sources()) {
                if (!didChooseTableSchema) {
                    result = InferTableSchema(source);
                    didChooseTableSchema = true;
                } else {
                    YCHECK(result == InferTableSchema(source));
                }
            }
            return result;
        }
        default:
            YUNREACHABLE();
    }
}

TKeyColumns InferKeyColumns(const TOperator* op)
{
    switch (op->GetKind()) {
        case EOperatorKind::Scan:
            return GetKeyColumnsFromDataSplit(op->As<TScanOperator>()->DataSplit());
        case EOperatorKind::Filter:
            return InferKeyColumns(op->As<TFilterOperator>()->GetSource());
        case EOperatorKind::Project:
            return TKeyColumns();
        case EOperatorKind::Union: {
            TKeyColumns result;
            bool didChooseKeyColumns = false;
            auto* typedOp = op->As<TUnionOperator>();
            for (const auto& source : typedOp->Sources()) {
                if (!didChooseKeyColumns) {
                    result = InferKeyColumns(source);
                    didChooseKeyColumns = true;
                } else {
                    YCHECK(result == InferKeyColumns(source));
                }
            }
            return result;
        }
        default:
            YUNREACHABLE();
    }
}

EValueType InferType(const TExpression* expr, bool ignoreCached)
{
    if (!ignoreCached) {
        auto cachedType = expr->GetCachedType();
        if (cachedType != EValueType::Null) {
            return cachedType;
        }
        cachedType = InferType(expr, true);
        expr->SetCachedType(cachedType);
        return cachedType;
    }
    switch (expr->GetKind()) {
        case EExpressionKind::IntegerLiteral:
            return EValueType::Integer;
        case EExpressionKind::DoubleLiteral:
            return EValueType::Double;
        case EExpressionKind::Reference:
            // For reference expression, always trust cached type.
            return expr->GetCachedType();
        case EExpressionKind::Function:
            YUNIMPLEMENTED();
        case EExpressionKind::BinaryOp: {
            auto* typedExpr = expr->As<TBinaryOpExpression>();
            auto lhsType = InferType(typedExpr->GetLhs());
            auto rhsType = InferType(typedExpr->GetRhs());
            if (lhsType != rhsType) {
                THROW_ERROR_EXCEPTION(
                    "Type mismatch between left- and right-hand sides in expression %s",
                    ~typedExpr->GetSource().Quote())
                    << TErrorAttribute("lhs_type", lhsType.ToString())
                    << TErrorAttribute("rhs_type", rhsType.ToString());
            }
            if (lhsType != EValueType::Integer && lhsType != EValueType::Double) {
                THROW_ERROR_EXCEPTION(
                    "Expression %s require numeric operands",
                    ~typedExpr->GetSource().Quote())
                    << TErrorAttribute("lhs_type", lhsType.ToString())
                    << TErrorAttribute("rhs_type", rhsType.ToString());
            }
            switch (typedExpr->GetOpcode()) {
                // For arithmetic operations resulting type matches operands' type.
                case EBinaryOp::Plus:
                case EBinaryOp::Minus:
                case EBinaryOp::Multiply:
                case EBinaryOp::Divide:
                    return lhsType;
                // For integral and logical operations operands must be integral.
                case EBinaryOp::Modulo:
                case EBinaryOp::And:
                case EBinaryOp::Or:
                    if (lhsType != EValueType::Integer) {
                        THROW_ERROR_EXCEPTION(
                            "Operands must be integral in expression %s",
                            ~typedExpr->GetSource().Quote())
                            << TErrorAttribute("lhs_type", lhsType.ToString())
                            << TErrorAttribute("rhs_type", rhsType.ToString());
                    }
                    return EValueType::Integer;
                // For comparsion operations resulting type is integer type
                // because we do not have built-in boolean type, and thus
                // we represent comparsion result as 0/1.
                case EBinaryOp::Equal:
                case EBinaryOp::NotEqual:
                case EBinaryOp::Less:
                case EBinaryOp::LessOrEqual:
                case EBinaryOp::Greater:
                case EBinaryOp::GreaterOrEqual:
                    return EValueType::Integer;
                default:
                    YUNREACHABLE();
            }
        }
        default:
            YUNREACHABLE();
    }
}

Stroka InferName(const TExpression* expr, bool ignoreCached)
{
    if (!ignoreCached) {
        auto cachedName = expr->GetCachedName();
        if (!cachedName.empty()) {
            return cachedName;
        }
        cachedName = InferName(expr, true);
        expr->SetCachedName(cachedName);
        return cachedName;
    }
    switch (expr->GetKind()) {
        case EExpressionKind::IntegerLiteral:
            return ToString(expr->As<TIntegerLiteralExpression>()->GetValue());
        case EExpressionKind::DoubleLiteral:
            return ToString(expr->As<TDoubleLiteralExpression>()->GetValue());
        case EExpressionKind::Reference:
            return expr->As<TReferenceExpression>()->GetColumnName();
        case EExpressionKind::Function: {
            auto* typedExpr = expr->As<TFunctionExpression>();
            Stroka result;
            result += typedExpr->GetFunctionName();
            result += "(";
            for (const auto& argument : typedExpr->Arguments()) {
                if (!result.empty()) {
                    result += ", ";
                }
                result += InferName(argument);
            }
            result += ")";
            return result;
        }
        case EExpressionKind::BinaryOp: {
            auto* typedExpr = expr->As<TBinaryOpExpression>();
            auto canOmitParenthesis = [] (const TExpression* expr) {
                return
                    expr->GetKind() == EExpressionKind::IntegerLiteral ||
                    expr->GetKind() == EExpressionKind::DoubleLiteral ||
                    expr->GetKind() == EExpressionKind::Reference ||
                    expr->GetKind() == EExpressionKind::Function;
            };
            auto lhsName = InferName(typedExpr->GetLhs());
            if (!canOmitParenthesis(typedExpr->GetLhs())) {
                lhsName = "(" + lhsName + ")";
            }
            auto rhsName = InferName(typedExpr->GetRhs());
            if (!canOmitParenthesis(typedExpr->GetRhs())) {
                rhsName = "(" + rhsName + ")";
            }
            return
                lhsName +
                " " + GetBinaryOpcodeLexeme(typedExpr->GetOpcode()) + " " +
                rhsName;
        }
        default:
            YUNREACHABLE();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

