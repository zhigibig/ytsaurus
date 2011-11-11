#include "stdafx.h"
#include "leader_pinger.h"

#include "../misc/serialize.h"
#include "../bus/message.h"

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;

////////////////////////////////////////////////////////////////////////////////

TLeaderPinger::TLeaderPinger(
    const TConfig& config,
    TMetaStateManager::TPtr metaStateManager,
    TCellManager::TPtr cellManager,
    TPeerId leaderId,
    TEpoch epoch,
    IInvoker::TPtr controlInvoker)
    : Config(config)
    , MetaStateManager(metaStateManager)
    , CellManager(cellManager)
    , LeaderId(leaderId)
    , Epoch(epoch)
    , CancelableInvoker(New<TCancelableInvoker>(controlInvoker))
{
    YASSERT(~metaStateManager != NULL);
    YASSERT(~cellManager != NULL);
    YASSERT(~controlInvoker != NULL);

    SchedulePing();
}

void TLeaderPinger::Stop()
{
    CancelableInvoker->Cancel();
    CancelableInvoker.Reset();
    MetaStateManager.Reset();
}

void TLeaderPinger::SchedulePing()
{
    TDelayedInvoker::Get()->Submit(
        FromMethod(&TLeaderPinger::SendPing, TPtr(this))
        ->Via(~CancelableInvoker),
        Config.PingInterval);

    LOG_DEBUG("Leader ping scheduled");
}

void TLeaderPinger::SendPing()
{
    auto status = MetaStateManager->GetControlStatus();
    auto proxy = CellManager->GetMasterProxy<TProxy>(LeaderId);
    auto request = proxy->PingLeader();
    request->SetEpoch(Epoch.ToProto());
    request->SetFollowerId(CellManager->GetSelfId());
    request->SetStatus(status);
    request->Invoke(Config.RpcTimeout)->Subscribe(
        FromMethod(
        &TLeaderPinger::OnSendPing, TPtr(this))
        ->Via(~CancelableInvoker));

    LOG_DEBUG("Leader ping sent (LeaderId: %d, State: %s)",
        LeaderId,
        ~status.ToString());
}

void TLeaderPinger::OnSendPing(TProxy::TRspPingLeader::TPtr response)
{
    if (response->IsOK()) {
        LOG_DEBUG("Leader ping succeeded (LeaderId: %d)",
            LeaderId);
    } else {
        LOG_WARNING("Error pinging leader (LeaderId: %d, Error: %s)",
            LeaderId,
            ~response->GetError().ToString());
    }

    if (response->GetErrorCode() == NRpc::EErrorCode::Timeout) {
        SendPing();
    } else {
        SchedulePing();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
