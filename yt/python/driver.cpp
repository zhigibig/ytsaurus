#include "public.h"
#include "buffered_stream.h"
#include "descriptor.h"
#include "helpers.h"
#include "response.h"
#include "serialize.h"
#include "shutdown.h"
#include "stream.h"

#include <yt/ytlib/api/admin.h>
#include <yt/ytlib/api/connection.h>
#include <yt/ytlib/api/transaction.h>

#include <yt/ytlib/driver/config.h>
#include <yt/ytlib/driver/driver.h>

#include <yt/ytlib/formats/format.h>

#include <yt/ytlib/hydra/hydra_service_proxy.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/core/concurrency/async_stream.h>

#include <yt/core/logging/log_manager.h>
#include <yt/core/logging/config.h>

#include <yt/core/misc/intrusive_ptr.h>
#include <yt/core/misc/crash_handler.h>

#include <yt/core/tracing/trace_manager.h>
#include <yt/core/tracing/config.h>

#include <yt/core/ytree/convert.h>

#include <contrib/libs/pycxx/Extensions.hxx>
#include <contrib/libs/pycxx/Objects.hxx>

namespace NYT {
namespace NPython {

///////////////////////////////////////////////////////////////////////////////

using namespace NFormats;
using namespace NDriver;
using namespace NYson;
using namespace NYTree;
using namespace NConcurrency;
using namespace NTabletClient;

///////////////////////////////////////////////////////////////////////////////

static const NLogging::TLogger Logger("PythonDriver");
static yhash<TGuid, TWeakPtr<IDriver>> ActiveDrivers;

///////////////////////////////////////////////////////////////////////////////

Py::Exception CreateYtError(const std::string& message, const NYT::TError& error = TError())
{
    auto ytModule = Py::Module(PyImport_ImportModule("yt.common"), true);
    auto ytErrorClass = Py::Callable(GetAttr(ytModule, "YtError"));

    std::vector<TError> innerErrors({error});

    Py::Dict options;
    options.setItem("message", ConvertTo<Py::Object>(message));
    options.setItem("code", ConvertTo<Py::Object>(1));
    options.setItem("inner_errors", ConvertTo<Py::Object>(innerErrors));
    auto ytError = ytErrorClass.apply(Py::Tuple(), options);
    return Py::Exception(*ytError.type(), ytError);
}

#define CATCH(message) \
    catch (const NYT::TErrorException& error) { \
        throw CreateYtError(message, error.Error()); \
    } catch (const std::exception& ex) { \
        if (PyErr_ExceptionMatches(PyExc_BaseException)) { \
            throw; \
        } else { \
            throw CreateYtError(message, TError(ex)); \
        } \
    }

///////////////////////////////////////////////////////////////////////////////

INodePtr ConvertObjectToNode(const Py::Object& obj)
{
    auto factory = GetEphemeralNodeFactory();
    auto builder = CreateBuilderFromFactory(factory);
    builder->BeginTree();
    Serialize(obj, builder.get(), MakeNullable<Stroka>("utf-8"));
    return builder->EndTree();
}

class TDriver
    : public Py::PythonClass<TDriver>
{
public:
    TDriver(Py::PythonClassInstance *self, Py::Tuple& args, Py::Dict& kwargs)
        : Py::PythonClass<TDriver>::PythonClass(self, args, kwargs)
        , Id_(TGuid::Create())
        , Logger(NLogging::TLogger(NYT::NPython::Logger)
            .AddTag("DriverId: %v", Id_))
    {
        auto configDict = ExtractArgument(args, kwargs, "config");
        ValidateArgumentsEmpty(args, kwargs);

        try {
            ConfigNode_ = ConvertObjectToNode(configDict);
            UnderlyingDriver_ = CreateDriver(ConfigNode_);
        } catch(const std::exception& ex) {
            throw Py::RuntimeError(Stroka("Error creating driver\n") + ex.what());
        }

        YCHECK(ActiveDrivers.emplace(Id_, UnderlyingDriver_).second);
    }

    ~TDriver()
    {
        UnderlyingDriver_->Terminate();
        ActiveDrivers.erase(Id_);
    }

    static void InitType()
    {
        behaviors().name("Driver");
        behaviors().doc("Represents a YT driver");
        behaviors().supportGetattro();
        behaviors().supportSetattro();

        PYCXX_ADD_KEYWORDS_METHOD(execute, Execute, "Executes the request");
        PYCXX_ADD_KEYWORDS_METHOD(get_command_descriptor, GetCommandDescriptor, "Describes the command");
        PYCXX_ADD_KEYWORDS_METHOD(get_command_descriptors, GetCommandDescriptors, "Describes all commands");
        PYCXX_ADD_KEYWORDS_METHOD(kill_process, KillProcess, "Forces a remote YT process (node, scheduler or master) to exit immediately");
        PYCXX_ADD_KEYWORDS_METHOD(write_core_dump, WriteCoreDump, "Writes a core dump of a remote YT process (node, scheduler or master)");
        PYCXX_ADD_KEYWORDS_METHOD(build_snapshot, BuildSnapshot, "Forces to build a snapshot");
        PYCXX_ADD_KEYWORDS_METHOD(gc_collect, GCCollect, "Runs garbage collection");
        PYCXX_ADD_KEYWORDS_METHOD(clear_metadata_caches, ClearMetadataCaches, "Clears metadata caches");

        PYCXX_ADD_KEYWORDS_METHOD(get_config, GetConfig, "Get config");

        behaviors().readyType();
    }

    Py::Object Execute(Py::Tuple& args, Py::Dict& kwargs)
    {
        LOG_DEBUG("Preparing driver request");

        auto pyRequest = ExtractArgument(args, kwargs, "request");
        ValidateArgumentsEmpty(args, kwargs);

        Py::Callable classType(TDriverResponse::type());
        Py::PythonClassObject<TDriverResponse> pythonResponse(classType.apply(Py::Tuple(), Py::Dict()));
        auto* response = pythonResponse.getCxxObject();

        TDriverRequest request;
        request.CommandName = ConvertStringObjectToStroka(GetAttr(pyRequest, "command_name"));
        request.Parameters = ConvertObjectToNode(GetAttr(pyRequest, "parameters"))->AsMap();
        request.ResponseParametersConsumer = response->GetResponseParametersConsumer();

        auto user = GetAttr(pyRequest, "user");
        if (!user.isNone()) {
            request.AuthenticatedUser = ConvertStringObjectToStroka(user);
        }

        if (pyRequest.hasAttr("id")) {
            auto id = GetAttr(pyRequest, "id");
            if (!id.isNone()) {
                request.Id = Py::ConvertToLongLong(id);
            }
        }

        auto inputStreamObj = GetAttr(pyRequest, "input_stream");
        if (!inputStreamObj.isNone()) {
            std::unique_ptr<TInputStreamWrap> inputStream(new TInputStreamWrap(inputStreamObj));
            request.InputStream = CreateAsyncAdapter(inputStream.get());
            response->OwnInputStream(inputStream);
        }

        auto outputStreamObj = GetAttr(pyRequest, "output_stream");
        TBufferedStreamWrap* bufferedOutputStream = nullptr;
        if (!outputStreamObj.isNone()) {
            bool isBufferedStream = PyObject_IsInstance(outputStreamObj.ptr(), TBufferedStreamWrap::type().ptr());
            if (isBufferedStream) {
                bufferedOutputStream = dynamic_cast<TBufferedStreamWrap*>(Py::getPythonExtensionBase(outputStreamObj.ptr()));
                request.OutputStream = bufferedOutputStream->GetStream();
            } else {
                std::unique_ptr<TOutputStreamWrap> outputStream(new TOutputStreamWrap(outputStreamObj));
                request.OutputStream = CreateAsyncAdapter(outputStream.get());
                response->OwnOutputStream(outputStream);
            }
        }

        try {
            auto driverResponse = UnderlyingDriver_->Execute(request);
            response->SetResponse(driverResponse);
            if (bufferedOutputStream) {
                auto outputStream = bufferedOutputStream->GetStream();
                driverResponse.Subscribe(BIND([=] (TError error) {
                    outputStream->Finish();
                }));
            }
        } CATCH("Driver command execution failed");

        LOG_DEBUG("Request execution started (RequestId: %v, CommandName: %v, User: %v)",
            request.Id,
            request.CommandName,
            request.AuthenticatedUser);

        return pythonResponse;
    }
    PYCXX_KEYWORDS_METHOD_DECL(TDriver, Execute)

    Py::Object GetCommandDescriptor(Py::Tuple& args, Py::Dict& kwargs)
    {
        auto commandName = ConvertStringObjectToStroka(ExtractArgument(args, kwargs, "command_name"));
        ValidateArgumentsEmpty(args, kwargs);

        Py::Callable class_type(TCommandDescriptor::type());
        Py::PythonClassObject<TCommandDescriptor> descriptor(class_type.apply(Py::Tuple(), Py::Dict()));
        try {
            descriptor.getCxxObject()->SetDescriptor(UnderlyingDriver_->GetCommandDescriptor(commandName));
        } CATCH("Failed to get command descriptor");

        return descriptor;
    }
    PYCXX_KEYWORDS_METHOD_DECL(TDriver, GetCommandDescriptor)

    Py::Object GetCommandDescriptors(Py::Tuple& args, Py::Dict& kwargs)
    {
        ValidateArgumentsEmpty(args, kwargs);

        try {
            auto descriptors = Py::Dict();
            for (const auto& nativeDescriptor : UnderlyingDriver_->GetCommandDescriptors()) {
                Py::Callable class_type(TCommandDescriptor::type());
                Py::PythonClassObject<TCommandDescriptor> descriptor(class_type.apply(Py::Tuple(), Py::Dict()));
                descriptor.getCxxObject()->SetDescriptor(nativeDescriptor);
                descriptors.setItem(~nativeDescriptor.CommandName, descriptor);
            }
            return descriptors;
        } CATCH("Failed to get command descriptors");
    }
    PYCXX_KEYWORDS_METHOD_DECL(TDriver, GetCommandDescriptors)

    Py::Object GCCollect(Py::Tuple& args, Py::Dict& kwargs)
    {
        try {
            auto admin = UnderlyingDriver_->GetConnection()->CreateAdmin();
            WaitFor(admin->GCCollect())
                .ThrowOnError();
            return Py::None();
        } CATCH("Failed to perform garbage collect");
    }
    PYCXX_KEYWORDS_METHOD_DECL(TDriver, GCCollect)

    Py::Object KillProcess(Py::Tuple& args, Py::Dict& kwargs)
    {
        auto options = NApi::TKillProcessOptions();

        if (!HasArgument(args, kwargs, "address")) {
            throw CreateYtError("Missing argument 'address'");
        }
        auto address = ConvertStringObjectToStroka(ExtractArgument(args, kwargs, "address"));

        if (HasArgument(args, kwargs, "exit_code")) {
            options.ExitCode = static_cast<int>(Py::Int(ExtractArgument(args, kwargs, "exit_code")));
        }

        ValidateArgumentsEmpty(args, kwargs);

        try {
            auto admin = UnderlyingDriver_->GetConnection()->CreateAdmin();
            WaitFor(admin->KillProcess(address, options))
                .ThrowOnError();
            return Py::None();
        } CATCH("Failed to kill process");
    }
    PYCXX_KEYWORDS_METHOD_DECL(TDriver, KillProcess)

    Py::Object WriteCoreDump(Py::Tuple& args, Py::Dict& kwargs)
    {
        auto options = NApi::TWriteCoreDumpOptions();

        if (!HasArgument(args, kwargs, "address")) {
            throw CreateYtError("Missing argument 'address'");
        }
        auto address = ConvertStringObjectToStroka(ExtractArgument(args, kwargs, "address"));

        ValidateArgumentsEmpty(args, kwargs);

        try {
            auto admin = UnderlyingDriver_->GetConnection()->CreateAdmin();
            auto path = WaitFor(admin->WriteCoreDump(address, options))
                .ValueOrThrow();
            return Py::String(path);
        } CATCH("Failed to write core dump");
    }
    PYCXX_KEYWORDS_METHOD_DECL(TDriver, WriteCoreDump)

    Py::Object BuildSnapshot(Py::Tuple& args, Py::Dict& kwargs)
    {
        auto options = NApi::TBuildSnapshotOptions();

        if (HasArgument(args, kwargs, "set_read_only")) {
            options.SetReadOnly = static_cast<bool>(Py::Boolean(ExtractArgument(args, kwargs, "set_read_only")));
        }

        if (!HasArgument(args, kwargs, "cell_id")) {
            throw CreateYtError("Missing argument 'cell_id'");
        }

        auto cellId = ExtractArgument(args, kwargs, "cell_id");
        if (!cellId.isNone()) {
            options.CellId = TTabletCellId::FromString(ConvertStringObjectToStroka(cellId));
        }

        ValidateArgumentsEmpty(args, kwargs);

        try {
            auto admin = UnderlyingDriver_->GetConnection()->CreateAdmin();
            int snapshotId = WaitFor(admin->BuildSnapshot(options))
                .ValueOrThrow();
            return Py::Long(snapshotId);
        } CATCH("Failed to build snapshot");
    }
    PYCXX_KEYWORDS_METHOD_DECL(TDriver, BuildSnapshot)

    Py::Object ClearMetadataCaches(Py::Tuple& args, Py::Dict& kwargs)
    {
        ValidateArgumentsEmpty(args, kwargs);

        try {
            UnderlyingDriver_->GetConnection()->ClearMetadataCaches();
            return Py::None();
        } CATCH("Failed to clear metadata caches");
    }
    PYCXX_KEYWORDS_METHOD_DECL(TDriver, ClearMetadataCaches)

    Py::Object GetConfig(Py::Tuple& args, Py::Dict& kwargs)
    {
        ValidateArgumentsEmpty(args, kwargs);

        return ConvertTo<Py::Object>(ConfigNode_);
    }
    PYCXX_KEYWORDS_METHOD_DECL(TDriver, GetConfig)

private:
    const TGuid Id_;
    const NLogging::TLogger Logger;

    INodePtr ConfigNode_;
    IDriverPtr UnderlyingDriver_;
};

///////////////////////////////////////////////////////////////////////////////

class TDriverModule
    : public Py::ExtensionModule<TDriverModule>
{
public:
    TDriverModule()
        // This should be the same as .so file name.
        : Py::ExtensionModule<TDriverModule>("driver_lib")
    {
        PyEval_InitThreads();

        RegisterShutdown(BIND([=] () {
            LOG_INFO("Module shutdown started");
            for (const auto& pair : ActiveDrivers) {
                auto driver = pair.second.Lock();
                if (!driver) {
                    continue;
                }
                LOG_INFO("Terminating leaked driver (DriverId: %v)", pair.first);
                driver->Terminate();
            }
            ActiveDrivers.clear();
            LOG_INFO("Module shutdown finished");
        }));

        InstallCrashSignalHandler(std::set<int>({SIGSEGV}));

        TDriver::InitType();
        TBufferedStreamWrap::InitType();
        TDriverResponse::InitType();
        TCommandDescriptor::InitType();

        add_keyword_method("configure_logging", &TDriverModule::ConfigureLogging, "Configures YT driver logging");
        add_keyword_method("configure_tracing", &TDriverModule::ConfigureTracing, "Configures YT driver tracing");

        initialize("Python bindings for YT driver");

        Py::Dict moduleDict(moduleDictionary());
        moduleDict["Driver"] = TDriver::type();
        moduleDict["BufferedStream"] = TBufferedStreamWrap::type();
        moduleDict["Response"] = TDriverResponse::type();
    }

    virtual ~TDriverModule() = default;

    Py::Object ConfigureLogging(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        auto configNode = ConvertObjectToNode(ExtractArgument(args, kwargs, "config"));
        ValidateArgumentsEmpty(args, kwargs);
        auto config = ConvertTo<NLogging::TLogConfigPtr>(configNode);

        NLogging::TLogManager::Get()->Configure(config);

        return Py::None();
    }

    Py::Object ConfigureTracing(const Py::Tuple& args_, const Py::Dict& kwargs_)
    {
        auto args = args_;
        auto kwargs = kwargs_;

        auto configNode = ConvertObjectToNode(ExtractArgument(args, kwargs, "config"));
        ValidateArgumentsEmpty(args, kwargs);
        auto config = ConvertTo<NTracing::TTraceManagerConfigPtr>(configNode);

        NTracing::TTraceManager::Get()->Configure(config);

        return Py::None();
    }
};

///////////////////////////////////////////////////////////////////////////////

} // namespace NPython
} // namespace NYT

///////////////////////////////////////////////////////////////////////////////

#if defined( _WIN32 )
#define EXPORT_SYMBOL __declspec( dllexport )
#else
#define EXPORT_SYMBOL
#endif

static PyObject* init_module()
{
    static auto* driverModule = new NYT::NPython::TDriverModule;
    return driverModule->module().ptr();
}

#if PY_MAJOR_VERSION < 3
extern "C" EXPORT_SYMBOL void initdriver_lib() { Y_UNUSED(init_module()); }
extern "C" EXPORT_SYMBOL void initdriver_lib_d() { initdriver_lib(); }
#else
extern "C" EXPORT_SYMBOL PyObject* PyInit_driver_lib() { return init_module(); }
extern "C" EXPORT_SYMBOL PyObject* PyInit_driver_lib_d() { return PyInit_driver_lib(); }
#endif
