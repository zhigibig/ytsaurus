#include "stdafx.h"
#include "holder_lease_tracker.h"
#include "chunk_manager.h"
#include "holder.h"

#include <ytlib/cell_master/bootstrap.h>
#include <ytlib/cell_master/config.h>

namespace NYT {
namespace NChunkServer {

using namespace NProto;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("ChunkServer");

////////////////////////////////////////////////////////////////////////////////

THolderLeaseTracker::THolderLeaseTracker(
    TChunkManagerConfigPtr config,
    TBootstrap* bootstrap)
    : Config(config)
    , Bootstrap(bootstrap)
    , OnlineHolderCount(0)
{
    YASSERT(config);
    YASSERT(bootstrap);
}

void THolderLeaseTracker::OnHolderRegistered(const THolder& holder, bool confirmed)
{
    THolderInfo holderInfo;
    holderInfo.Confirmed = confirmed;
    holderInfo.Lease = TLeaseManager::CreateLease(
        GetTimeout(holder, holderInfo),
        ~FromMethod(
            &THolderLeaseTracker::OnExpired,
            MakeStrong(this),
            holder.GetId())
        ->Via(
            Bootstrap->GetStateInvoker(EStateThreadQueue::ChunkRefresh),
            Bootstrap->GetMetaStateManager()->GetEpochContext()));
    YVERIFY(HolderInfoMap.insert(MakePair(holder.GetId(), holderInfo)).second);
}

void THolderLeaseTracker::OnHolderOnline(const THolder& holder)
{
    auto& holderInfo = GetHolderInfo(holder.GetId());
    holderInfo.Confirmed = true;
    RenewLease(holder, holderInfo);
    YASSERT(holder.GetState() == EHolderState::Online);
    ++OnlineHolderCount;
}

void THolderLeaseTracker::OnHolderUnregistered(const THolder& holder)
{
    auto holderId = holder.GetId();
    auto& holderInfo = GetHolderInfo(holderId);
    TLeaseManager::CloseLease(holderInfo.Lease);
    YVERIFY(HolderInfoMap.erase(holderId) == 1);
    if (holder.GetState() == EHolderState::Online) {
        --OnlineHolderCount;
    }
}

void THolderLeaseTracker::OnHolderHeartbeat(const THolder& holder)
{
    auto& holderInfo = GetHolderInfo(holder.GetId());
    holderInfo.Confirmed = true;
    RenewLease(holder, holderInfo);
}

bool THolderLeaseTracker::IsHolderConfirmed(const THolder& holder)
{
    const auto& holderInfo = GetHolderInfo(holder.GetId());
    return holderInfo.Confirmed;
}

int THolderLeaseTracker::GetOnlineHolderCount()
{
    return OnlineHolderCount;
}

void THolderLeaseTracker::OnExpired(THolderId holderId)
{
    // Check if the holder is still registered.
    auto* holderInfo = FindHolderInfo(holderId);
    if (!holderInfo)
        return;

    LOG_INFO("Holder expired (HolderId: %d)", holderId);

    TMsgUnregisterHolder message;
    message.set_holder_id(holderId);
    Bootstrap
        ->GetChunkManager()
        ->InitiateUnregisterHolder(message)
        ->SetRetriable(Config->HolderExpirationBackoffTime)
        ->OnSuccess(~FromFunctor([=] (TVoid) {
            LOG_INFO("Holder expiration commit success (HolderId: %d)", holderId);
        }))
        ->OnError(~FromFunctor([=] () {
            LOG_INFO("Holder expiration commit failed (HolderId: %d)", holderId);
        }))
        ->Commit();
}

TDuration THolderLeaseTracker::GetTimeout(const THolder& holder, const THolderInfo& holderInfo)
{
    if (!holderInfo.Confirmed) {
        return Config->UnconfirmedHolderTimeout;
    }
    return holder.GetState() == EHolderState::Registered
        ? Config->RegisteredHolderTimeout
        : Config->OnlineHolderTimeout;
}

void THolderLeaseTracker::RenewLease(const THolder& holder, const THolderInfo& holderInfo)
{
    TLeaseManager::RenewLease(
        holderInfo.Lease,
        GetTimeout(holder, holderInfo));
}

THolderLeaseTracker::THolderInfo* THolderLeaseTracker::FindHolderInfo(THolderId holderId)
{
    auto it = HolderInfoMap.find(holderId);
    return it == HolderInfoMap.end() ? NULL : &it->second;
}

THolderLeaseTracker::THolderInfo& THolderLeaseTracker::GetHolderInfo(THolderId holderId)
{
    auto it = HolderInfoMap.find(holderId);
    YASSERT(it != HolderInfoMap.end());
    return it->second;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
