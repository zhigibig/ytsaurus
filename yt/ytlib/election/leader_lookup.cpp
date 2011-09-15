#include "leader_lookup.h"
#include "election_manager.h"

#include "../misc/serialize.h"
#include "../logging/log.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("LeaderLookup");

////////////////////////////////////////////////////////////////////////////////

TLeaderLookup::TLeaderLookup(const TConfig& config)
    : Config(config)
{ }

TLeaderLookup::TLookupResult::TPtr TLeaderLookup::GetLeader()
{
    TLookupResult::TPtr asyncResult = New<TLookupResult>();
    TParallelAwaiter::TPtr awaiter = New<TParallelAwaiter>();

    FOREACH(Stroka address, Config.Addresses) {
        LOG_DEBUG("Requesting leader from master %s", ~address);

        TProxy proxy(~ChannelCache.GetChannel(address));
        auto request = proxy.GetStatus();
        awaiter->Await(request->Invoke(Config.Timeout), FromMethod(
            &TLeaderLookup::OnResponse,
            awaiter,
            asyncResult,
            address));
    }
    
    awaiter->Complete(FromMethod(
        &TLeaderLookup::OnComplete,
        asyncResult));
    return asyncResult;
}

void TLeaderLookup::OnResponse(
    TProxy::TRspGetStatus::TPtr response,
    TParallelAwaiter::TPtr awaiter,
    TLookupResult::TPtr asyncResult,
    Stroka address)
{
    if (!response->IsOK()) {
        LOG_WARNING("Error requesting leader from master %s (ErrorCode: %s)",
            ~address,
            ~response->GetErrorCode().ToString());
        return;
    }

    TPeerId voteId = response->GetVoteId();
    TGuid epoch = TGuid::FromProto(response->GetVoteEpoch());

    LOG_DEBUG("Received status from master %s (Id: %d, State: %s, VoteId: %d, Priority: %" PRIx64 ", Epoch: %s)",
        ~address,
        response->GetSelfId(),
        ~TProxy::EState(response->GetState()).ToString(),
        response->GetVoteId(),
        response->GetPriority(),
        ~epoch.ToString());

    if (response->GetState() == TProxy::EState::Leading) {
        YASSERT(voteId == response->GetSelfId());

        TResult result;
        result.Address = address;
        result.Id = voteId;
        result.Epoch = epoch;
        asyncResult->Set(result);

        awaiter->Cancel();

        LOG_INFO("Leader found at %s (Id: %d, Epoch: %s)",
            ~address,
            response->GetSelfId(),
            ~epoch.ToString());
    }   
}

void TLeaderLookup::OnComplete(TLookupResult::TPtr asyncResult)
{
    TResult result;
    result.Address = "";
    result.Id = InvalidPeerId;
    result.Epoch = TEpoch();
    asyncResult->Set(result);

    LOG_INFO("No leader found");
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
