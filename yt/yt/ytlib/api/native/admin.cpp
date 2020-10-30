#include "admin.h"
#include "box.h"
#include "config.h"
#include "connection.h"
#include "private.h"

#include <yt/client/api/admin.h>

#include <yt/ytlib/admin/admin_service_proxy.h>

#include <yt/ytlib/controller_agent/controller_agent_service_proxy.h>

#include <yt/ytlib/hive/cell_directory.h>
#include <yt/ytlib/hive/cell_directory_synchronizer.h>

#include <yt/ytlib/hydra/hydra_service_proxy.h>

#include <yt/ytlib/node_tracker_client/node_tracker_service_proxy.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/core/rpc/helpers.h>

#include <yt/core/concurrency/scheduler.h>

namespace NYT::NApi::NNative {

////////////////////////////////////////////////////////////////////////////////

using namespace NAdmin;
using namespace NConcurrency;
using namespace NRpc;
using namespace NObjectClient;
using namespace NTabletClient;
using namespace NNodeTrackerClient;
using namespace NHydra;
using namespace NHiveClient;
using namespace NJobTrackerClient;
using namespace NScheduler;
using namespace NControllerAgent;

DECLARE_REFCOUNTED_CLASS(TAdmin)

////////////////////////////////////////////////////////////////////////////////

class TAdmin
    : public IAdmin
{
public:
    TAdmin(
        IConnectionPtr connection,
        const TAdminOptions& options)
        : Connection_(std::move(connection))
        , Options_(options)
        , Logger(NLogging::TLogger(ApiLogger)
            .AddTag("AdminId: %v", TGuid::Create()))
    {
        Y_UNUSED(Options_);
    }

#define DROP_BRACES(...) __VA_ARGS__
#define IMPLEMENT_METHOD(returnType, method, signature, args) \
    virtual TFuture<returnType> method signature override \
    { \
        return Execute( \
            AsStringBuf(#method), \
            BIND( \
                &TAdmin::Do ## method, \
                MakeStrong(this), \
                DROP_BRACES args)); \
    }

    IMPLEMENT_METHOD(int, BuildSnapshot, (
        const TBuildSnapshotOptions& options),
        (options))
    IMPLEMENT_METHOD(TCellIdToSnapshotIdMap, BuildMasterSnapshots, (
        const TBuildMasterSnapshotsOptions& options),
        (options))
    IMPLEMENT_METHOD(void, SwitchLeader, (
        TCellId cellId,
        TPeerId newLeaderId,
        const TSwitchLeaderOptions& options),
        (cellId, newLeaderId, options))
    IMPLEMENT_METHOD(void, GCCollect, (
        const TGCCollectOptions& options),
        (options))
    IMPLEMENT_METHOD(void, KillProcess, (
        const TString& address,
        const TKillProcessOptions& options),
        (address, options))
    IMPLEMENT_METHOD(TString, WriteCoreDump, (
        const TString& address,
        const TWriteCoreDumpOptions& options),
        (address, options))
    IMPLEMENT_METHOD(TString, WriteOperationControllerCoreDump, (
        TOperationId operationId,
        const TWriteOperationControllerCoreDumpOptions& options),
        (operationId, options))

private:
    const IConnectionPtr Connection_;
    const TAdminOptions Options_;

    const NLogging::TLogger Logger;


    template <class T>
    TFuture<T> Execute(TStringBuf commandName, TCallback<T()> callback)
    {
        return BIND([=, this_ = MakeStrong(this)] () {
                try {
                    YT_LOG_DEBUG("Command started (Command: %v)", commandName);
                    TBox<T> result(callback);
                    YT_LOG_DEBUG("Command completed (Command: %v)", commandName);
                    return result.Unwrap();
                } catch (const std::exception& ex) {
                    YT_LOG_DEBUG(ex, "Command failed (Command: %v)", commandName);
                    throw;
                }
            })
            .AsyncVia(Connection_->GetInvoker())
            .Run();
    }

    int DoBuildSnapshot(const TBuildSnapshotOptions& options)
    {
        auto cellId = options.CellId ? options.CellId : Connection_->GetPrimaryMasterCellId();
        auto channel = GetLeaderCellChannelOrThrow(cellId);

        THydraServiceProxy proxy(channel);
        auto req = proxy.ForceBuildSnapshot();
        req->SetTimeout(TDuration::Hours(1)); // effective infinity
        req->set_set_read_only(options.SetReadOnly);
        req->set_wait_for_snapshot_completion(options.WaitForSnapshotCompletion);

        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();

        return rsp->snapshot_id();
    }

    TCellIdToSnapshotIdMap DoBuildMasterSnapshots(const TBuildMasterSnapshotsOptions& options)
    {
        using TResponseFuture = TFuture<TIntrusivePtr<TTypedClientResponse<NHydra::NProto::TRspForceBuildSnapshot>>>;
        struct TSnapshotRequest
        {
            TResponseFuture Future;
            TCellId CellId;
        };

        auto constructRequest = [&] (TCellId cellId) {
            auto channel = GetLeaderCellChannelOrThrow(cellId);
            THydraServiceProxy proxy(channel);
            auto req = proxy.ForceBuildSnapshot();
            req->SetTimeout(TDuration::Hours(1)); // effective infinity
            req->set_set_read_only(options.SetReadOnly);
            req->set_wait_for_snapshot_completion(options.WaitForSnapshotCompletion);
            return req;
        };

        std::vector<TCellId> cellIds;

        cellIds.push_back(Connection_->GetPrimaryMasterCellId());

        for (auto cellTag : Connection_->GetSecondaryMasterCellTags()) {
            cellIds.push_back(Connection_->GetMasterCellId(cellTag));
        }

        std::queue<TSnapshotRequest> requestQueue;
        auto enqueueRequest = [&] (TCellId cellId) {
            YT_LOG_INFO("Requesting cell to build a snapshot (CellId: %v)", cellId);
            auto request = constructRequest(cellId);
            requestQueue.push({request->Invoke(), cellId});
        };

        for (auto cellId : cellIds) {
            enqueueRequest(cellId);
        }

        THashMap<TCellId, int> cellIdToSnapshotId;
        while (!requestQueue.empty()) {
            auto request = requestQueue.front();
            requestQueue.pop();

            auto cellId = request.CellId;
            YT_LOG_INFO("Waiting for snapshot (CellId: %v)", cellId);
            auto snapshotIdOrError = WaitFor(request.Future);
            if (snapshotIdOrError.IsOK()) {
                auto snapshotId = snapshotIdOrError.Value()->snapshot_id();
                YT_LOG_INFO("Snapshot built successfully (CellId: %v, SnapshotId: %v)", cellId, snapshotId);
                cellIdToSnapshotId[cellId] = snapshotId;
            } else {
                auto errorCode = snapshotIdOrError.GetCode();
                if (errorCode == NHydra::EErrorCode::ReadOnlySnapshotBuilt) {
                    YT_LOG_INFO("Skipping cell since it is already in read-only mode and has a valid snapshot (CellId: %v)", cellId);
                    auto snapshotId = snapshotIdOrError.Attributes().Get<int>("snapshot_id");
                    cellIdToSnapshotId[cellId] = snapshotId;
                } else if (options.Retry && errorCode != NHydra::EErrorCode::ReadOnlySnapshotBuildFailed) {
                    YT_LOG_INFO(snapshotIdOrError, "Failed to build snapshot; retrying (CellId: %v)", cellId);
                    enqueueRequest(cellId);
                } else {
                    snapshotIdOrError.ThrowOnError();
                }
            }
        }

        return cellIdToSnapshotId;
    }

    void DoSwitchLeader(
        TCellId cellId,
        TPeerId newLeaderId,
        const TSwitchLeaderOptions& options)
    {
        auto currentLeaderChannel = GetLeaderCellChannelOrThrow(cellId);

        auto cellDescriptor = GetCellDescriptorOrThrow(cellId);
        std::vector<IChannelPtr> peerChannels;
        for (const auto& peerDescriptor : cellDescriptor.Peers) {
            peerChannels.push_back(CreateRealmChannel(
                Connection_->GetChannelFactory()->CreateChannel(peerDescriptor.GetDefaultAddress()),
                cellId));
        }

        if (newLeaderId < 0 || newLeaderId >= peerChannels.size()) {
            THROW_ERROR_EXCEPTION("New leader peer id is invalid: expected in range [0,%v], got %v",
                peerChannels.size() - 1,
                newLeaderId);
        }
        const auto& newLeaderChannel = peerChannels[newLeaderId];

        auto timeout = Connection_->GetConfig()->HydraControlRpcTimeout;

        {
            YT_LOG_DEBUG("Preparing switch at current leader");

            THydraServiceProxy proxy(currentLeaderChannel);
            auto req = proxy.PrepareLeaderSwitch();
            req->SetTimeout(timeout);
            
            WaitFor(req->Invoke())
                .ValueOrThrow();
        }

        {
            YT_LOG_DEBUG("Synchronizing new leader with the current one");

            THydraServiceProxy proxy(newLeaderChannel);
            auto req = proxy.ForceSyncWithLeader();
            req->SetTimeout(timeout);
            
            WaitFor(req->Invoke())
                .ValueOrThrow();
        }

        TError restartReason(
            "Switching leader to %v by admin request",
            cellDescriptor.Peers[newLeaderId].GetDefaultAddress());

        {
            YT_LOG_DEBUG("Restarting new leader with priority boost armed");

            THydraServiceProxy proxy(newLeaderChannel);
            auto req = proxy.ForceRestart();
            req->SetTimeout(timeout);
            ToProto(req->mutable_reason(), restartReason);
            req->set_arm_priority_boost(true);
            
            WaitFor(req->Invoke())
                .ValueOrThrow();
        }
        
        {
            YT_LOG_DEBUG("Restarting all other peers");

            for (int peerId = 0; peerId < peerChannels.size(); ++peerId) {
                if (peerId == newLeaderId) {
                    continue;
                }

                THydraServiceProxy proxy(peerChannels[peerId]);
                auto req = proxy.ForceRestart();
                ToProto(req->mutable_reason(), restartReason);
                req->SetTimeout(timeout);
                
                // Fire-and-forget.
                req->Invoke();
            }
        }
    }

    void DoGCCollect(const TGCCollectOptions& options)
    {
        auto cellId = options.CellId ? options.CellId : Connection_->GetPrimaryMasterCellId();
        auto channel = Connection_->GetMasterChannelOrThrow(EMasterChannelKind::Leader, cellId);

        TObjectServiceProxy proxy(channel);
        auto req = proxy.GCCollect();
        req->SetTimeout(TDuration::Hours(1)); // effective infinity

        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    void DoKillProcess(const TString& address, const TKillProcessOptions& options)
    {
        auto channel = Connection_->GetChannelFactory()->CreateChannel(address);

        TAdminServiceProxy proxy(channel);
        auto req = proxy.Die();
        req->set_exit_code(options.ExitCode);
        
        // NB: this will always throw an error since the service can
        // never reply to the request because it makes _exit immediately.
        // This is the intended behavior.
        WaitFor(req->Invoke())
            .ThrowOnError();
    }

    TString DoWriteCoreDump(const TString& address, const TWriteCoreDumpOptions& options)
    {
        auto channel = Connection_->GetChannelFactory()->CreateChannel(address);

        TAdminServiceProxy proxy(channel);
        auto req = proxy.WriteCoreDump();
        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();
        return rsp->path();
    }

    TString DoWriteOperationControllerCoreDump(
        TOperationId operationId,
        const TWriteOperationControllerCoreDumpOptions& /*options*/)
    {
        auto address = FindControllerAgentAddressFromCypress(
            operationId,
            Connection_->GetMasterChannelOrThrow(EMasterChannelKind::Follower));
        if (!address) {
            THROW_ERROR_EXCEPTION("Cannot find address of the controller agent for operation %v",
                operationId);
        }

        auto channel = Connection_->GetChannelFactory()->CreateChannel(*address);

        TControllerAgentServiceProxy proxy(channel);
        auto req = proxy.WriteOperationControllerCoreDump();
        ToProto(req->mutable_operation_id(), operationId);
        
        auto rsp = WaitFor(req->Invoke())
            .ValueOrThrow();
        return rsp->path();
    }

    
    IChannelPtr GetLeaderCellChannelOrThrow(TCellId cellId)
    {
        WaitFor(Connection_->GetCellDirectorySynchronizer()->Sync())
            .ThrowOnError();

        const auto& cellDirectory = Connection_->GetCellDirectory();
        return cellDirectory->GetChannelOrThrow(cellId);
    }
    
    TCellDescriptor GetCellDescriptorOrThrow(TCellId cellId)
    {
        WaitFor(Connection_->GetCellDirectorySynchronizer()->Sync())
            .ThrowOnError();

        const auto& cellDirectory = Connection_->GetCellDirectory();
        return cellDirectory->GetDescriptorOrThrow(cellId);
    }
};

DEFINE_REFCOUNTED_TYPE(TAdmin)

IAdminPtr CreateAdmin(
    IConnectionPtr connection,
    const TAdminOptions& options)
{
    return New<TAdmin>(std::move(connection), options);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
