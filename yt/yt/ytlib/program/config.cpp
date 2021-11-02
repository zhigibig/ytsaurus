#include "config.h"

namespace NYT {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

TRpcConfig::TRpcConfig()
{
    RegisterParameter("tracing", Tracing)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

TSingletonsConfig::TSingletonsConfig()
{
    RegisterParameter("spinlock_hiccup_threshold", SpinlockHiccupThreshold)
        .Default(TDuration::MicroSeconds(100));
    RegisterParameter("yt_alloc", YTAlloc)
        .DefaultNew();
    RegisterParameter("fiber_stack_pool_sizes", FiberStackPoolSizes)
        .Default({});
    RegisterParameter("address_resolver", AddressResolver)
        .DefaultNew();
    RegisterParameter("tcp_dispatcher", TcpDispatcher)
        .DefaultNew();
    RegisterParameter("rpc_dispatcher", RpcDispatcher)
        .DefaultNew();
    RegisterParameter("yp_service_discovery", YPServiceDiscovery)
        .DefaultNew();
    RegisterParameter("chunk_client_dispatcher", ChunkClientDispatcher)
        .DefaultNew();
    RegisterParameter("profile_manager", ProfileManager)
        .DefaultNew();
    RegisterParameter("solomon_exporter", SolomonExporter)
        .DefaultNew();
    RegisterParameter("logging", Logging)
        .Default(NLogging::TLogManagerConfig::CreateDefault());
    RegisterParameter("jaeger", Jaeger)
        .DefaultNew();
    RegisterParameter("rpc", Rpc)
        .DefaultNew();

    // COMPAT(prime@): backward compatible config for CHYT
    RegisterPostprocessor([this] {
        if (!ProfileManager->GlobalTags.empty()) {
            SolomonExporter->Host = "";
            SolomonExporter->InstanceTags = ProfileManager->GlobalTags;
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

TDeprecatedSingletonsDynamicConfig::TDeprecatedSingletonsDynamicConfig()
{
    RegisterParameter("spinlock_hiccup_threshold", SpinlockHiccupThreshold)
        .Optional();
    RegisterParameter("yt_alloc", YTAlloc)
        .Optional();
    RegisterParameter("tcp_dispatcher", TcpDispatcher)
        .DefaultNew();
    RegisterParameter("rpc_dispatcher", RpcDispatcher)
        .DefaultNew();
    RegisterParameter("chunk_client_dispatcher", ChunkClientDispatcher)
        .DefaultNew();
    RegisterParameter("logging", Logging)
        .DefaultNew();
    RegisterParameter("jaeger", Jaeger)
        .DefaultNew();
    RegisterParameter("rpc", Rpc)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

void TSingletonsDynamicConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("spinlock_hiccup_threshold", &TThis::SpinlockHiccupThreshold)
        .Optional();
    registrar.Parameter("yt_alloc", &TThis::YTAlloc)
        .Optional();
    registrar.Parameter("tcp_dispatcher", &TThis::TcpDispatcher)
        .DefaultNew();
    registrar.Parameter("rpc_dispatcher", &TThis::RpcDispatcher)
        .DefaultNew();
    registrar.Parameter("chunk_client_dispatcher", &TThis::ChunkClientDispatcher)
        .DefaultNew();
    registrar.Parameter("logging", &TThis::Logging)
        .DefaultNew();
    registrar.Parameter("jaeger", &TThis::Jaeger)
        .DefaultNew();
    registrar.Parameter("rpc", &TThis::Rpc)
        .DefaultNew();
}

////////////////////////////////////////////////////////////////////////////////

TDiagnosticDumpConfig::TDiagnosticDumpConfig()
{
    RegisterParameter("yt_alloc_dump_period", YTAllocDumpPeriod)
        .Default();
    RegisterParameter("ref_counted_tracker_dump_period", RefCountedTrackerDumpPeriod)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

void WarnForUnrecognizedOptions(
    const NLogging::TLogger& logger,
    const NYTree::TYsonSerializablePtr& config)
{
    const auto& Logger = logger;
    auto unrecognized = config->GetUnrecognizedRecursively();
    if (unrecognized && unrecognized->GetChildCount() > 0) {
        YT_LOG_WARNING("Bootstrap config contains unrecognized options (Unrecognized: %v)",
            ConvertToYsonString(unrecognized, NYson::EYsonFormat::Text));
    }
}

void AbortOnUnrecognizedOptions(
    const NLogging::TLogger& logger,
    const NYTree::TYsonSerializablePtr& config)
{
    const auto& Logger = logger;
    auto unrecognized = config->GetUnrecognizedRecursively();
    if (unrecognized && unrecognized->GetChildCount() > 0) {
        YT_LOG_ERROR("Bootstrap config contains unrecognized options, terminating (Unrecognized: %v)",
            ConvertToYsonString(unrecognized, NYson::EYsonFormat::Text));
        YT_ABORT();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

