#include "cluster.h"

#include "account.h"
#include "cluster_reader.h"
#include "helpers.h"
#include "internet_address.h"
#include "ip4_address_pool.h"
#include "label_filter_cache.h"
#include "network_module.h"
#include "node.h"
#include "node_segment.h"
#include "pod.h"
#include "pod_disruption_budget.h"
#include "pod_set.h"
#include "private.h"
#include "resource.h"
#include "topology_zone.h"

#include <yp/server/master/bootstrap.h>

#include <yp/server/objects/object_manager.h>
#include <yp/server/objects/transaction.h>
#include <yp/server/objects/type_info.h>

#include <yt/core/ytree/convert.h>

namespace NYP::NServer::NScheduler {

using namespace NObjects;

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TCluster::TImpl
    : public TRefCounted
{
public:
    explicit TImpl(NMaster::TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
        , Reader_(CreateClusterReader(bootstrap))
    { }

    #define IMPLEMENT_ACCESSORS(name, pluralName) \
        std::vector<T##name*> Get##pluralName() \
        { \
            std::vector<T##name*> result; \
            result.reserve(name##Map_.size()); \
            for (const auto& [id, object] : name##Map_) { \
                result.push_back(object.get()); \
            } \
            return result; \
        } \
        \
        T##name* Find##name(const TObjectId& id) \
        { \
            if (!id) { \
                return nullptr; \
            } \
            auto it = name##Map_.find(id); \
            return it == name##Map_.end() ? nullptr : it->second.get(); \
        } \
        \
        T##name* Get##name##OrThrow(const TObjectId& id) \
        { \
            if (!id) { \
                THROW_ERROR_EXCEPTION("%v id cannot be null", \
                    GetCapitalizedHumanReadableTypeName(EObjectType::name)); \
            } \
            auto* object = Find##name(id); \
            if (!object) { \
                THROW_ERROR_EXCEPTION( \
                    NClient::NApi::EErrorCode::NoSuchObject, \
                    "No such %v %Qv", \
                    GetHumanReadableTypeName(EObjectType::name), \
                    id); \
            } \
            return object; \
        }


    IMPLEMENT_ACCESSORS(Node, Nodes)
    IMPLEMENT_ACCESSORS(NodeSegment, NodeSegments)
    IMPLEMENT_ACCESSORS(PodDisruptionBudget, PodDisruptionBudgets)
    IMPLEMENT_ACCESSORS(PodSet, PodSets)
    IMPLEMENT_ACCESSORS(Pod, Pods)
    IMPLEMENT_ACCESSORS(InternetAddress, InternetAddresses)
    IMPLEMENT_ACCESSORS(IP4AddressPool, IP4AddressPools)
    IMPLEMENT_ACCESSORS(Account, Accounts)
    IMPLEMENT_ACCESSORS(NetworkModule, NetworkModules)
    IMPLEMENT_ACCESSORS(Resource, Resources)

    #undef IMPLEMENT_ACCESSORS

    TTimestamp GetSnapshotTimestamp() const
    {
        return Timestamp_;
    }

    void LoadSnapshot()
    {
        try {
            YT_LOG_INFO("Started loading cluster snapshot");

            PROFILE_TIMING("/cluster_snapshot/time/clear") {
                Clear();
            }

            YT_LOG_INFO("Starting snapshot transaction");

            PROFILE_TIMING("/cluster_snapshot/time/start_transaction") {
                Timestamp_ = Reader_->StartTransaction();
            }

            YT_LOG_INFO("Snapshot transaction started (Timestamp: %llx)",
                Timestamp_);

            PROFILE_TIMING("/cluster_snapshot/time/read_ip4_address_pools") {
                Reader_->ReadIP4AddressPools(
                    [this] (std::unique_ptr<TIP4AddressPool> ip4AddressPool) {
                        RegisterObject(IP4AddressPoolMap_, std::move(ip4AddressPool));
                    });
            }

            PROFILE_TIMING("/cluster_snapshot/time/read_internet_addresses") {
                Reader_->ReadInternetAddresses(
                    [this] (std::unique_ptr<TInternetAddress> internetAddress) {
                        RegisterObject(InternetAddressMap_, std::move(internetAddress));
                    });
            }

            InitializeInternetAddresses();

            PROFILE_TIMING("/cluster_snapshot/time/read_nodes") {
                Reader_->ReadNodes(
                    [this] (std::unique_ptr<TNode> node) {
                        RegisterObject(NodeMap_, std::move(node));
                    });
            }

            InitializeNodeTopologyZones();

            PROFILE_TIMING("/cluster_snapshot/time/read_accounts") {
                Reader_->ReadAccounts(
                    [this] (std::unique_ptr<TAccount> account) {
                        RegisterObject(AccountMap_, std::move(account));
                    });
            }

            InitializeAccountsHierarchy();

            PROFILE_TIMING("/cluster_snapshot/time/read_node_segments") {
                Reader_->ReadNodeSegments(
                    [this] (std::unique_ptr<TNodeSegment> nodeSegment) {
                        RegisterObject(NodeSegmentMap_, std::move(nodeSegment));
                    });
            }

            InitializeNodeSegmentNodes();

            PROFILE_TIMING("/cluster_snapshot/time/read_pod_disruption_budgets") {
                Reader_->ReadPodDisruptionBudgets(
                    [this] (std::unique_ptr<TPodDisruptionBudget> podDisruptionBudget) {
                        RegisterObject(PodDisruptionBudgetMap_, std::move(podDisruptionBudget));
                    });
            }

            PROFILE_TIMING("/cluster_snapshot/time/read_pod_sets") {
                Reader_->ReadPodSets(
                    [this] (std::unique_ptr<TPodSet> podSet) {
                        RegisterObject(PodSetMap_, std::move(podSet));
                    });
            }

            InitializePodSets();

            PROFILE_TIMING("/cluster_snapshot/time/read_pods") {
                Reader_->ReadPods(
                    [this] (std::unique_ptr<TPod> pod) {
                        RegisterObject(PodMap_, std::move(pod));
                    });
            }

            InitializePods();

            PROFILE_TIMING("/cluster_snapshot/time/read_resources") {
                Reader_->ReadResources(
                    [this] (std::unique_ptr<TResource> resource) {
                        RegisterObject(ResourceMap_, std::move(resource));
                    });
            }

            InitializeResources();
            InitializeNodeResources();

            InitializeNodePods();
            InitializePodSetPods();
            InitializeAccountPods();
            InitializeAntiaffinityVacancies();
            InitializeNetworkModules();

            YT_LOG_INFO("Finished loading cluster snapshot (PodCount: %v, NodeCount: %v, NodeSegmentCount: %v)",
                PodMap_.size(),
                NodeMap_.size(),
                NodeSegmentMap_.size());
        } catch (const std::exception& ex) {
            Clear();
            THROW_ERROR_EXCEPTION("Error loading cluster snapshot")
                << ex;
        }
    }

private:
    NMaster::TBootstrap* const Bootstrap_;
    const IClusterReaderPtr Reader_;

    NObjects::TTimestamp Timestamp_ = NObjects::NullTimestamp;
    THashMap<TObjectId, std::unique_ptr<TNode>> NodeMap_;
    THashMap<TObjectId, std::unique_ptr<TPod>> PodMap_;
    THashMap<TObjectId, std::unique_ptr<TPodDisruptionBudget>> PodDisruptionBudgetMap_;
    THashMap<TObjectId, std::unique_ptr<TPodSet>> PodSetMap_;
    THashMap<TObjectId, std::unique_ptr<TNodeSegment>> NodeSegmentMap_;
    THashMap<TObjectId, std::unique_ptr<TAccount>> AccountMap_;
    THashMap<TObjectId, std::unique_ptr<TInternetAddress>> InternetAddressMap_;
    THashMap<TObjectId, std::unique_ptr<TIP4AddressPool>> IP4AddressPoolMap_;
    THashMap<TObjectId, std::unique_ptr<TNetworkModule>> NetworkModuleMap_;
    THashMap<TObjectId, std::unique_ptr<TResource>> ResourceMap_;

    THashMap<std::pair<TString, TString>, std::unique_ptr<TTopologyZone>> TopologyZoneMap_;
    THashMultiMap<TString, TTopologyZone*> TopologyKeyZoneMap_;


    template <class T>
    void RegisterObject(THashMap<TObjectId, std::unique_ptr<T>>& map, std::unique_ptr<T> object)
    {
        // NB! It is crucial to construct this argument in a separate code line
        //     to overcome UB due to unspecified order between
        //     T::GetId call and std::unique_ptr<T> move constructor.
        auto id = object->GetId();
        YT_VERIFY(map.emplace(
            std::move(id),
            std::move(object)).second);
    }


    void InitializeInternetAddresses()
    {
        std::vector<TObjectId> invalidInternetAddressIds;
        for (const auto& [internetAddressId, internetAddress] : InternetAddressMap_) {
            const auto& ip4AddressPoolId = internetAddress->ParentId();
            auto* ip4AddressPool = FindIP4AddressPool(ip4AddressPoolId);
            if (!ip4AddressPool) {
                YT_LOG_WARNING("Internet address refers to an unknown IP4 address pool (InternetAddressId: %v, IP4AddressPoolId: %v)",
                    internetAddressId,
                    ip4AddressPoolId);
                invalidInternetAddressIds.push_back(internetAddressId);
                continue;
            }
        }
        for (const auto& invalidInternetAddressId : invalidInternetAddressIds) {
            YT_VERIFY(InternetAddressMap_.erase(invalidInternetAddressId) > 0);
        }
    }

    void InitializeNodeTopologyZones()
    {
        for (const auto& [nodeId, node] : NodeMap_) {
            auto labelMap = ConvertTo<IMapNodePtr>(node->GetLabels());
            node->TopologyZones() = ParseTopologyZones(nodeId, labelMap);
        }
    }

    void InitializeAccountsHierarchy()
    {
        for (const auto& [accountId, account] : AccountMap_) {
            const auto& parentId = account->ParentId();
            if (!parentId) {
                continue;
            }
            auto* parent = FindAccount(parentId);
            if (!parent) {
                YT_LOG_WARNING("Account refers to an unknown parent (AccountId: %v, ParentId: %v)",
                    account->GetId(),
                    parentId);
                continue;
            }
            account->SetParent(parent);
            YT_VERIFY(parent->Children().insert(account.get()).second);
        }
    }

    void InitializeNodeSegmentNodes()
    {
        auto* nodeTypeHandler = Bootstrap_->GetObjectManager()->GetTypeHandler(EObjectType::Node);

        auto allNodesLabelFilterCache = std::make_unique<TLabelFilterCache<TNode>>(
            nodeTypeHandler,
            GetNodes());

        auto allSchedulableNodesLabelFilterCache = std::make_unique<TLabelFilterCache<TNode>>(
            nodeTypeHandler,
            GetSchedulableNodes());

        std::vector<TObjectId> invalidNodeSegmentIds;
        for (const auto& [nodeSegmentId, nodeSegment] : NodeSegmentMap_) {
            auto nodesOrError = allNodesLabelFilterCache->GetFilteredObjects(
                nodeSegment->NodeFilter());

            auto schedulableNodesOrError = allSchedulableNodesLabelFilterCache->GetFilteredObjects(
                nodeSegment->NodeFilter());

            if (!nodesOrError.IsOK() || !schedulableNodesOrError.IsOK()) {
                YT_LOG_ERROR("Invalid node segment node filter; scheduling for this segment is disabled (NodeSegmentId: %v)",
                    nodeSegmentId);
                invalidNodeSegmentIds.push_back(nodeSegmentId);
                continue;
            }

            auto schedulableNodeLabelFilterCache = std::make_unique<TLabelFilterCache<TNode>>(
                nodeTypeHandler,
                schedulableNodesOrError.Value());

            nodeSegment->Nodes() = std::move(nodesOrError).Value();
            nodeSegment->SchedulableNodes() = std::move(schedulableNodesOrError).Value();
            nodeSegment->SetSchedulableNodeLabelFilterCache(std::move(schedulableNodeLabelFilterCache));
        }
        for (const auto& invalidNodeSegmentId : invalidNodeSegmentIds) {
            YT_VERIFY(NodeSegmentMap_.erase(invalidNodeSegmentId) > 0);
        }
    }

    void InitializePodSets()
    {
        std::vector<TObjectId> invalidPodSetIds;
        for (const auto& [podSetId, podSet] : PodSetMap_) {
            const auto& nodeSegmentId = podSet->NodeSegmentId();
            auto* nodeSegment = FindNodeSegment(nodeSegmentId);
            if (!nodeSegment) {
                YT_LOG_WARNING("Pod set refers to an unknown node segment (PodSetId: %v, NodeSegmentId: %v)",
                    podSetId,
                    nodeSegmentId);
                invalidPodSetIds.push_back(podSetId);
                continue;
            }

            const auto& accountId = podSet->AccountId();
            auto* account = FindAccount(accountId);
            if (!account) {
                YT_LOG_WARNING("Pod set refers to an unknown account (PodSetId: %v, AccountId: %v)",
                    podSetId,
                    accountId);
                invalidPodSetIds.push_back(podSetId);
                continue;
            }

            const auto& podDisruptionBudgetId = podSet->PodDisruptionBudgetId();
            auto* podDisruptionBudget = FindPodDisruptionBudget(podDisruptionBudgetId);
            if (podDisruptionBudgetId && !podDisruptionBudget) {
                YT_LOG_WARNING("Pod set refers to an unknown pod disruption budget (PodSetId: %v, PodDisruptionBudgetId: %v)",
                    podSetId,
                    podDisruptionBudgetId);
                invalidPodSetIds.push_back(podSetId);
                continue;
            }

            podSet->SetNodeSegment(nodeSegment);
            podSet->SetAccount(account);
            podSet->SetPodDisruptionBudget(podDisruptionBudget);
        }
        for (const auto& invalidPodSetId : invalidPodSetIds) {
            YT_VERIFY(PodSetMap_.erase(invalidPodSetId) > 0);
        }
    }

    void InitializePods()
    {
        std::vector<TObjectId> invalidPodIds;
        for (const auto& [podId, pod] : PodMap_) {
            const auto& podSetId = pod->PodSetId();
            auto* podSet = FindPodSet(podSetId);
            if (!podSet) {
                YT_LOG_WARNING("Pod refers to an unknown pod set (PodId: %v, PodSetId: %v)",
                    podId,
                    podSetId);
                invalidPodIds.push_back(podId);
                continue;
            }

            const auto& nodeId = pod->NodeId();
            auto* node = FindNode(nodeId);
            if (nodeId && !node) {
                YT_LOG_WARNING("Pod refers to an unknown node (PodId: %v, NodeId: %v)",
                    podId,
                    nodeId);
                invalidPodIds.push_back(podId);
                continue;
            }

            const auto& accountId = pod->AccountId();
            auto* account = FindAccount(accountId);
            if (accountId && !account) {
                YT_LOG_WARNING("Pod refers to an unknown account (PodId: %v, AccountId: %v)",
                    podId,
                    accountId);
                invalidPodIds.push_back(podId);
                continue;
            }

            pod->SetPodSet(podSet);
            pod->SetNode(node);
            pod->SetAccount(account);
        }
        for (const auto& invalidPodId : invalidPodIds) {
            YT_VERIFY(PodMap_.erase(invalidPodId) > 0);
        }
    }

    void InitializeResources()
    {
        std::vector<TObjectId> invalidResourceIds;
        for (const auto& [resourceId, resource] : ResourceMap_) {
            const auto& nodeId = resource->NodeId();
            auto* node = FindNode(nodeId);
            if (!node) {
                YT_LOG_WARNING("Resource refers to an unknown node (ResourceId: %v, NodeId: %v)",
                    resourceId,
                    nodeId);
                invalidResourceIds.push_back(resourceId);
                continue;
            }

            resource->SetNode(node);
        }
        for (const auto& invalidResourceId : invalidResourceIds) {
            YT_VERIFY(ResourceMap_.erase(invalidResourceId) > 0);
        }
    }

    void InitializeNodeResources()
    {
        for (const auto& [resourceId, resource] : ResourceMap_) {
            auto totalCapacities = GetResourceCapacities(resource->Spec());

            auto aggregateAllocations = [&] (const auto& allocations) {
                THashMap<TStringBuf, TAllocationStatistics> podIdToStatistics;
                for (const auto& allocation : allocations) {
                    auto& statistics = podIdToStatistics[allocation.pod_id()];
                    statistics.Capacities += GetAllocationCapacities(allocation);
                    statistics.Used |= true;
                    statistics.UsedExclusively |= GetAllocationExclusive(allocation);
                }
                return podIdToStatistics;
            };

            auto podIdToScheduledStatistics = aggregateAllocations(resource->ScheduledAllocations());
            auto podIdToActualStatistics = aggregateAllocations(resource->ActualAllocations());

            auto podIdToMaxStatistics = podIdToScheduledStatistics;
            for (const auto& [podId, statistics] : podIdToActualStatistics) {
                auto& current = podIdToMaxStatistics[podId];
                current = Max(current, statistics);
            }

            TAllocationStatistics allocatedStatistics;
            for (const auto& [podId, maxStatistics] : podIdToMaxStatistics) {
                allocatedStatistics += maxStatistics;
            }

            auto* node = resource->GetNode();
            YT_VERIFY(node);

            switch (resource->GetKind()) {
                case EResourceKind::Cpu:
                    node->CpuResource() = THomogeneousResource(
                        totalCapacities,
                        allocatedStatistics.Capacities);
                    break;
                case EResourceKind::Memory:
                    node->MemoryResource() = THomogeneousResource(
                        totalCapacities,
                        allocatedStatistics.Capacities);
                    break;
                case EResourceKind::Slot:
                    node->SlotResource() = THomogeneousResource(
                        totalCapacities,
                        allocatedStatistics.Capacities);
                    break;
                case EResourceKind::Disk: {
                    TDiskVolumePolicyList supportedPolicies;
                    for (auto policy : resource->Spec().disk().supported_policies()) {
                        supportedPolicies.push_back(
                            static_cast<NClient::NApi::NProto::EDiskVolumePolicy>(policy));
                    }
                    node->DiskResources().emplace_back(
                        resource->Spec().disk().storage_class(),
                        supportedPolicies,
                        totalCapacities,
                        allocatedStatistics.Used,
                        allocatedStatistics.UsedExclusively,
                        allocatedStatistics.Capacities);
                    break;
                }
                default:
                    YT_ABORT();
            }
        }
    }

    void InitializeNodePods()
    {
        for (const auto& [podId, pod] : PodMap_) {
            if (pod->GetNode()) {
                YT_VERIFY(pod->GetNode()->Pods().insert(pod.get()).second);
            }
        }
    }

    void InitializePodSetPods()
    {
        for (const auto& [podId, pod] : PodMap_) {
            auto* podSet = pod->GetPodSet();
            YT_VERIFY(podSet->Pods().insert(pod.get()).second);
        }
    }

    void InitializeAccountPods()
    {
        for (const auto& [podId, pod] : PodMap_) {
            YT_VERIFY(pod->GetEffectiveAccount()->Pods().insert(pod.get()).second);
        }
    }

    void InitializeAntiaffinityVacancies()
    {
        for (const auto& [podId, pod] : PodMap_) {
            auto* node = pod->GetNode();
            if (node) {
                node->AcquireAntiaffinityVacancies(pod.get());
            }
        }
    }

    void InitializeNetworkModules()
    {
        for (const auto& [internetAddressId, internetAddress] : InternetAddressMap_) {
            const auto& networkModuleId = internetAddress->Spec().network_module_id();
            auto* networkModule = GetOrCreateNetworkModule(networkModuleId);
            ++networkModule->InternetAddressCount();
            if (internetAddress->Status().has_pod_id()) {
                ++networkModule->AllocatedInternetAddressCount();
            }
        }
    }

    std::vector<TNode*> GetSchedulableNodes()
    {
        std::vector<TNode*> result;
        result.reserve(NodeMap_.size());
        for (const auto& [nodeId, node] : NodeMap_) {
            if (node->IsSchedulable()) {
                result.push_back(node.get());
            }
        }
        return result;
    }


    TTopologyZone* GetOrCreateTopologyZone(const TString& key, const TString& value)
    {
        auto pair = std::make_pair(key, value);
        auto it = TopologyZoneMap_.find(pair);
        if (it == TopologyZoneMap_.end()) {
            auto zone = std::make_unique<TTopologyZone>(key, value);
            TopologyKeyZoneMap_.emplace(key, zone.get());
            it = TopologyZoneMap_.emplace(pair, std::move(zone)).first;
        }
        return it->second.get();
    }

    std::vector<TTopologyZone*> ParseTopologyZones(const TObjectId& nodeId, const IMapNodePtr& labelMap)
    {
        auto topologyNode = labelMap->FindChild(TopologyLabel);
        if (!topologyNode) {
            return {};
        }

        if (topologyNode->GetType() != ENodeType::Map) {
            YT_LOG_WARNING("Invalid %Qv label: expected %Qlv, got %Qlv (NodeId: %v)",
                topologyNode->GetPath(),
                ENodeType::Map,
                topologyNode->GetType(),
                nodeId);
            return {};
        }

        auto topologyMap = topologyNode->AsMap();
        std::vector<TTopologyZone*> zones;
        zones.reserve(topologyMap->GetChildCount());
        for (const auto& [key, valueNode] : topologyMap->GetChildren()) {
            if (valueNode->GetType() != ENodeType::String) {
                YT_LOG_WARNING("Invalid %Qv label: expected %Qlv, got %Qlv (NodeId: %v)",
                    valueNode->GetPath(),
                    ENodeType::String,
                    valueNode->GetType(),
                    nodeId);
                continue;
            }

            const auto& value = valueNode->GetValue<TString>();
            auto* zone = GetOrCreateTopologyZone(key, value);
            zones.push_back(zone);
        }
        return zones;
    }


    TNetworkModule* GetOrCreateNetworkModule(const TObjectId& id)
    {
        if (!id) {
            THROW_ERROR_EXCEPTION("Network module id cannot be null");
        }
        auto it = NetworkModuleMap_.find(id);
        if (it == NetworkModuleMap_.end()) {
            it = NetworkModuleMap_.emplace(id, std::make_unique<TNetworkModule>()).first;
        }
        return it->second.get();
    }


    void Clear()
    {
        NodeMap_.clear();
        PodMap_.clear();
        PodDisruptionBudgetMap_.clear();
        PodSetMap_.clear();
        AccountMap_.clear();
        InternetAddressMap_.clear();
        IP4AddressPoolMap_.clear();
        NetworkModuleMap_.clear();
        TopologyZoneMap_.clear();
        TopologyKeyZoneMap_.clear();
        NodeSegmentMap_.clear();
        ResourceMap_.clear();
        Timestamp_ = NullTimestamp;
    }
};

////////////////////////////////////////////////////////////////////////////////

TCluster::TCluster(NMaster::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(bootstrap))
{ }

std::vector<TNode*> TCluster::GetNodes()
{
    return Impl_->GetNodes();
}

TNode* TCluster::FindNode(const TObjectId& id)
{
    return Impl_->FindNode(id);
}

TNode* TCluster::GetNodeOrThrow(const TObjectId& id)
{
    return Impl_->GetNodeOrThrow(id);
}

std::vector<TResource*> TCluster::GetResources()
{
    return Impl_->GetResources();
}

TResource* TCluster::FindResource(const TObjectId& id)
{
    return Impl_->FindResource(id);
}

TResource* TCluster::GetResourceOrThrow(const TObjectId& id)
{
    return Impl_->GetResourceOrThrow(id);
}

std::vector<TPod*> TCluster::GetPods()
{
    return Impl_->GetPods();
}

TPod* TCluster::FindPod(const TObjectId& id)
{
    return Impl_->FindPod(id);
}

TPod* TCluster::GetPodOrThrow(const TObjectId& id)
{
    return Impl_->GetPodOrThrow(id);
}

std::vector<TNodeSegment*> TCluster::GetNodeSegments()
{
    return Impl_->GetNodeSegments();
}

TNodeSegment* TCluster::FindNodeSegment(const TObjectId& id)
{
    return Impl_->FindNodeSegment(id);
}

TNodeSegment* TCluster::GetNodeSegmentOrThrow(const TObjectId& id)
{
    return Impl_->GetNodeSegmentOrThrow(id);
}

std::vector<TInternetAddress*> TCluster::GetInternetAddresses()
{
    return Impl_->GetInternetAddresses();
}

std::vector<TIP4AddressPool*> TCluster::GetIP4AddressPools()
{
    return Impl_->GetIP4AddressPools();
}

std::vector<TAccount*> TCluster::GetAccounts()
{
    return Impl_->GetAccounts();
}

TNetworkModule* TCluster::FindNetworkModule(const TObjectId& id)
{
    return Impl_->FindNetworkModule(id);
}

std::vector<TPodSet*> TCluster::GetPodSets()
{
    return Impl_->GetPodSets();
}

std::vector<TPodDisruptionBudget*> TCluster::GetPodDisruptionBudgets()
{
    return Impl_->GetPodDisruptionBudgets();
}

TTimestamp TCluster::GetSnapshotTimestamp() const
{
    return Impl_->GetSnapshotTimestamp();
}

void TCluster::LoadSnapshot()
{
    Impl_->LoadSnapshot();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NScheduler
