#include "stdafx.h"
#include "persistent_state_manager.h"
#include "meta_state_manager.h"
#include "private.h"
#include "config.h"
#include "change_log.h"
#include "change_log_cache.h"
#include "meta_state_manager_proxy.h"
#include "snapshot.h"
#include "snapshot_builder.h"
#include "recovery.h"
#include "mutation_committer.h"
#include "follower_tracker.h"
#include "meta_state.h"
#include "snapshot_store.h"
#include "decorated_meta_state.h"
#include "mutation_context.h"
#include "response_keeper.h"

#include <ytlib/misc/thread_affinity.h>

#include <ytlib/election/cell_manager.h>
#include <ytlib/election/election_manager.h>

#include <ytlib/rpc/service.h>

#include <ytlib/ytree/fluent.h>

#include <ytlib/meta_state/meta_state_manager.pb.h>

#include <util/folder/dirut.h>

namespace NYT {
namespace NMetaState {

using namespace NElection;
using namespace NRpc;
using namespace NYTree;

///////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;

////////////////////////////////////////////////////////////////////////////////

class TPersistentStateManager;
typedef TIntrusivePtr<TPersistentStateManager> TPersistentStateManagerPtr;

struct TEpochContext;
typedef TIntrusivePtr<TEpochContext> TEpochContextPtr;

////////////////////////////////////////////////////////////////////////////////

struct TEpochContext
    : public NElection::TEpochContext
{
    IInvokerPtr EpochSystemStateInvoker;
    IInvokerPtr EpochUserStateInvoker;
    IInvokerPtr EpochControlInvoker;
    TSnapshotBuilderPtr SnapshotBuilder;
    TLeaderRecoveryPtr LeaderRecovery;
    TFollowerRecoveryPtr FollowerRecovery;
    TLeaderCommitterPtr LeaderCommitter;
    TFollowerCommitterPtr FollowerCommitter;
    TFollowerTrackerPtr FollowerTracker;
};

////////////////////////////////////////////////////////////////////////////////

class TPersistentStateManager
    : public TServiceBase
    , public IMetaStateManager
{
public:
    class TElectionCallbacks
        : public IElectionCallbacks
    {
    public:
        explicit TElectionCallbacks(TPersistentStateManagerPtr owner)
            : Owner(owner)
        { }

        virtual void OnStartLeading() override
        {
            Owner->OnElectionStartLeading();
        }

        virtual void OnStopLeading() override
        {
            Owner->OnElectionStopLeading();
        }

        virtual void OnStartFollowing() override
        {
            Owner->OnElectionStartFollowing();
        }

        virtual void OnStopFollowing() override
        {
            Owner->OnElectionStopFollowing();
        }

        virtual TPeerPriority GetPriority() override
        {
            return Owner->GetPriority();
        }

        virtual Stroka FormatPriority(TPeerPriority priority) override
        {
            return Owner->FormatPriority(priority);
        }

    private:
        TPersistentStateManagerPtr Owner;

    };

    TPersistentStateManager(
        TPersistentStateManagerConfigPtr config,
        IInvokerPtr controlInvoker,
        IInvokerPtr stateInvoker,
        IMetaStatePtr metaState,
        NRpc::IServerPtr server)
        : TServiceBase(controlInvoker, TProxy::GetServiceName(), Logger.GetCategory())
        , Config(config)
        , ControlInvoker(controlInvoker)
        , StateInvoker(stateInvoker)
        , ReadOnly(false)
        , ControlStatus(EPeerStatus::Stopped)
    {
        ChangeLogCache = New<TChangeLogCache>(Config->ChangeLogs);

        SnapshotStore = New<TSnapshotStore>(Config->Snapshots);
        DecoratedState = New<TDecoratedMetaState>(
            Config,
            metaState,
            StateInvoker,
            ControlInvoker,
            SnapshotStore,
            ChangeLogCache);

        IOQueue = New<TActionQueue>("MetaStateIO");

        VERIFY_INVOKER_AFFINITY(controlInvoker, ControlThread);
        VERIFY_INVOKER_AFFINITY(stateInvoker, StateThread);
        VERIFY_INVOKER_AFFINITY(IOQueue->GetInvoker(), IOThread);

        CellManager = New<TCellManager>(Config->Cell);

        LOG_INFO("SelfAddress: %s, SelfId: %d",
            ~CellManager->GetSelfAddress(),
            CellManager->GetSelfId());

        ElectionManager = New<TElectionManager>(
            Config->Election,
            CellManager,
            controlInvoker,
            New<TElectionCallbacks>(this));

        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetSnapshotInfo));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ReadSnapshot));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetChangeLogInfo));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ReadChangeLog));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(ApplyMutations));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AdvanceSegment));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(PingFollower));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(LookupSnapshot));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetQuorum));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(BuildSnapshot)
            .SetInvoker(DecoratedState->CreateGuardedUserInvoker(StateInvoker)));

        server->RegisterService(this);
        server->RegisterService(ElectionManager);
    }

    virtual void Start() override
    {
        VERIFY_THREAD_AFFINITY_ANY();
        YCHECK(ControlStatus == EPeerStatus::Stopped);

        ChangeLogCache->Start();
        SnapshotStore->Start();
        DecoratedState->Start();

        ControlStatus = EPeerStatus::Elections;

        DecoratedState->GetSystemInvoker()->Invoke(BIND(
            &TDecoratedMetaState::Clear,
            DecoratedState));

        ElectionManager->Start();
    }

    virtual EPeerStatus GetControlStatus() const override
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        return ControlStatus;
    }

    virtual EPeerStatus GetStateStatus() const override
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        return DecoratedState->GetStatus();
    }

    virtual IInvokerPtr CreateGuardedStateInvoker(IInvokerPtr underlyingInvoker) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return DecoratedState->CreateGuardedUserInvoker(underlyingInvoker);
    }

    virtual bool HasActiveQuorum() const override
    {
        auto tracker = GetFollowerTracker();
        if (!tracker) {
            return false;
        }

        return tracker->HasActiveQuorum();
    }

    virtual NElection::TEpochContextPtr GetEpochContext() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return EpochContext;
    }

    virtual TCellManagerPtr GetCellManager() const override
    {
        return CellManager;
    }
    
    virtual bool GetReadOnly() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return AtomicGet(ReadOnly);
    }

    virtual void SetReadOnly(bool value) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        AtomicSet(ReadOnly, value);
    }

    virtual void GetMonitoringInfo(IYsonConsumer* consumer) override
    {
        auto tracker = GetFollowerTracker();

        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("status").Scalar(FormatEnum(ControlStatus))
                .Item("version").Scalar(DecoratedState->GetVersionAsync().ToString())
                .Item("reachable_version").Scalar(DecoratedState->GetReachableVersionAsync().ToString())
                .Item("elections").Do(BIND(&TElectionManager::GetMonitoringInfo, ElectionManager))
                .DoIf(tracker, [=] (TFluentMap fluent) {
                    fluent
                        .Item("has_quorum").Scalar(tracker->HasActiveQuorum())
                        .Item("active_followers").DoListFor(
                            0,
                            CellManager->GetPeerCount(),
                            [=] (TFluentList fluent, TPeerId id) {
                                if (tracker->IsPeerActive(id)) {
                                    fluent.Item().Scalar(id);
                                }
                            });
                })
            .EndMap();
    }

    DEFINE_SIGNAL(void(), StartLeading);
    DEFINE_SIGNAL(void(), LeaderRecoveryComplete);
    DEFINE_SIGNAL(void(), ActiveQuorumEstablished);
    DEFINE_SIGNAL(void(), StopLeading);

    DEFINE_SIGNAL(void(), StartFollowing);
    DEFINE_SIGNAL(void(), FollowerRecoveryComplete);
    DEFINE_SIGNAL(void(), StopFollowing);

    virtual TFuture< TValueOrError<TMutationResponse> > CommitMutation(const TMutationRequest& request) override
    {
        VERIFY_THREAD_AFFINITY(StateThread);
        YCHECK(!DecoratedState->GetMutationContext());

        if (GetStateStatus() != EPeerStatus::Leading) {
            return MakeFuture(TValueOrError<TMutationResponse>(TError(
                ECommitCode::NoLeader,
                "Not a leader")));
        }

        if (AtomicGet(ReadOnly)) {
            return MakeFuture(TValueOrError<TMutationResponse>(TError(
                ECommitCode::ReadOnly,
                "Read-only mode is active")));
        }

        auto epochContext = EpochContext;
        if (!epochContext ||
            !epochContext->FollowerTracker ||
            !epochContext->LeaderCommitter ||
            !epochContext->FollowerTracker->HasActiveQuorum())
        {
            return MakeFuture(TValueOrError<TMutationResponse>(TError(
                ECommitCode::NoQuorum,
                "No active quorum")));
        }

        return epochContext->LeaderCommitter->Commit(request)
            .Apply(BIND(&TThis::OnMutationCommitted, MakeStrong(this)));
    }

    virtual TMutationContext* GetMutationContext() override
    {
        return DecoratedState->GetMutationContext();
    }

 private:
    typedef TPersistentStateManager TThis;
    typedef TMetaStateManagerProxy TProxy;
    typedef TProxy::EErrorCode EErrorCode;

    TPersistentStateManagerConfigPtr Config;
    TCellManagerPtr CellManager;
    IInvokerPtr ControlInvoker;
    IInvokerPtr StateInvoker;
    TAtomic ReadOnly;
    EPeerStatus ControlStatus;

    NElection::TElectionManagerPtr ElectionManager;
    TChangeLogCachePtr ChangeLogCache;
    TSnapshotStorePtr SnapshotStore;
    TDecoratedMetaStatePtr DecoratedState;
    TActionQueuePtr IOQueue;

    TEpochContextPtr EpochContext;

    // RPC methods

    DECLARE_RPC_SERVICE_METHOD(NProto, GetSnapshotInfo)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        i32 snapshotId = request->snapshot_id();

        context->SetRequestInfo("SnapshotId: %d", snapshotId);

        auto result = SnapshotStore->GetReader(snapshotId);
        if (!result.IsOK()) {
            THROW_ERROR result;
        }

        auto reader = result.Value();
        reader->Open();

        i64 length = reader->GetLength();
        auto checksum = reader->GetChecksum();
        int prevRecordCount = reader->GetPrevRecordCount();

        response->set_length(length);

        context->SetResponseInfo("Length: %" PRId64 ", PrevRecordCount: %d, Checksum: %" PRIx64,
            length,
            prevRecordCount,
            checksum);

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, ReadSnapshot)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        UNUSED(response);

        i32 snapshotId = request->snapshot_id();
        i64 offset = request->offset();
        i32 length = request->length();

        context->SetRequestInfo("SnapshotId: %d, Offset: %" PRId64 ", Length: %d",
            snapshotId,
            offset,
            length);

        YCHECK(offset >= 0);
        YCHECK(length >= 0);

        auto fileName = SnapshotStore->GetSnapshotFileName(snapshotId);
        if (!isexist(~fileName)) {
            context->Reply(TError(
                EErrorCode::NoSuchSnapshot,
                Sprintf("No such snapshot %d", snapshotId)));
            return;
        }

        TSharedPtr<TFile, TAtomicCounter> snapshotFile;
        try {
            snapshotFile = new TFile(fileName, OpenExisting | RdOnly | CloseOnExec);
        }
        catch (const std::exception& ex) {
            LOG_FATAL(ex, "IO error while opening snapshot %d",
                snapshotId);
        }

        IOQueue->GetInvoker()->Invoke(context->Wrap(BIND([=] () {
            VERIFY_THREAD_AFFINITY(IOThread);

            TBlob data(length);
            size_t bytesRead = 0;
            try {
                snapshotFile->Seek(offset, sSet);
                bytesRead = snapshotFile->Read(&*data.begin(), length);
            } catch (const std::exception& ex) {
                LOG_FATAL(ex, "IO error while reading snapshot %d",
                    snapshotId);
            }

            data.erase(data.begin() + bytesRead, data.end());
            context->Response().Attachments().push_back(TSharedRef(MoveRV(data)));

            context->SetResponseInfo("BytesRead: %" PRISZT, bytesRead);

            context->Reply();
        })));
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, GetChangeLogInfo)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        i32 changeLogId = request->change_log_id();

        context->SetRequestInfo("ChangeLogId: %d",
            changeLogId);

        auto result = ChangeLogCache->Get(changeLogId);
        if (!result.IsOK()) {
            THROW_ERROR result;
        }

        auto changeLog = result.Value();
        i32 recordCount = changeLog->GetRecordCount();

        response->set_record_count(recordCount);

        context->SetResponseInfo("RecordCount: %d", recordCount);
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, ReadChangeLog)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        UNUSED(response);

        i32 changeLogId = request->change_log_id();
        i32 startRecordId = request->start_record_id();
        i32 recordCount = request->record_count();

        context->SetRequestInfo("ChangeLogId: %d, StartRecordId: %d, RecordCount: %d",
            changeLogId,
            startRecordId,
            recordCount);

        YCHECK(startRecordId >= 0);
        YCHECK(recordCount >= 0);

        auto result = ChangeLogCache->Get(changeLogId);
        if (!result.IsOK()) {
            THROW_ERROR result;
        }

        IOQueue->GetInvoker()->Invoke(context->Wrap(BIND(
            &TThis::DoReadChangeLog,
            MakeStrong(this),
            result.Value(),
            startRecordId,
            recordCount)));
    }

    void DoReadChangeLog(
        TCachedAsyncChangeLogPtr changeLog,
        i32 startRecordId,
        i32 recordCount,
        TCtxReadChangeLogPtr context)
    {
        VERIFY_THREAD_AFFINITY(IOThread);

        std::vector<TSharedRef> recordData;
        try {
            changeLog->Read(startRecordId, recordCount, &recordData);
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "IO error while reading changelog %d",
                changeLog->GetId());
        }

        // Pack refs to minimize allocations.
        context->Response().Attachments().push_back(PackRefs(recordData));

        context->SetResponseInfo("RecordCount: %d", recordData.size());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, ApplyMutations)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto epochId = TEpochId::FromProto(request->epoch_id());
        i32 segmentId = request->segment_id();
        i32 recordCount = request->record_count();
        TMetaVersion version(segmentId, recordCount);

        context->SetRequestInfo("EpochId: %s, Version: %s",
            ~epochId.ToString(),
            ~version.ToString());

        if (GetControlStatus() != EPeerStatus::Following && GetControlStatus() != EPeerStatus::FollowerRecovery) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::InvalidStatus,
                "Cannot apply changes while %s",
                ~GetControlStatus().ToString());
        }

        CheckEpoch(epochId);

        int changeCount = request->Attachments().size();
        switch (GetControlStatus()) {
            case EPeerStatus::Following: {
                LOG_DEBUG("ApplyChange: applying changes (Version: %s, ChangeCount: %d)",
                    ~version.ToString(),
                    changeCount);

                EpochContext->FollowerCommitter->Commit(version, request->Attachments())
                    .Subscribe(BIND(&TThis::OnFollowerCommitted, MakeStrong(this), context));
                break;
            }

            case EPeerStatus::FollowerRecovery: {
                if (EpochContext->FollowerRecovery) {
                    LOG_DEBUG("ApplyChange: keeping postponed changes (Version: %s, ChangeCount: %d)",
                        ~version.ToString(),
                        changeCount);

                    auto error = EpochContext->FollowerRecovery->PostponeMutations(version, request->Attachments());
                    if (!error.IsOK()) {
                        LOG_WARNING(error, "Error postponing mutations, restarting");
                        Restart();
                    }

                    response->set_committed(false);
                    context->Reply();
                } else {
                    LOG_DEBUG("ApplyChange: ignoring changes (Version: %s, ChangeCount: %d)",
                        ~version.ToString(),
                        changeCount);
                    context->Reply(TError(
                        EErrorCode::InvalidStatus,
                        "Ping is not received yet (Status: %s)",
                        ~GetControlStatus().ToString()));
                }
                break;
            }

            default:
                YUNREACHABLE();
        }
    }

    void OnFollowerCommitted(TCtxApplyMutationsPtr context, TError error)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto& response = context->Response();
        response.set_committed(error.IsOK());

        if (error.GetCode() == ECommitCode::OutOfOrderMutations) {
            Restart();
        }

        context->Reply(error);
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, PingFollower)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        i32 segmentId = request->segment_id();
        i32 recordCount = request->record_count();
        auto version = TMetaVersion(segmentId, recordCount);
        auto epochId = TEpochId::FromProto(request->epoch_id());

        context->SetRequestInfo("Version: %s, EpochId: %s",
            ~version.ToString(),
            ~epochId.ToString());

        auto status = GetControlStatus();

        if (status != EPeerStatus::Following && status != EPeerStatus::FollowerRecovery) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::InvalidStatus,
                "Cannot process follower ping while %s",
                ~GetControlStatus().ToString());
        }

        CheckEpoch(epochId);

        switch (status) {
            case EPeerStatus::Following:
                // code here for two-phase commit
                // right now, ignoring ping
                break;

            case EPeerStatus::FollowerRecovery:
                if (!EpochContext->FollowerRecovery) {
                    LOG_INFO("Received sync ping from leader (Version: %s, Epoch: %s)",
                        ~version.ToString(),
                        ~epochId.ToString());

                    EpochContext->FollowerRecovery = New<TFollowerRecovery>(
                        Config,
                        CellManager,
                        DecoratedState,
                        ChangeLogCache,
                        SnapshotStore,
                        epochId,
                        EpochContext->LeaderId,
                        ControlInvoker,
                        EpochContext->EpochControlInvoker,
                        EpochContext->EpochSystemStateInvoker,
                        version);

                    EpochContext->FollowerRecovery->Run().Subscribe(
                        BIND(&TThis::OnControlFollowerRecoveryComplete, MakeStrong(this), EpochContext)
                            .Via(EpochContext->EpochControlInvoker));
                }
                break;

            default:
                YUNREACHABLE();
        }

        response->set_status(status);

        // Reply with OK in any case.
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, AdvanceSegment)
    {
        UNUSED(response);
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto epochId = TEpochId::FromProto(request->epoch_id());
        i32 segmentId = request->segment_id();
        i32 recordCount = request->record_count();
        auto version = TMetaVersion(segmentId, recordCount);
        bool createSnapshot = request->create_snapshot();

        context->SetRequestInfo("EpochId: %s, Version: %s, CreateSnapshot: %s",
            ~epochId.ToString(),
            ~version.ToString(),
            ~ToString(createSnapshot));

        if (GetControlStatus() != EPeerStatus::Following && GetControlStatus() != EPeerStatus::FollowerRecovery) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::InvalidStatus,
                "Cannot advance segment while %s",
                ~GetControlStatus().ToString());
        }

        CheckEpoch(epochId);

        switch (GetControlStatus()) {
            case EPeerStatus::Following:
                if (createSnapshot) {
                    LOG_DEBUG("AdvanceSegment: starting snapshot creation");

                    BIND(&TSnapshotBuilder::BuildSnapshotLocal, EpochContext->SnapshotBuilder, version)
                        .AsyncVia(EpochContext->EpochUserStateInvoker)
                        .Run()
                        .Subscribe(BIND(
                            &TThis::OnCreateLocalSnapshot,
                            MakeStrong(this),
                            context));
                } else {
                    LOG_DEBUG("AdvanceSegment: advancing segment");

                    EpochContext->EpochUserStateInvoker->Invoke(context->Wrap(BIND(
                        &TThis::DoStateAdvanceSegment,
                        MakeStrong(this),
                        version,
                        EpochContext->EpochId)));
                }
                break;

            case EPeerStatus::FollowerRecovery: {
                if (EpochContext->FollowerRecovery) {
                    LOG_DEBUG("AdvanceSegment: postponing snapshot creation");

                    auto error = EpochContext->FollowerRecovery->PostponeSegmentAdvance(version);
                    if (!error.IsOK()) {
                        LOG_ERROR(error);
                        Restart();
                    }

                    if (createSnapshot) {
                        context->Reply(TError(
                            EErrorCode::InvalidStatus,
                            "Unable to create a snapshot during recovery"));
                    } else {
                        context->Reply();
                    }
                } else {
                    context->Reply(TError(
                        EErrorCode::InvalidStatus,
                        "Ping is not received yet (Status: %s)",
                        ~GetControlStatus().ToString()));
                }
                break;
            }

            default:
                YUNREACHABLE();
        }
    }

    void DoStateAdvanceSegment(
        const TMetaVersion& version,
        const TEpochId& epochId,
        TCtxAdvanceSegmentPtr context)
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        if (DecoratedState->GetVersion() != version) {
            Restart();
            THROW_ERROR_EXCEPTION(
                EErrorCode::InvalidVersion,
                "Invalid version, segment advancement canceled (Expected: %s, Received: %s)",
                ~version.ToString(),
                ~DecoratedState->GetVersion().ToString());
        }

        DecoratedState->RotateChangeLog(epochId);

        context->Reply();
    }

    void OnCreateLocalSnapshot(
        TCtxAdvanceSegmentPtr context,
        TSnapshotBuilder::TResultOrError result)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto& response = context->Response();

        if (!result.IsOK()) {
            context->Reply(result);
            return;
        }

        const auto& value = result.Value();
        response.set_checksum(value.Checksum);
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, LookupSnapshot)
    {
        i32 maxSnapshotId = request->max_snapshot_id();
        context->SetRequestInfo("MaxSnapshotId: %d", maxSnapshotId);

        i32 snapshotId = SnapshotStore->LookupLatestSnapshot(maxSnapshotId);

        response->set_snapshot_id(snapshotId);
        context->SetResponseInfo("SnapshotId: %d", snapshotId);
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, GetQuorum)
    {
        UNUSED(request);
        VERIFY_THREAD_AFFINITY(ControlThread);

        context->SetRequestInfo("");

        if (GetControlStatus() != EPeerStatus::Leading) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::InvalidStatus,
                "Cannot answer quorum queries while %s",
                ~GetControlStatus().ToString());
        }

        response->set_leader_address(CellManager->GetSelfAddress());
        for (TPeerId id = 0; id < CellManager->GetPeerCount(); ++id) {
            if (EpochContext->FollowerTracker->IsPeerActive(id)) {
                response->add_follower_addresses(CellManager->GetPeerAddress(id));
            }
        }

        *response->mutable_epoch_id() = EpochContext->EpochId.ToProto();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, BuildSnapshot)
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        bool setReadOnly = request->set_read_only();

        context->SetRequestInfo("SetReadOnly: %s",
            ~FormatBool(setReadOnly));

        auto epochContext = EpochContext;

        if (!epochContext || GetStateStatus() != EPeerStatus::Leading) {
            THROW_ERROR_EXCEPTION(EErrorCode::InvalidStatus, "Not a leader");
        }

        if (!HasActiveQuorum()) {
            THROW_ERROR_EXCEPTION(EErrorCode::InvalidStatus, "Not active quorum");
        }

        BuildSnapshotDistributed(epochContext).Subscribe(
            BIND(&TThis::OnSnapshotBuilt, MakeStrong(this), context);

        if (setReadOnly) {
            SetReadOnly(true);
        }
    }

    void OnSnapshotBuilt(
        TCtxBuildSnapshotPtr context,
        TSnapshotBuilder::TResultOrError result)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!result.IsOK()) {
            context->Reply(result);
            return;
        }

        const auto& value = result.Value();
        auto& response = context->Response();
        response.set_snapshot_id(value.SnapshotId);
        context->Reply();
    }

    // End of RPC methods


    void Restart()
    {
        VERIFY_THREAD_AFFINITY_ANY();

        ElectionManager->Restart();
    }

    TValueOrError<TMutationResponse> OnMutationCommitted(TValueOrError<TMutationResponse> result)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        if (!result.IsOK()) {
            LOG_ERROR(result, "Error committing mutation, restarting");
            Restart();
        }

        return result;
    }

    void OnLocalMutationApplied(TEpochContextPtr epochContext)
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        auto version = DecoratedState->GetVersion();
        auto period = Config->MaxChangesBetweenSnapshots;
        if (period &&
            version.RecordCount > 0 &&
            version.RecordCount % period.Get() == 0)
        {
            BuildSnapshotDistributed(epochContext);
        }
    }

    TFuture<TSnapshotBuilder::TResultOrError> BuildSnapshotDistributed(TEpochContextPtr epochContext)
    {
        YASSERT(DecoratedState->GetStatus() == EPeerStatus::Leading);
        YASSERT(HasActiveQuorum());

        epochContext->LeaderCommitter->Flush(true);
        return epochContext->SnapshotBuilder->BuildSnapshotDistributed();
    }


    void OnElectionStartLeading()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Starting leader recovery");

        ControlStatus = EPeerStatus::LeaderRecovery;
        StartEpoch();

        // During recovery the leader is reporting its reachable version to followers.
        auto version = DecoratedState->GetReachableVersionAsync();
        DecoratedState->SetPingVersion(version);

        EpochContext->FollowerTracker = New<TFollowerTracker>(
            Config->FollowerTracker,
            CellManager,
            DecoratedState,
            EpochContext->EpochId,
            EpochContext->EpochControlInvoker);

        EpochContext->LeaderCommitter = New<TLeaderCommitter>(
            Config->LeaderCommitter,
            CellManager,
            DecoratedState,
            ChangeLogCache,
            EpochContext->FollowerTracker,
            EpochContext->EpochId,
            ControlInvoker,
            EpochContext->EpochUserStateInvoker);
        EpochContext->LeaderCommitter->SubscribeMutationApplied(
            BIND(&TThis::OnLocalMutationApplied, MakeWeak(this), EpochContext));

        EpochContext->SnapshotBuilder = New<TSnapshotBuilder>(
            Config->SnapshotBuilder,
            CellManager,
            DecoratedState,
            SnapshotStore,
            EpochContext->EpochId,
            ControlInvoker,
            StateInvoker);

        EpochContext->LeaderRecovery = New<TLeaderRecovery>(
            Config,
            CellManager,
            DecoratedState,
            ChangeLogCache,
            SnapshotStore,
            EpochContext->EpochId,
            ControlInvoker,
            EpochContext->EpochControlInvoker,
            EpochContext->EpochSystemStateInvoker);

        EpochContext->FollowerTracker->Start();

        DecoratedState->GetSystemInvoker()->Invoke(BIND(
            &TThis::DoStateStartLeading,
            MakeStrong(this),
            EpochContext));
    }

    void DoStateStartLeading(TEpochContextPtr epochContext)
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        DecoratedState->OnStartLeading();

        StartLeading_.Fire();

        BIND(&TLeaderRecovery::Run, epochContext->LeaderRecovery)
            .AsyncVia(epochContext->EpochControlInvoker)
            .Run()
            .Subscribe(
                BIND(&TThis::OnStateLeaderRecoveryComplete, MakeStrong(this), epochContext)
                    .Via(epochContext->EpochSystemStateInvoker));
    }

    void OnStateLeaderRecoveryComplete(TEpochContextPtr epochContext, TError error)
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        if (!error.IsOK()) {
            LOG_WARNING(error, "Leader recovery failed, restarting");
            Restart();
            return;
        }

        // Switch to a new changelog unless the current one is empty.
        // This enables changelog truncation for those followers that are down and have uncommitted changes.
        auto version = DecoratedState->GetVersion();
        if (version.RecordCount > 0) {
            LOG_INFO("Switching to a new changelog %d", version.SegmentId + 1);
            epochContext->SnapshotBuilder->RotateChangeLog();
        }

        DecoratedState->OnLeaderRecoveryComplete();

        LeaderRecoveryComplete_.Fire();

        epochContext->EpochControlInvoker->Invoke(BIND(
            &TThis::DoControlLeaderRecoveryComplete,
            MakeStrong(this),
            epochContext));
    }

    void DoControlLeaderRecoveryComplete(TEpochContextPtr epochContext)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        YCHECK(ControlStatus == EPeerStatus::LeaderRecovery);
        ControlStatus = EPeerStatus::Leading;

        LOG_INFO("Leader recovery complete");

        epochContext->FollowerTracker->GetActiveQuorum().Subscribe(
            BIND(&TThis::OnActiveQuorumEstablished, MakeStrong(this))
                .Via(epochContext->EpochUserStateInvoker));
    }

    void OnActiveQuorumEstablished()
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        LOG_INFO("Active quorum established");

        ActiveQuorumEstablished_.Fire();
    }


    void OnElectionStopLeading()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Stopped leading");

        DecoratedState->GetSystemInvoker()->Invoke(BIND(
            &TThis::DoStateStopLeading,
            MakeStrong(this)));

        ControlStatus = EPeerStatus::Elections;

        StopEpoch();
    }

    void DoStateStopLeading()
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        StopLeading_.Fire();

        DecoratedState->OnStopLeading();
    }


    void OnElectionStartFollowing()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Starting follower recovery");

        ControlStatus = EPeerStatus::FollowerRecovery;
        
        StartEpoch();

        EpochContext->FollowerCommitter = New<TFollowerCommitter>(
            DecoratedState,
            ControlInvoker,
            EpochContext->EpochUserStateInvoker);

        EpochContext->SnapshotBuilder = New<TSnapshotBuilder>(
            Config->SnapshotBuilder,
            CellManager,
            DecoratedState,
            SnapshotStore,
            EpochContext->EpochId,
            EpochContext->EpochControlInvoker,
            EpochContext->EpochUserStateInvoker);

        DecoratedState->GetSystemInvoker()->Invoke(BIND(
            &TThis::DoStateStartFollowing,
            MakeStrong(this)));
    }

    void DoStateStartFollowing()
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        DecoratedState->OnStartFollowing();

        StartFollowing_.Fire();
    }

    void OnControlFollowerRecoveryComplete(TEpochContextPtr epochContext, TError error)
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        if (!error.IsOK()) {
            LOG_WARNING(error, "Follower recovery failed, restarting");
            Restart();
            return;
        }

        epochContext->EpochSystemStateInvoker->Invoke(BIND(
            &TThis::DoStateFollowerRecoveryComplete,
            MakeStrong(this),
            epochContext));

        ControlStatus = EPeerStatus::Following;

        LOG_INFO("Follower recovery complete");
    }

    void DoStateFollowerRecoveryComplete(TEpochContextPtr epochContext)
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        DecoratedState->OnFollowerRecoveryComplete();

        FollowerRecoveryComplete_.Fire();
    }


    void OnElectionStopFollowing()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        LOG_INFO("Stopped following");

        DecoratedState->GetSystemInvoker()->Invoke(BIND(
            &TThis::DoStateStopFollowing,
            MakeStrong(this)));

        ControlStatus = EPeerStatus::Elections;

        StopEpoch();
    }

    void DoStateStopFollowing()
    {
        VERIFY_THREAD_AFFINITY(StateThread);

        StopFollowing_.Fire();

        DecoratedState->OnStopFollowing();
    }


    void StartEpoch()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        auto electionEpochContext = ElectionManager->GetEpochContext();
        EpochContext = New<TEpochContext>();
        EpochContext->LeaderId = electionEpochContext->LeaderId;
        EpochContext->EpochId = electionEpochContext->EpochId;
        EpochContext->StartTime = electionEpochContext->StartTime;
        EpochContext->CancelableContext = electionEpochContext->CancelableContext;
        EpochContext->EpochControlInvoker = EpochContext->CancelableContext->CreateInvoker(ControlInvoker);
        EpochContext->EpochSystemStateInvoker = EpochContext->CancelableContext->CreateInvoker(DecoratedState->GetSystemInvoker());
        EpochContext->EpochUserStateInvoker = EpochContext->CancelableContext->CreateInvoker(StateInvoker);
    }

    void StopEpoch()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);
        
        YCHECK(EpochContext);

        EpochContext->CancelableContext->Cancel();
        auto snapshotBuilder = EpochContext->SnapshotBuilder;
        EpochContext.Reset();

        if (snapshotBuilder) {
            DecoratedState->GetSystemInvoker()->Invoke(BIND(
                &TSnapshotBuilder::WaitUntilFinished,
                snapshotBuilder));
        }
    }

    void CheckEpoch(const TEpochId& epochId) const
    {
        auto currentEpochId = EpochContext->EpochId;
        if (epochId != currentEpochId) {
            THROW_ERROR_EXCEPTION(
                EErrorCode::InvalidEpoch,
                "Invalid epoch: expected %s, received %s",
                ~currentEpochId.ToString(),
                ~epochId.ToString());
        }
    }


    TFollowerTrackerPtr GetFollowerTracker() const
    {
        auto epochContext = EpochContext;
        if (!epochContext) {
            return NULL;
        }

        return epochContext->FollowerTracker;
    }


    TPeerPriority GetPriority() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto version = DecoratedState->GetReachableVersionAsync();
        return ((TPeerPriority) version.SegmentId << 32) | version.RecordCount;
    }

    static Stroka FormatPriority(TPeerPriority priority)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        int segmentId = priority >> 32;
        int recordCount = priority & 0xffffffff;
        return Sprintf("(%d, %d)", segmentId, recordCount);
    }


    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(StateThread);
    DECLARE_THREAD_AFFINITY_SLOT(IOThread);
};

////////////////////////////////////////////////////////////////////////////////

IMetaStateManagerPtr CreatePersistentStateManager(
    TPersistentStateManagerConfigPtr config,
    IInvokerPtr controlInvoker,
    IInvokerPtr stateInvoker,
    IMetaStatePtr metaState,
    NRpc::IServerPtr server)
{
    YCHECK(controlInvoker);
    YCHECK(metaState);
    YCHECK(server);

    return New<TPersistentStateManager>(
        config,
        controlInvoker,
        stateInvoker,
        metaState,
        server);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
