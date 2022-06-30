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

void TTCMallocConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("background_release_rate", &TThis::BackgroundReleaseRate)
        .Default(32_MB);
    registrar.Parameter("max_per_cpu_cache_size", &TThis::MaxPerCpuCacheSize)
        .Default(3_MB);
}

////////////////////////////////////////////////////////////////////////////////

void TSingletonsConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("spin_wait_slow_path_logging_threshold", &TThis::SpinWaitSlowPathLoggingThreshold)
        .Default(TDuration::MicroSeconds(100));
    registrar.Parameter("yt_alloc", &TThis::YTAlloc)
        .DefaultNew();
    registrar.Parameter("fiber_stack_pool_sizes", &TThis::FiberStackPoolSizes)
        .Default({});
    registrar.Parameter("address_resolver", &TThis::AddressResolver)
        .DefaultNew();
    registrar.Parameter("tcp_dispatcher", &TThis::TcpDispatcher)
        .DefaultNew();
    registrar.Parameter("rpc_dispatcher", &TThis::RpcDispatcher)
        .DefaultNew();
    registrar.Parameter("yp_service_discovery", &TThis::YPServiceDiscovery)
        .DefaultNew();
    registrar.Parameter("chunk_client_dispatcher", &TThis::ChunkClientDispatcher)
        .DefaultNew();
    registrar.Parameter("profile_manager", &TThis::ProfileManager)
        .DefaultNew();
    registrar.Parameter("solomon_exporter", &TThis::SolomonExporter)
        .DefaultNew();
    registrar.Parameter("logging", &TThis::Logging)
        .DefaultCtor([] () { return NLogging::TLogManagerConfig::CreateDefault(); });
    registrar.Parameter("jaeger", &TThis::Jaeger)
        .DefaultNew();
    registrar.Parameter("rpc", &TThis::Rpc)
        .DefaultNew();
    registrar.Parameter("tcmalloc", &TThis::TCMalloc)
        .DefaultNew();
    registrar.Parameter("enable_ref_counted_tracker_profiling", &TThis::EnableRefCountedTrackerProfiling)
        .Default(true);
    registrar.Parameter("enable_resource_tracker", &TThis::EnableResourceTracker)
        .Default(true);

    // COMPAT(prime@): backward compatible config for CHYT
    registrar.Postprocessor([] (TThis* config) {
        if (!config->ProfileManager->GlobalTags.empty()) {
            config->SolomonExporter->Host = "";
            config->SolomonExporter->InstanceTags = config->ProfileManager->GlobalTags;
        }
    });
}

////////////////////////////////////////////////////////////////////////////////

void TSingletonsDynamicConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("spin_lock_slow_path_logging_threshold", &TThis::SpinWaitSlowPathLoggingThreshold)
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
    registrar.Parameter("tcmalloc", &TThis::TCMalloc)
        .Optional();
}

////////////////////////////////////////////////////////////////////////////////

void TDiagnosticDumpConfig::Register(TRegistrar registrar)
{
    registrar.Parameter("yt_alloc_dump_period", &TThis::YTAllocDumpPeriod)
        .Default();
    registrar.Parameter("ref_counted_tracker_dump_period", &TThis::RefCountedTrackerDumpPeriod)
        .Default();
}

////////////////////////////////////////////////////////////////////////////////

void WarnForUnrecognizedOptionsImpl(
    const NLogging::TLogger& logger,
    const IMapNodePtr& unrecognized)
{
    const auto& Logger = logger;
    if (unrecognized && unrecognized->GetChildCount() > 0) {
        YT_LOG_WARNING("Bootstrap config contains unrecognized options (Unrecognized: %v)",
            ConvertToYsonString(unrecognized, NYson::EYsonFormat::Text));
    }
}

void WarnForUnrecognizedOptions(
    const NLogging::TLogger& logger,
    const NYTree::TYsonStructPtr& config)
{
    WarnForUnrecognizedOptionsImpl(logger, config->GetRecursiveUnrecognized());
}

void WarnForUnrecognizedOptions(
    const NLogging::TLogger& logger,
    const NYTree::TYsonSerializablePtr& config)
{
    WarnForUnrecognizedOptionsImpl(logger, config->GetUnrecognizedRecursively());
}

void AbortOnUnrecognizedOptionsImpl(
    const NLogging::TLogger& logger,
    const IMapNodePtr& unrecognized)
{
    const auto& Logger = logger;
    if (unrecognized && unrecognized->GetChildCount() > 0) {
        YT_LOG_ERROR("Bootstrap config contains unrecognized options, terminating (Unrecognized: %v)",
            ConvertToYsonString(unrecognized, NYson::EYsonFormat::Text));
        YT_ABORT();
    }
}

void AbortOnUnrecognizedOptions(
    const NLogging::TLogger& logger,
    const NYTree::TYsonStructPtr& config)
{
    AbortOnUnrecognizedOptionsImpl(logger, config->GetRecursiveUnrecognized());
}

void AbortOnUnrecognizedOptions(
    const NLogging::TLogger& logger,
    const NYTree::TYsonSerializablePtr& config)
{
    AbortOnUnrecognizedOptionsImpl(logger, config->GetUnrecognizedRecursively());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

