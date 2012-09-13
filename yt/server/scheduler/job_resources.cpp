#include "stdafx.h"
#include "job_resources.h"
#include "config.h"

#include <ytlib/ytree/fluent.h>

#include <server/job_proxy/config.h>

namespace NYT {
namespace NScheduler {

using namespace NScheduler::NProto;
using namespace NYTree;
using namespace NJobProxy;

////////////////////////////////////////////////////////////////////

//! Additive term for each job memory usage.
//! Accounts for job proxy process and other lightweight stuff.
static const i64 FootprintMemorySize = (i64) 256 * 1024 * 1024;

//! Nodes having less free memory are considered fully occupied.
static const i64 LowWatermarkMemorySize = (i64) 512 * 1024 * 1024;

////////////////////////////////////////////////////////////////////

Stroka FormatResourceUtilization(
    const TNodeResources& utilization,
    const TNodeResources& limits)
{
    return Sprintf("Slots: %d/%d, Cpu: %d/%d, Memory: %d/%d, Network: %d/%d",
        // Slots
        utilization.slots(),
        limits.slots(),
        // Cpu
        utilization.cpu(),
        limits.cpu(),
        // Memory (in MB)
        static_cast<int>(utilization.memory() / (1024 * 1024)),
        static_cast<int>(limits.memory() / (1024 * 1024)),
        utilization.network(),
        limits.network());
}

Stroka FormatResources(const TNodeResources& resources)
{
    return Sprintf("Slots: %d, Cpu: %d, Memory: %d, Network: %d",
        resources.slots(),
        resources.cpu(),
        static_cast<int>(resources.memory() / (1024 * 1024)),
        resources.network());
}

void AddResources(
    TNodeResources* lhs,
    const TNodeResources& rhs)
{
    lhs->set_slots(lhs->slots() + rhs.slots());
    lhs->set_cpu(lhs->cpu() + rhs.cpu());
    lhs->set_memory(lhs->memory() + rhs.memory());
    lhs->set_network(lhs->network() + rhs.network());
}

void SubtractResources(
    TNodeResources* lhs,
    const TNodeResources& rhs)
{
    lhs->set_slots(lhs->slots() - rhs.slots());
    lhs->set_cpu(lhs->cpu() - rhs.cpu());
    lhs->set_memory(lhs->memory() - rhs.memory());
    lhs->set_network(lhs->network() - rhs.network());
}

bool HasEnoughResources(
    const TNodeResources& currentUtilization,
    const TNodeResources& requestedUtilization,
    const TNodeResources& limits)
{
    return
        currentUtilization.slots() + requestedUtilization.slots() <= limits.slots() &&
        currentUtilization.cpu() + requestedUtilization.cpu() <= limits.cpu() &&
        currentUtilization.memory() + requestedUtilization.memory() <= limits.memory() &&
        currentUtilization.network() + requestedUtilization.network() <= limits.network();
}

bool HasSpareResources(
    const TNodeResources& utilization,
    const TNodeResources& limits)
{
    return
        utilization.slots() < limits.slots() &&
        utilization.cpu() < limits.cpu() &&
        utilization.memory() + LowWatermarkMemorySize < limits.memory();
}

void BuildNodeResourcesYson(
    const TNodeResources& resources,
    IYsonConsumer* consumer)
{
    BuildYsonMapFluently(consumer)
        .Item("slots").Scalar(resources.slots())
        .Item("cpu").Scalar(resources.cpu())
        .Item("memory").Scalar(resources.memory())
        .Item("network").Scalar(resources.network());
}

TNodeResources ZeroResources()
{
    TNodeResources result;
    result.set_slots(0);
    result.set_cpu(0);
    result.set_memory(0);
    result.set_network(0);
    return result;
}

TNodeResources InfiniteResources()
{
    TNodeResources result;
    result.set_slots(1000);
    result.set_cpu(1000);
    result.set_memory((i64) 1024 * 1024 * 1024 * 1024);
    result.set_network(1000);
    return result;
}

i64 GetFootprintMemorySize()
{
    return FootprintMemorySize;
}

i64 GetIOMemorySize(
    TJobIOConfigPtr ioConfig,
    int inputStreamCount,
    int outputStreamCount)
{
    return
        ioConfig->TableReader->WindowSize * ioConfig->TableReader->PrefetchWindow * inputStreamCount +
        (ioConfig->TableWriter->WindowSize + // remote chunk writer window
        ioConfig->TableWriter->EncodeWindowSize + // codec window
        ioConfig->TableWriter->MaxBufferSize) * 
        outputStreamCount * 2; // possibly writing two chunks at the time at chunk change
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

