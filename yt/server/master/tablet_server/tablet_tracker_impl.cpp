#include "bundle_node_tracker.h"
#include "config.h"
#include "private.h"
#include "tablet_cell.h"
#include "tablet_cell_bundle.h"
#include "tablet_manager.h"
#include "tablet_tracker_impl.h"

#include <yt/server/master/cell_master/bootstrap.h>
#include <yt/server/master/cell_master/config_manager.h>
#include <yt/server/master/cell_master/config.h>
#include <yt/server/master/cell_master/hydra_facade.h>

#include <yt/server/master/node_tracker_server/config.h>
#include <yt/server/master/node_tracker_server/node.h>
#include <yt/server/master/node_tracker_server/node_tracker.h>

#include <yt/server/master/table_server/table_node.h>

#include <yt/client/object_client/helpers.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/misc/numeric_helpers.h>

#include <yt/core/profiling/profile_manager.h>

namespace NYT::NTabletServer {

using namespace NCellMaster;
using namespace NConcurrency;
using namespace NObjectServer;
using namespace NTabletServer::NProto;
using namespace NNodeTrackerServer;
using namespace NHydra;
using namespace NHiveServer;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletServerLogger;

////////////////////////////////////////////////////////////////////////////////

class TTabletCellBalancerProvider
    : public ITabletCellBalancerProvider
{
public:
    explicit TTabletCellBalancerProvider(const TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
        , BalanceRequestTime_(Now())
    {
        const auto& bundleNodeTracker = Bootstrap_->GetTabletManager()->GetBundleNodeTracker();
        bundleNodeTracker->SubscribeBundleNodesChanged(BIND(&TTabletCellBalancerProvider::OnBundleNodesChanged, MakeWeak(this)));
    }

    virtual std::vector<TNodeHolder> GetNodes() override
    {
        BalanceRequestTime_.reset();

        const auto& nodeTracker = Bootstrap_->GetNodeTracker();
        const auto& tabletManager = Bootstrap_->GetTabletManager();

        auto isGood = [&] (const auto* node) {
            return CheckIfNodeCanHostTabletCells(node) && node->GetTotalTabletSlots() > 0;
        };

        int nodeCount = 0;
        for (const auto& pair : nodeTracker->Nodes()) {
            if (isGood(pair.second)) {
                ++nodeCount;
            }
        }

        std::vector<TNodeHolder> nodes;
        nodes.reserve(nodeCount);

        for (const auto& pair : nodeTracker->Nodes()) {
            const auto* node = pair.second;
            if (!isGood(node)) {
                continue;
            }

            const auto* cells = tabletManager->FindAssignedTabletCells(node->GetDefaultAddress());
            nodes.emplace_back(
                node,
                node->GetTotalTabletSlots(),
                cells ? *cells : TTabletCellSet());
        }

        return nodes;
    }

    virtual const TReadOnlyEntityMap<TTabletCellBundle>& TabletCellBundles() override
    {
        return Bootstrap_->GetTabletManager()->TabletCellBundles();
    }

    virtual bool IsPossibleHost(const TNode* node, const TTabletCellBundle* bundle) override
    {
        const auto& bundleNodeTracker = Bootstrap_->GetTabletManager()->GetBundleNodeTracker();
        return bundleNodeTracker->GetBundleNodes(bundle).contains(node);
    }

    virtual bool IsVerboseLoggingEnabled() override
    {
        return Bootstrap_->GetConfigManager()->GetConfig()
            ->TabletManager->TabletCellBalancer->EnableVerboseLogging;
    }

    virtual bool IsBalancingRequired() override
    {
        if (!GetConfig()->EnableTabletCellSmoothing) {
            return false;
        }

        auto waitTime = GetConfig()->RebalanceWaitTime;

        if (BalanceRequestTime_ && *BalanceRequestTime_ + waitTime < Now()) {
            BalanceRequestTime_.reset();
            return true;
        }

        return false;
    }

private:
    const TBootstrap* Bootstrap_;
    std::optional<TInstant> BalanceRequestTime_;

    void OnBundleNodesChanged(const TTabletCellBundle* /*bundle*/)
    {
        if (!BalanceRequestTime_) {
            BalanceRequestTime_ = Now();
        }
    }

    const TDynamicTabletCellBalancerMasterConfigPtr& GetConfig()
    {
        return Bootstrap_->GetConfigManager()->GetConfig()
            ->TabletManager->TabletCellBalancer;
    }
};

////////////////////////////////////////////////////////////////////////////////

TTabletTrackerImpl::TTabletTrackerImpl(
    NCellMaster::TBootstrap* bootstrap,
    TInstant startTime)
    : Bootstrap_(bootstrap)
    , StartTime_(startTime)
    , TTabletCellBalancerProvider_(New<TTabletCellBalancerProvider>(Bootstrap_))
    , Profiler("/tablet_server/tablet_tracker")
{
    YT_VERIFY(Bootstrap_);
    VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(NCellMaster::EAutomatonThreadQueue::Default), AutomatonThread);

    const auto& tabletManager = Bootstrap_->GetTabletManager();
    tabletManager->SubscribeTabletCellPeersAssigned(BIND(&TTabletTrackerImpl::OnTabletCellPeersReassigned, MakeWeak(this)));
}

void TTabletTrackerImpl::ScanCells()
{
    VERIFY_THREAD_AFFINITY(AutomatonThread);

    if (WaitForCommit_) {
        return;
    }

    TBundleCounter leaderReassignmentCounter, peerRevocationCounter, peerAssignmentCounter;

    auto balancer = CreateTabletCellBalancer(TTabletCellBalancerProvider_);

    const auto& hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    const auto& tabletManger = Bootstrap_->GetTabletManager();
    for (const auto& pair : tabletManger->TabletCells()) {
        auto* cell = pair.second;
        if (!IsObjectAlive(cell))
            continue;

        ScheduleLeaderReassignment(cell, &leaderReassignmentCounter);
        SchedulePeerAssignment(cell, balancer.get(), &peerAssignmentCounter);
        SchedulePeerRevocation(cell, balancer.get(), &peerRevocationCounter);
    }

    auto moveDescriptors = balancer->GetTabletCellMoveDescriptors();
    Profile(moveDescriptors, leaderReassignmentCounter, peerRevocationCounter, peerAssignmentCounter);

    TReqReassignPeers request;

    {
        TReqRevokePeers* revocation;
        const TTabletCell* cell = nullptr;

        for (const auto& moveDescriptor : moveDescriptors) {
            const auto* source = moveDescriptor.Source;
            const auto* target = moveDescriptor.Target;

            if (source || !target) {
                if (moveDescriptor.Cell != cell) {
                    cell = moveDescriptor.Cell;
                    revocation = request.add_revocations();
                    ToProto(revocation->mutable_cell_id(), cell->GetId());
                }

                if (!target && IsDecommissioned(source, cell->GetCellBundle()->NodeTagFilter())) {
                    continue;
                }

                revocation->add_peer_ids(moveDescriptor.PeerId);
            }
        }
    }

    {
        TReqAssignPeers* assignment;
        const TTabletCell* cell = nullptr;

        for (const auto& moveDescriptor : moveDescriptors) {
            if (moveDescriptor.Target) {
                if (moveDescriptor.Cell != cell) {
                    cell = moveDescriptor.Cell;
                    assignment = request.add_assignments();
                    ToProto(assignment->mutable_cell_id(), cell->GetId());
                }

                auto* peerInfo = assignment->add_peer_infos();
                peerInfo->set_peer_id(moveDescriptor.PeerId);
                ToProto(peerInfo->mutable_node_descriptor(), moveDescriptor.Target->GetDescriptor());
            }
        }
    }

    WaitForCommit_ = true;

    CreateMutation(hydraManager, request)
        ->CommitAndLog(Logger);
}

void TTabletTrackerImpl::OnTabletCellPeersReassigned()
{
    WaitForCommit_ = false;
}

const TDynamicTabletManagerConfigPtr& TTabletTrackerImpl::GetDynamicConfig()
{
    return Bootstrap_->GetConfigManager()->GetConfig()->TabletManager;
}

void TTabletTrackerImpl::Profile(
    const std::vector<TTabletCellMoveDescriptor>& moveDescriptors,
    const TBundleCounter& leaderReassignmentCounter,
    const TBundleCounter& peerRevocationCounter,
    const TBundleCounter& peerAssignmentCounter)
{
    TBundleCounter moveCounts;

    for (const auto& moveDescriptor : moveDescriptors) {
        moveCounts[NProfiling::TTagIdList{moveDescriptor.Cell->GetCellBundle()->GetProfilingTag()}]++;
    }

    for (const auto& [tags, count] : moveCounts) {
        Profiler.Enqueue(
            "/tablet_cell_moves",
            count,
            NProfiling::EMetricType::Gauge,
            tags);
    }

    for (const auto& [tags, count] : leaderReassignmentCounter) {
        Profiler.Enqueue(
            "/leader_reassignment",
            count,
            NProfiling::EMetricType::Gauge,
            tags);
    }

    for (const auto& [tags, count] : peerRevocationCounter) {
        Profiler.Enqueue(
            "/peer_revocation",
            count,
            NProfiling::EMetricType::Gauge,
            tags);
    }

    for (const auto& [tags, count] : peerAssignmentCounter) {
        Profiler.Enqueue(
            "/peer_assignment",
            count,
            NProfiling::EMetricType::Gauge,
            tags);
    }
}

void TTabletTrackerImpl::ScheduleLeaderReassignment(TTabletCell* cell, TBundleCounter* counter)
{
    // Try to move the leader to a good peer.
    const auto& leadingPeer = cell->Peers()[cell->GetLeadingPeerId()];
    TError error;

    if (!leadingPeer.Descriptor.IsNull()) {
        error = IsFailed(leadingPeer, cell->GetCellBundle()->NodeTagFilter(), GetDynamicConfig()->LeaderReassignmentTimeout);
        if (error.IsOK()) {
            return;
        }
    }

    auto goodPeerId = FindGoodPeer(cell);
    if (goodPeerId == InvalidPeerId)
        return;

    YT_LOG_DEBUG(error, "Schedule leader reassignment (CellId: %v, PeerId: %v, Address: %v)",
        cell->GetId(),
        cell->GetLeadingPeerId(),
        leadingPeer.Descriptor.GetDefaultAddress());

    TReqSetLeadingPeer request;
    ToProto(request.mutable_cell_id(), cell->GetId());
    request.set_peer_id(goodPeerId);

    NProfiling::TTagIdList tagIds{
        cell->GetCellBundle()->GetProfilingTag(),
        NProfiling::TProfileManager::Get()->RegisterTag("reason", error.GetMessage())
    };

    (*counter)[tagIds]++;

    const auto& hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
    CreateMutation(hydraManager, request)
        ->CommitAndLog(Logger);
}

void TTabletTrackerImpl::SchedulePeerAssignment(TTabletCell* cell, ITabletCellBalancer* balancer, TBundleCounter* counter)
{
    const auto& peers = cell->Peers();

    // Don't assign new peers if there's a follower but no leader.
    // Try to promote the follower first.
    bool hasFollower = false;
    bool hasLeader = false;
    for (const auto& peer : peers) {
        auto* node = peer.Node;
        if (!node) {
            continue;
        }

        auto* slot = node->FindTabletSlot(cell);
        if (!slot) {
            continue;
        }

        auto state = slot->PeerState;
        if (state == EPeerState::Leading || state == EPeerState::LeaderRecovery) {
            hasLeader = true;
        }
        if (state == EPeerState::Following || state == EPeerState::FollowerRecovery) {
            hasFollower = true;
        }
    }

    if (hasFollower && !hasLeader) {
        return;
    }

    int assignCount = 0;

    // Try to assign missing peers.
    for (TPeerId id = 0; id < static_cast<int>(cell->Peers().size()); ++id) {
        if (peers[id].Descriptor.IsNull()) {
            ++assignCount;
            balancer->AssignPeer(cell, id);
        }
    }

    (*counter)[NProfiling::TTagIdList{cell->GetCellBundle()->GetProfilingTag()}] += assignCount;
}

void TTabletTrackerImpl::SchedulePeerRevocation(TTabletCell* cell, ITabletCellBalancer* balancer, TBundleCounter* counter)
{
    // Don't perform failover until enough time has passed since the start.
    if (TInstant::Now() < StartTime_ + GetDynamicConfig()->PeerRevocationTimeout) {
        return;
    }

    for (TPeerId peerId = 0; peerId < cell->Peers().size(); ++peerId) {
        const auto& peer = cell->Peers()[peerId];
        if (peer.Descriptor.IsNull()) {
            continue;
        }

        auto error = IsFailed(peer, cell->GetCellBundle()->NodeTagFilter(), GetDynamicConfig()->PeerRevocationTimeout);

        if (!error.IsOK()) {
            YT_LOG_DEBUG(error, "Schedule peer revocation (CellId: %v, PeerId: %v, Address: %v)",
                cell->GetId(),
                peerId,
                peer.Descriptor.GetDefaultAddress());

            balancer->RevokePeer(cell, peerId);

            NProfiling::TTagIdList tagIds{
                cell->GetCellBundle()->GetProfilingTag(),
                NProfiling::TProfileManager::Get()->RegisterTag("reason", error.GetMessage())
            };

            (*counter)[tagIds]++;
        }
    }
}

TError TTabletTrackerImpl::IsFailed(
    const TTabletCell::TPeer& peer,
    const TBooleanFormula& nodeTagFilter,
    TDuration timeout)
{
    const auto& nodeTracker = Bootstrap_->GetNodeTracker();
    const auto* node = nodeTracker->FindNodeByAddress(peer.Descriptor.GetDefaultAddress());
    if (node) {
        if (node->GetBanned()) {
            return TError("Node banned");
        }

        if (node->GetDecommissioned()) {
            return TError("Node decommissioned");
        }

        if (node->GetDisableTabletCells()) {
            return TError("Node tablet slots disabled");
        }

        if (!nodeTagFilter.IsSatisfiedBy(node->Tags())) {
            return TError("Node does not satisfy tag filter");
        }
    }

    if (peer.LastSeenTime + timeout > TInstant::Now()) {
        return TError();
    }

    if (peer.Node) {
        return TError();
    }

    return TError("Node is not assigned");
}

bool TTabletTrackerImpl::IsDecommissioned(
    const TNode* node,
    const TBooleanFormula& nodeTagFilter)
{
    if (!node) {
        return false;
    }

    if (node->GetBanned()) {
        return false;
    }

    if (!nodeTagFilter.IsSatisfiedBy(node->Tags())) {
        return false;
    }

    if (node->GetDecommissioned()) {
        return true;
    }

    if (node->GetDisableTabletCells()) {
        return true;
    }

    return false;
}

int TTabletTrackerImpl::FindGoodPeer(const TTabletCell* cell)
{
    for (TPeerId id = 0; id < static_cast<int>(cell->Peers().size()); ++id) {
        const auto& peer = cell->Peers()[id];
        if (CheckIfNodeCanHostTabletCells(peer.Node)) {
            return id;
        }
    }
    return InvalidPeerId;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
