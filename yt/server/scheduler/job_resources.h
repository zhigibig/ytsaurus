#pragma once

#include "public.h"

#include <ytlib/ytree/public.h>

#include <ytlib/scheduler/scheduler_service.pb.h>

#include <server/job_proxy/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

Stroka FormatResourceUtilization(
    const NProto::TNodeResources& utilization,
    const NProto::TNodeResources& limits);

Stroka FormatResources(
    const NProto::TNodeResources& resources);

void AddResources(
    NProto::TNodeResources* lhs,
    const NProto::TNodeResources& rhs);

void SubtractResources(
    NProto::TNodeResources* lhs,
    const NProto::TNodeResources& rhs);

bool HasEnoughResources(
    const NProto::TNodeResources& currentUtilization,
    const NProto::TNodeResources& requestedUtilization,
    const NProto::TNodeResources& limits);

bool HasSpareResources(
    const NProto::TNodeResources& utilization,
    const NProto::TNodeResources& limits);

void BuildNodeResourcesYson(
    const NProto::TNodeResources& resources,
    NYTree::IYsonConsumer* consumer);

NProto::TNodeResources ZeroResources();
NProto::TNodeResources InfiniteResources();

i64 GetFootprintMemorySize();

i64 GetIOMemorySize(
    NJobProxy::TJobIOConfigPtr ioConfig,
    int inputStreamCount,
    int outputStreamCount);

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
