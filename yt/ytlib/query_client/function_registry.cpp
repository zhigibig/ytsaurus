#include "function_registry.h"
#include "functions.h"
#include "builtin_functions.h"

#include <ytlib/api/public.h>
#include <ytlib/api/client.h>
#include <ytlib/api/file_reader.h>

#include <core/logging/log.h>

#include <core/ytree/convert.h>

#include <core/ypath/token.h>

#include <core/concurrency/scheduler.h>

#include <core/misc/error.h>

#include <mutex>

namespace NYT {
namespace NQueryClient {

using namespace NConcurrency;
using namespace NYTree;
using namespace NYPath;

static const auto& Logger = QueryClientLogger;

////////////////////////////////////////////////////////////////////////////////

IFunctionRegistry::~IFunctionRegistry()
{ }

IFunctionDescriptorPtr IFunctionRegistry::GetFunction(const Stroka& functionName)
{
    auto function = FindFunction(functionName);
    YCHECK(function != nullptr);
    return function;
}

////////////////////////////////////////////////////////////////////////////////

void TFunctionRegistry::RegisterFunction(TIntrusivePtr<IFunctionDescriptor> function)
{
    Stroka functionName = to_lower(function->GetName());
    YCHECK(RegisteredFunctions_.insert(std::make_pair(functionName, std::move(function))).second);
}

IFunctionDescriptorPtr TFunctionRegistry::FindFunction(const Stroka& functionName)
{
    auto name = to_lower(functionName);
    if (RegisteredFunctions_.count(name) == 0) {
        return nullptr;
    } else {
        return RegisteredFunctions_.at(name);
    }
}

////////////////////////////////////////////////////////////////////////////////

void RegisterBuiltinFunctionsImpl(TFunctionRegistryPtr registry)
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
////////////////////////////////////////////////////////////////////////////////

class TCypressFunctionDescriptor
    : public TYsonSerializable
{
public:
    Stroka Name;
    std::vector<EValueType> ArgumentTypes;
    EValueType ResultType;
    Stroka ImplementationPath;

    TCypressFunctionDescriptor()
    {
        RegisterParameter("name", Name)
            .NonEmpty();
        RegisterParameter("argument_types", ArgumentTypes);
        RegisterParameter("result_type", ResultType);
        RegisterParameter("implementation_path", ImplementationPath)
            .NonEmpty();
    }
};

DECLARE_REFCOUNTED_CLASS(TCypressFunctionDescriptor)
DEFINE_REFCOUNTED_TYPE(TCypressFunctionDescriptor)

IFunctionDescriptorFetcher::~IFunctionDescriptorFetcher()
{ }

class TCypressFunctionDescriptorFetcher
    : public IFunctionDescriptorFetcher
{
public:
    TCypressFunctionDescriptorFetcher(
        NApi::IClientPtr client,
        const Stroka& cypressRegistryPath)
        : Client_(std::move(client))
        , CypressRegistryPath_(cypressRegistryPath)
    { }

    TSharedRef ReadFile(const Stroka& fileName) const
    {
        auto reader = Client_->CreateFileReader(fileName);
        WaitFor(reader->Open());

        int size = 0;
        std::vector<TSharedRef> blocks;
        while (true) {
            auto blockOrError = reader->Read().Get();
            THROW_ERROR_EXCEPTION_IF_FAILED(blockOrError);
            auto block = blockOrError.Value();

            if (!block) {
                break;
            }

            blocks.push_back(block);
            size += block.Size();
        }
        
        auto file = TSharedRef::Allocate(size);
        auto memoryOutput = TMemoryOutput(
            file.Begin(),
            size);
        
        for (const auto& block : blocks) {
            memoryOutput.Write(block.Begin(), block.Size());
        }

        return file;
    }

    virtual IFunctionDescriptorPtr LookupFunction(const Stroka& functionName) override
    {
        LOG_DEBUG("Looking for implementation of function \"%Qv\" in Cypress", functionName);
        auto functionPath = CypressRegistryPath_ + "/" + ToYPathLiteral(to_lower(functionName));

        auto cypressFunctionOrError = WaitFor(Client_->GetNode(functionPath));
        if (!cypressFunctionOrError.IsOK()) {
            return nullptr;
        }

        auto function = ConvertTo<TCypressFunctionDescriptorPtr>(
            cypressFunctionOrError.Value());

        auto implementationFile = ReadFile(function->ImplementationPath);

        return New<TUserDefinedFunction>(
            function->Name,
            function->ArgumentTypes,
            function->ResultType,
            implementationFile);
    }

private:
    NApi::IClientPtr Client_;
    const Stroka CypressRegistryPath_;
};

TCypressFunctionRegistry::TCypressFunctionRegistry(
    std::unique_ptr<IFunctionDescriptorFetcher> functionFetcher,
    TFunctionRegistryPtr builtinRegistry)
    : FunctionFetcher_(std::move(functionFetcher))
    , BuiltinRegistry_(std::move(builtinRegistry))
    , UDFRegistry_(New<TFunctionRegistry>())
{ }

IFunctionDescriptorPtr TCypressFunctionRegistry::FindFunction(const Stroka& functionName)
{
    if (auto function = BuiltinRegistry_->FindFunction(functionName)) {
        return function;
    } else if (auto function = UDFRegistry_->FindFunction(functionName)) {
        LOG_DEBUG("Found a cached implementation of function \"%Qv\"", functionName);
        return function;
    } else {
        LookupAndRegister(functionName);
        return UDFRegistry_->FindFunction(functionName);
    }
}

void TCypressFunctionRegistry::LookupAndRegister(const Stroka& functionName)
{
    auto function = FunctionFetcher_->LookupFunction(functionName);
    if (function) {
        UDFRegistry_->RegisterFunction(function);
    }
}

////////////////////////////////////////////////////////////////////////////////

TFunctionRegistryPtr CreateBuiltinFunctionRegistryImpl()
{
    auto registry = New<TFunctionRegistry>();
    RegisterBuiltinFunctionsImpl(registry);
    return registry;
}

IFunctionRegistryPtr CreateBuiltinFunctionRegistry()
{
    return CreateBuiltinFunctionRegistryImpl();
}

IFunctionRegistryPtr CreateFunctionRegistry(NApi::IClientPtr client)
{
    auto builtinRegistry = CreateBuiltinFunctionRegistryImpl();
    auto fetcher = std::make_unique<TCypressFunctionDescriptorFetcher>(
        client,
        "//tmp/udfs");
    return New<TCypressFunctionRegistry>(
        std::move(fetcher),
        builtinRegistry);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
