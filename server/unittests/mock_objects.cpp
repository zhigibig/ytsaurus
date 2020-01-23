#include "mock_objects.h"

#include <yp/server/objects/helpers.h>

#include <yp/server/lib/cluster/resource_capacities.h>

namespace NYP::NServer::NCluster::NTests {

namespace {

////////////////////////////////////////////////////////////////////////////////

TObjectId GenerateUniqueId()
{
    static int lastObjectIndex = 0;
    return "mock_object_" + ToString(lastObjectIndex++);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<TPod> CreateMockPod(ui64 cpuCapacity, ui64 memoryCapacity)
{
    auto uuid = NObjects::GenerateUuid();

    NObjects::TPodResourceRequests resourceRequests;
    resourceRequests.set_vcpu_guarantee(cpuCapacity);
    resourceRequests.set_memory_limit(memoryCapacity);

    return std::make_unique<TPod>(
        GenerateUniqueId(),
        /* labels */ NYT::NYson::TYsonString(),
        /* podSetId */ TObjectId(),
        /* nodeId */ TObjectId(),
        /* accountId */ TObjectId(),
        std::move(uuid),
        std::move(resourceRequests),
        NObjects::TPodDiskVolumeRequests(),
        NObjects::TPodGpuRequests(),
        NObjects::TPodIP6AddressRequests(),
        NObjects::TPodIP6SubnetRequests(),
        /* node filter */ TString(),
        /* enable scheduling */ true,
        NClient::NApi::NProto::TPodStatus_TEviction(),
        NYT::NProto::TError(),
        NObjects::TNodeAlerts(),
        NClient::NApi::NProto::TPodStatus_TMaintenance());
}

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<TNode> CreateMockNode(
    THomogeneousResource cpuResource,
    THomogeneousResource memoryResource)
{
    auto node = std::make_unique<TNode>(
        GenerateUniqueId(),
        /* labels */ NYT::NYson::TYsonString(),
        NServer::NObjects::EHfsmState::Unknown,
        /* hasUnknownPods */ false,
        NObjects::TNodeAlerts(),
        NClient::NApi::NProto::TNodeStatus_TMaintenance(),
        NClient::NApi::NProto::TNodeSpec());

    node->CpuResource() = cpuResource;
    node->MemoryResource() = memoryResource;

    return node;
}

std::unique_ptr<TNode> CreateMockNode()
{
    return CreateMockNode(
        THomogeneousResource(
            /* total */ MakeCpuCapacities(1000),
            /* allocated */ MakeCpuCapacities(0)),
        THomogeneousResource(
            /* total */ MakeMemoryCapacities(1024 * 1024),
            /* allocated */ MakeMemoryCapacities(0)));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NCluster::NTests
