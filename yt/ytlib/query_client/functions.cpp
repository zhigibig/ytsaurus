#include "functions.h"

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

const std::set<EValueType> SimpleHashFunction::HashTypes = 
    std::set<EValueType>({
        EValueType::Int64,
        EValueType::Uint64,
        EValueType::Boolean,
        EValueType::String });

const std::set<EValueType> CastFunction::CastTypes = std::set<EValueType>({
        EValueType::Int64,
        EValueType::Uint64,
        EValueType::Double });

EValueType TTypedFunction::TypingFunction(
    const std::vector<TType>& expectedArgTypes,
    TType repeatedArgType,
    TType resultType,
    Stroka functionName,
    const std::vector<EValueType>& argTypes,
    const TStringBuf& source)
{
    std::unordered_map<TTypeArgument, EValueType> genericAssignments;

    auto isSubtype = [&] (EValueType type1, TType type2) {
        YCHECK(!type2.TryAs<TTypeArgument>());
        if (auto unionType = type2.TryAs<TUnionType>()) {
            return unionType->count(type1) > 0;
        } else if (auto concreteType = type2.TryAs<EValueType>()) {
            return type1 == *concreteType;
        }
        return false;
    };

    auto unify = [&] (TType type1, EValueType type2) {
        if (auto genericId = type1.TryAs<TTypeArgument>()) {
            if (genericAssignments.count(*genericId)) {
                return genericAssignments[*genericId] == type2;
            } else {
                genericAssignments[*genericId] = type2;
                return true;
            }
        } else {
            return isSubtype(type2, type1);
        }
    };

    //TODO: better error messages
    auto arg = argTypes.begin();
    auto expectedArg = expectedArgTypes.begin();
    for (;
        expectedArg != expectedArgTypes.end() && arg != argTypes.end();
        arg++, expectedArg++)
    {
        if (!unify(*expectedArg, *arg)) {
            THROW_ERROR_EXCEPTION(
                "Wrong argument type",
                source);
        }
    }


    if (expectedArg != expectedArgTypes.end()) {
        THROW_ERROR_EXCEPTION(
            "Expression %Qv expects %v arguments",
            functionName,
            argTypes.size())
            << TErrorAttribute("expression", source);
    }

    for (; arg != argTypes.end(); arg++)
    {
        if (!unify(repeatedArgType, *arg)) {
            THROW_ERROR_EXCEPTION(
                "Wrong argument type",
                source);
        }
    }

    if (auto genericResult = resultType.TryAs<TTypeArgument>()) {
        if (!genericAssignments.count(*genericResult)) {
            THROW_ERROR_EXCEPTION(
                "Ambiguous result type",
                source);
        }
        return genericAssignments[*genericResult];
    } else if (!resultType.TryAs<EValueType>()) {
        THROW_ERROR_EXCEPTION(
            "Ambiguous result type",
            source);
    } else {
        return resultType.As<EValueType>();
    }

    return EValueType::Null;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
