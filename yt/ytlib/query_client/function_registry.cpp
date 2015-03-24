#include "function_registry.h"
#include "functions.h"
#include "builtin_functions.h"

#include <ytlib/api/public.h>

#include <ytlib/api/public.h>

#include <mutex>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

void TFunctionRegistry::RegisterFunction(TIntrusivePtr<IFunctionDescriptor> function)
{
    Stroka functionName = to_lower(function->GetName());
    YCHECK(RegisteredFunctions_.insert(std::make_pair(functionName, std::move(function))).second);
}

IFunctionDescriptor& TFunctionRegistry::GetFunction(const Stroka& functionName)
{
    return *RegisteredFunctions_.at(to_lower(functionName));
}

bool TFunctionRegistry::IsRegistered(const Stroka& functionName)
{
    return RegisteredFunctions_.count(to_lower(functionName)) != 0;
}

void RegisterFunctionsImpl(TFunctionRegistryPtr registry)
{
    registry->RegisterFunction(New<TIfFunction>());
    registry->RegisterFunction(New<TIsPrefixFunction>());
    registry->RegisterFunction(New<TIsSubstrFunction>());
    registry->RegisterFunction(New<TLowerFunction>());
    registry->RegisterFunction(New<THashFunction>(
        "simple_hash",
        "SimpleHash"));
    registry->RegisterFunction(New<THashFunction>(
        "farm_hash",
        "FarmHash"));
    registry->RegisterFunction(New<TIsNullFunction>());
    registry->RegisterFunction(New<TCastFunction>(
        EValueType::Int64,
        "int64"));
    registry->RegisterFunction(New<TCastFunction>(
        EValueType::Uint64,
        "uint64"));
    registry->RegisterFunction(New<TCastFunction>(
        EValueType::Double,
        "double"));
}

TFunctionRegistryPtr CreateBuiltinFunctionRegistry()
{
    auto registry = New<TFunctionRegistry>();
    RegisterFunctionsImpl(registry);
    return registry;
}

TFunctionRegistryPtr CreateFunctionRegistry(NApi::IClientPtr client)
{
    //TODO
    return CreateBuiltinFunctionRegistry();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
