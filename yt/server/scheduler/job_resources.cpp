#include "job_resources.h"

#include <yt/core/ytree/fluent.h>

#include <yt/ytlib/node_tracker_client/helpers.h>

namespace NYT {
namespace NScheduler {

using namespace NYson;
using namespace NYTree;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NProfiling;

////////////////////////////////////////////////////////////////////

//! Nodes having less free memory are considered fully occupied,
//! thus no scheduling attempts will be made.
static const i64 LowWatermarkMemorySize = (i64) 256 * 1024 * 1024;

////////////////////////////////////////////////////////////////////

TExtendedJobResources::TExtendedJobResources()
    : UserSlots_(0)
    , Cpu_(0)
    , JobProxyMemory_(0)
    , UserJobMemory_(0)
    , FootprintMemory_(0)
    , Network_(0)
{ }

i64 TExtendedJobResources::GetMemory() const
{
    return JobProxyMemory_ + UserJobMemory_ + FootprintMemory_;
}

void Serialize(const TExtendedJobResources& resources, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("cpu").Value(resources.GetCpu())
            .Item("user_slots").Value(resources.GetUserSlots())
            .Item("job_proxy_memory").Value(resources.GetJobProxyMemory())
            .Item("user_job_memory").Value(resources.GetUserJobMemory())
            .Item("footprint_memory").Value(resources.GetFootprintMemory())
            .Item("network").Value(resources.GetNetwork())
        .EndMap();
}

void TExtendedJobResources::Persist(TStreamPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Cpu_);
    Persist(context, UserSlots_);
    Persist(context, JobProxyMemory_);
    Persist(context, UserJobMemory_);
    Persist(context, FootprintMemory_);
    Persist(context, Network_);
}

TJobResources::TJobResources()
    : TEmptyJobResourcesBase()
#define XX(name, Name) , Name##_(0)
ITERATE_JOB_RESOURCES(XX)
#undef XX
{ }

TJobResources::TJobResources(const TNodeResources& resources)
    : TEmptyJobResourcesBase()
#define XX(name, Name) , Name##_(resources.name())
ITERATE_JOB_RESOURCES(XX)
#undef XX
{ }

TNodeResources TJobResources::ToNodeResources() const
{
    TNodeResources result;
    #define XX(name, Name) result.set_##name(Name##_);
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

void TJobResources::Persist(TStreamPersistenceContext& context)
{
    using NYT::Persist;

    #define XX(name, Name) Persist(context, Name##_);
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
}

Stroka FormatResourceUsage(
    const TJobResources& usage,
    const TJobResources& limits)
{
    return Format(
        "UserSlots: %v/%v, Cpu: %v/%v, Memory: %v/%v, Network: %v/%v",
        // User slots
        usage.GetUserSlots(),
        limits.GetUserSlots(),
        // Cpu
        usage.GetCpu(),
        limits.GetCpu(),
        // Memory (in MB)
        usage.GetMemory() / (1024 * 1024),
        limits.GetMemory() / (1024 * 1024),
        // Network
        usage.GetNetwork(),
        limits.GetNetwork());
}

Stroka FormatResources(const TJobResources& resources)
{
    return Format(
        "{UserSlots: %v, Cpu: %v, Memory: %v, Network: %v}",
        resources.GetUserSlots(),
        resources.GetCpu(),
        resources.GetMemory() / (1024 * 1024),
        resources.GetNetwork());
}

Stroka FormatResources(const TExtendedJobResources& resources)
{
    return Format(
        "{UserSlots: %v, Cpu: %v, JobProxyMemory: %v, UserJobMemory: %v, FootprintMemory: %v, Network: %v}",
        resources.GetUserSlots(),
        resources.GetCpu(),
        resources.GetJobProxyMemory() / (1024 * 1024),
        resources.GetUserJobMemory() / (1024 * 1024),
        resources.GetFootprintMemory() / (1024 * 1024),
        resources.GetNetwork());
}

void ProfileResources(TProfiler& profiler, const TJobResources& resources)
{
    #define XX(name, Name) profiler.Enqueue("/" #name, resources.Get##Name(), EMetricType::Gauge);
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
}

EResourceType GetDominantResource(
    const TJobResources& demand,
    const TJobResources& limits)
{
    auto maxType = EResourceType::Cpu;
    double maxRatio = 0.0;
    auto update = [&] (i64 a, i64 b, EResourceType type) {
        if (b > 0) {
            double ratio = (double) a / b;
            if (ratio > maxRatio) {
                maxRatio = ratio;
                maxType = type;
            }
        }
    };
    #define XX(name, Name) update(demand.Get##Name(), limits.Get##Name(), EResourceType::Name);
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return maxType;
}

i64 GetResource(const TJobResources& resources, EResourceType type)
{
    switch (type) {
        #define XX(name, Name) case EResourceType::Name: return resources.Get##Name();
        ITERATE_JOB_RESOURCES(XX)
        #undef XX
        default:
            Y_UNREACHABLE();
    }
}

void SetResource(TJobResources& resources, EResourceType type, i64 value)
{
    switch (type) {
        #define XX(name, Name) \
            case EResourceType::Name: \
            resources.Set##Name(static_cast<decltype(resources.Get##Name())>(value)); break;
        ITERATE_JOB_RESOURCES(XX)
        #undef XX
        default:
            Y_UNREACHABLE();
    }
}

double GetMinResourceRatio(
    const TJobResources& nominator,
    const TJobResources& denominator)
{
    double result = std::numeric_limits<double>::infinity();
    auto update = [&] (i64 a, i64 b) {
        if (b > 0) {
            result = std::min(result, (double) a / b);
        }
    };
    #define XX(name, Name) update(nominator.Get##Name(), denominator.Get##Name());
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

double GetMaxResourceRatio(
    const TJobResources& nominator,
    const TJobResources& denominator)
{
    double result = 0.0;
    auto update = [&] (i64 a, i64 b) {
        if (b > 0) {
            result = std::max(result, (double) a / b);
        }
    };
    #define XX(name, Name) update(nominator.Get##Name(), denominator.Get##Name());
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

TJobResources GetAdjustedResourceLimits(
    const TJobResources& demand,
    const TJobResources& limits,
    int nodeCount)
{
    auto adjustedLimits = limits;

    // Take memory granularity into account.
    if (demand.GetUserSlots() > 0 && nodeCount > 0) {
        i64 memoryDemandPerJob = demand.GetMemory() / demand.GetUserSlots();
        i64 memoryLimitPerNode = limits.GetMemory() / nodeCount;
        if (memoryDemandPerJob > 0) {
            int slotsPerNode = memoryLimitPerNode / memoryDemandPerJob;
            i64 adjustedMemoryLimit = slotsPerNode * memoryDemandPerJob * nodeCount;
            adjustedLimits.SetMemory(adjustedMemoryLimit);
        }
    }

    return adjustedLimits;
}

const TJobResources& ZeroJobResources()
{
    static auto value = TJobResources();
    return value;
}

TJobResources GetInfiniteResources()
{
    TJobResources result;
    #define XX(name, Name) result.Set##Name(std::numeric_limits<decltype(result.Get##Name())>::max() / 4);
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

const TJobResources& InfiniteJobResources()
{
    static auto result = GetInfiniteResources();
    return result;
}

TJobResources operator + (const TJobResources& lhs, const TJobResources& rhs)
{
    TJobResources result;
    #define XX(name, Name) result.Set##Name(lhs.Get##Name() + rhs.Get##Name());
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

TJobResources& operator += (TJobResources& lhs, const TJobResources& rhs)
{
    #define XX(name, Name) lhs.Set##Name(lhs.Get##Name() + rhs.Get##Name());
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return lhs;
}

TJobResources operator - (const TJobResources& lhs, const TJobResources& rhs)
{
    TJobResources result;
    #define XX(name, Name) result.Set##Name(lhs.Get##Name() - rhs.Get##Name());
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

TJobResources& operator -= (TJobResources& lhs, const TJobResources& rhs)
{
    #define XX(name, Name) lhs.Set##Name(lhs.Get##Name() - rhs.Get##Name());
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return lhs;
}

TJobResources operator * (const TJobResources& lhs, i64 rhs)
{
    TJobResources result;
    #define XX(name, Name) result.Set##Name(lhs.Get##Name() * rhs);
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

TJobResources operator * (const TJobResources& lhs, double rhs)
{
    TJobResources result;
    #define XX(name, Name) result.Set##Name(static_cast<decltype(lhs.Get##Name())>(lhs.Get##Name() * rhs + 0.5));
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

TJobResources& operator *= (TJobResources& lhs, i64 rhs)
{
    #define XX(name, Name) lhs.Set##Name(lhs.Get##Name() * rhs);
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return lhs;
}

TJobResources& operator *= (TJobResources& lhs, double rhs)
{
    #define XX(name, Name) lhs.Set##Name(static_cast<decltype(lhs.Get##Name())>(lhs.Get##Name() * rhs + 0.5));
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return lhs;
}

TJobResources  operator - (const TJobResources& resources)
{
    TJobResources result;
    #define XX(name, Name) result.Set##Name(-resources.Get##Name());
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

bool operator == (const TJobResources& lhs, const TJobResources& rhs)
{
    return
    #define XX(name, Name) lhs.Get##Name() == rhs.Get##Name() &&
        ITERATE_JOB_RESOURCES(XX)
    #undef XX
    true;
}

bool operator != (const TJobResources& lhs, const TJobResources& rhs)
{
    return !(lhs == rhs);
}

bool Dominates(const TJobResources& lhs, const TJobResources& rhs)
{
    return
    #define XX(name, Name) lhs.Get##Name() >= rhs.Get##Name() &&
        ITERATE_JOB_RESOURCES(XX)
    #undef XX
    true;
}

TJobResources Max(const TJobResources& a, const TJobResources& b)
{
    TJobResources result;
    #define XX(name, Name) result.Set##Name(std::max(a.Get##Name(), b.Get##Name()));
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

TJobResources Min(const TJobResources& a, const TJobResources& b)
{
    TJobResources result;
    #define XX(name, Name) result.Set##Name(std::min(a.Get##Name(), b.Get##Name()));
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
    return result;
}

void Serialize(const TJobResources& resources, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
    #define XX(name, Name) .Item(#name).Value(resources.Get##Name())
    ITERATE_JOB_RESOURCES(XX)
    #undef XX
        .EndMap();
}

TJobResources GetMinSpareResources()
{
    TJobResources result;
    result.SetUserSlots(1);
    result.SetCpu(1);
    result.SetMemory(LowWatermarkMemorySize);
    return result;
}

const TJobResources& MinSpareNodeResources()
{
    static auto result = GetMinSpareResources();
    return result;
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

