#include "user_defined_functions.h"

#include "cg_fragment_compiler.h"
#include "plan_helpers.h"

#include <new_table_client/row_base.h>

#include <llvm/Object/ObjectFile.h>

#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <llvm/IRReader/IRReader.h>

#include <llvm/Linker/Linker.h>

using namespace llvm;

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

Value* SplitStringArguments(TCGValue argumentValue)
{
    return argumentValue.GetData();
}

TCodegenExpression PropagateNullArguments(
    std::vector<TCodegenExpression> codegenArgs,
    std::vector<Value*> argumentValues,
    std::function<Value*(std::vector<Value*>, TCGContext& builder)> codegenNonNull,
    EValueType type,
    const Stroka& name)
{
    return [=] (TCGContext& builder, Value* row) {
        if (codegenArgs.empty()) {
            return TCGValue::CreateFromValue(
                builder,
                builder.getFalse(),
                nullptr,
                codegenNonNull(argumentValues, builder),
                type);
        } else {
            auto codegenArg = codegenArgs.front();
            auto argumentValue = codegenArg(builder, row);
                
            auto splitArgumentValue = SplitStringArguments(argumentValue);

            auto newCodegenArgs = std::vector<TCodegenExpression>(
                codegenArgs.rbegin(),
                codegenArgs.rend());
            newCodegenArgs.pop_back();
            auto newArgumentValues = argumentValues;
            newArgumentValues.push_back(splitArgumentValue);

            return CodegenIf<TCGContext, TCGValue>(
                builder,
                argumentValue.IsNull(),
                [&] (TCGContext& builder) {
                    return TCGValue::CreateNull(builder, type);
                },
                [&] (TCGContext& builder) {
                    return PropagateNullArguments(
                        newCodegenArgs,
                        newArgumentValues,
                        codegenNonNull,
                        type,
                        name)(builder, row);
                },
                Twine(name.c_str()));
        }
    };
}

TCodegenExpression TSimpleCallingConvention::MakeCodegenExpr(
    std::vector<TCodegenExpression> codegenArgs,
    EValueType type,
    const Stroka& name) const
{
    auto callUdf = [
        this_ = MakeStrong(this),
        type,
        name
    ] (std::vector<Value*> argValues, TCGContext& builder) {
        return this_->LLVMValue(argValues, builder);
    };

    return PropagateNullArguments(
        codegenArgs,
        std::vector<Value*>(),
        callUdf,
        type,
        name);
}

////////////////////////////////////////////////////////////////////////////////

TUserDefinedFunction::TUserDefinedFunction(
    const Stroka& functionName,
    std::vector<EValueType> argumentTypes,
    EValueType resultType,
    TSharedRef implementationFile)
    : TTypedFunction(
        functionName,
        std::vector<TType>(argumentTypes.begin(), argumentTypes.end()),
        resultType)
    , FunctionName_(functionName)
    , ImplementationFile_(implementationFile)
    , ResultType_(resultType)
    , ArgumentTypes_(argumentTypes)
{ }

llvm::Type* ConvertToLLVMType(EValueType type, TCGContext& builder)
{
    auto& context = builder.getContext();
    switch (type) {
        case EValueType::Int64:
        case EValueType::Uint64:
            return Type::getInt64Ty(context);
        case EValueType::Double:
            return Type::getDoubleTy(context);
        case EValueType::Boolean:
            return Type::getInt1Ty(context);
        case EValueType::String:
            return Type::getInt8PtrTy(context);
        default:
            return nullptr;
    }
}

Stroka LLVMTypeToString(llvm::Type* tp)
{
    std::string str;
    llvm::raw_string_ostream stream(str);
    tp->print(stream);
    return Stroka(stream.str());
}

void TUserDefinedFunction::CheckCallee(llvm::Function* callee, TCGContext& builder) const
{
    if (callee == nullptr) {
        THROW_ERROR_EXCEPTION(
            "Could not find LLVM bitcode for %Qv",
            FunctionName_);
    } else if (callee->arg_size() != ArgumentTypes_.size()) {
        THROW_ERROR_EXCEPTION(
            "Wrong number of arguments in LLVM bitcode: expected %v, got %v",
            ArgumentTypes_.size(),
            callee->arg_size());
    } else if (callee->getReturnType() != ConvertToLLVMType(ResultType_, builder)) {
        THROW_ERROR_EXCEPTION(
            "Wrong result type in LLVM bitcode: expected %Qv, got %Qv",
            LLVMTypeToString(ConvertToLLVMType(ResultType_, builder)),
            LLVMTypeToString(callee->getReturnType()));
    }

    auto i = 0;
    auto expected = ArgumentTypes_.begin();
    for (
        auto actual = callee->arg_begin();
        expected != ArgumentTypes_.end();
        expected++, actual++, i++)
    {
        if (actual->getType() != ConvertToLLVMType(*expected, builder)) {
            THROW_ERROR_EXCEPTION(
                "Wrong type for argument %Qv in LLVM bitcode: expected %Qv, got %Qv",
                i,
                LLVMTypeToString(ConvertToLLVMType(*expected, builder)),
                LLVMTypeToString(actual->getType()));
        }
    }
}

Function* TUserDefinedFunction::GetLLVMFunction(TCGContext& builder) const
{
    auto module = builder.Module->GetModule();
    auto callee = module->getFunction(StringRef(FunctionName_));
    if (!callee) {
        auto diag = SMDiagnostic();
        auto buffer = MemoryBufferRef(
            StringRef(ImplementationFile_.Begin(), ImplementationFile_.Size()),
            StringRef("impl"));
        auto implModule = parseIR(buffer, diag, builder.getContext());

        if (!implModule) {
            THROW_ERROR_EXCEPTION(
                "Error parsing LLVM bitcode: %v")
                << TError(Stroka(diag.getMessage().str()));
        }

        Linker::LinkModules(module, implModule.get());
        callee = module->getFunction(StringRef(FunctionName_));
    }
    CheckCallee(callee, builder);
    return callee;
}

Value* TUserDefinedFunction::LLVMValue(
    std::vector<Value*> argumentValues,
    TCGContext& builder) const
{
        auto callee = GetLLVMFunction(builder);
        auto result = builder.CreateCall(callee, argumentValues);
        return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT
