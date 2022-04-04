#include "transaction_manager.h"
#include "bootstrap.h"
#include "private.h"
#include "automaton.h"
#include "tablet_slot.h"
#include "transaction.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>
#include <yt/yt/server/node/cluster_node/config.h>

#include <yt/yt/server/lib/hive/transaction_supervisor.h>
#include <yt/yt/server/lib/hive/transaction_lease_tracker.h>
#include <yt/yt/server/lib/hive/transaction_manager_detail.h>

#include <yt/yt/server/lib/hydra/distributed_hydra_manager.h>
#include <yt/yt/server/lib/hydra_common/mutation.h>

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/server/lib/transaction_server/helpers.h>

#include <yt/yt/server/node/tablet_node/transaction_manager.pb.h>

#include <yt/yt/ytlib/transaction_client/action.h>

#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/ytlib/tablet_client/config.h>

#include <yt/yt/ytlib/tablet_client/proto/tablet_service.pb.h>

#include <yt/yt/client/transaction_client/helpers.h>
#include <yt/yt/client/transaction_client/timestamp_provider.h>

#include <yt/yt/client/object_client/helpers.h>

#include <yt/yt/core/concurrency/thread_affinity.h>
#include <yt/yt/core/concurrency/periodic_executor.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/profiling/profile_manager.h>

#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/core/misc/heap.h>
#include <yt/yt/core/misc/ring_queue.h>

#include <util/generic/cast.h>

#include <set>

namespace NYT::NTabletNode {

using namespace NApi;
using namespace NConcurrency;
using namespace NYTree;
using namespace NYson;
using namespace NTransactionClient;
using namespace NTransactionServer;
using namespace NObjectClient;
using namespace NHydra;
using namespace NHiveClient;
using namespace NHiveServer;
using namespace NClusterNode;
using namespace NTabletClient;
using namespace NTabletClient::NProto;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

static constexpr auto ProfilingPeriod = TDuration::Seconds(1);

////////////////////////////////////////////////////////////////////////////////

class TTransactionManager::TImpl
    : public TTabletAutomatonPart
    , public TTransactionManagerBase<TTransaction>
{
public:
    DEFINE_SIGNAL(void(TTransaction*), TransactionStarted);
    DEFINE_SIGNAL(void(TTransaction*, bool), TransactionPrepared);
    DEFINE_SIGNAL(void(TTransaction*), TransactionCommitted);
    DEFINE_SIGNAL(void(TTransaction*), TransactionSerialized);
    DEFINE_SIGNAL(void(TTransaction*), BeforeTransactionSerialized);
    DEFINE_SIGNAL(void(TTransaction*), TransactionAborted);
    DEFINE_SIGNAL(void(TTimestamp), TransactionBarrierHandled);
    DEFINE_SIGNAL(void(TTransaction*), TransactionTransientReset);

public:
    TImpl(
        TTransactionManagerConfigPtr config,
        ITransactionManagerHostPtr host,
        TClusterTag clockClusterTag,
        ITransactionLeaseTrackerPtr transactionLeaseTracker)
        : TTabletAutomatonPart(
            host->GetCellId(),
            host->GetSimpleHydraManager(),
            host->GetAutomaton(),
            host->GetAutomatonInvoker())
        , Host_(host)
        , Config_(config)
        , LeaseTracker_(std::move(transactionLeaseTracker))
        , NativeCellTag_(host->GetNativeCellTag())
        , NativeConnection_(host->GetNativeConnection())
        , ClockClusterTag_(clockClusterTag)
        , TransactionSerializationLagTimer_(TabletNodeProfiler
            .WithTag("cell_id", ToString(host->GetCellId()))
            .Timer("/transaction_serialization_lag"))
        , AbortTransactionIdPool_(Config_->MaxAbortedTransactionPoolSize)
    {
        VERIFY_INVOKER_THREAD_AFFINITY(host->GetAutomatonInvoker(), AutomatonThread);

        Logger = TabletNodeLogger.WithTag("CellId: %v", host->GetCellId());

        YT_LOG_INFO("Set transaction manager clock cluster tag (ClockClusterTag: %v)",
            ClockClusterTag_);

        RegisterLoader(
            "TransactionManager.Keys",
            BIND(&TImpl::LoadKeys, Unretained(this)));
        RegisterLoader(
            "TransactionManager.Values",
            BIND(&TImpl::LoadValues, Unretained(this)));
        RegisterLoader(
            "TransactionManager.Async",
            BIND(&TImpl::LoadAsync, Unretained(this)));

        RegisterSaver(
            ESyncSerializationPriority::Keys,
            "TransactionManager.Keys",
            BIND(&TImpl::SaveKeys, Unretained(this)));
        RegisterSaver(
            ESyncSerializationPriority::Values,
            "TransactionManager.Values",
            BIND(&TImpl::SaveValues, Unretained(this)));
        RegisterSaver(
            EAsyncSerializationPriority::Default,
            "TransactionManager.Async",
            BIND(&TImpl::SaveAsync, Unretained(this)));

        RegisterMethod(BIND(&TImpl::HydraRegisterTransactionActions, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraRegisterTransactionActionsCompat, Unretained(this)));
        RegisterMethod(BIND(&TImpl::HydraHandleTransactionBarrier, Unretained(this)));

        OrchidService_ = IYPathService::FromProducer(BIND(&TImpl::BuildOrchidYson, MakeWeak(this)), TDuration::Seconds(1))
            ->Via(Host_->GetGuardedAutomatonInvoker());
    }

    TTransaction* FindPersistentTransaction(TTransactionId transactionId)
    {
        return PersistentTransactionMap_.Find(transactionId);
    }

    TTransaction* GetPersistentTransaction(TTransactionId transactionId)
    {
        return PersistentTransactionMap_.Get(transactionId);
    }

    TTransaction* GetPersistentTransactionOrThrow(TTransactionId transactionId)
    {
        if (auto* transaction = PersistentTransactionMap_.Find(transactionId)) {
            return transaction;
        }
        ThrowNoSuchTransaction(transactionId);
    }

    TTransaction* FindTransaction(TTransactionId transactionId)
    {
        if (auto* transaction = TransientTransactionMap_.Find(transactionId)) {
            return transaction;
        }
        if (auto* transaction = PersistentTransactionMap_.Find(transactionId)) {
            return transaction;
        }
        return nullptr;
    }

    TTransaction* GetTransactionOrThrow(TTransactionId transactionId)
    {
        auto* transaction = FindTransaction(transactionId);
        if (!transaction) {
            ThrowNoSuchTransaction(transactionId);
        }
        return transaction;
    }

    TTransaction* GetOrCreateTransaction(
        TTransactionId transactionId,
        TTimestamp startTimestamp,
        TDuration timeout,
        bool transient,
        bool* fresh = nullptr)
    {
        if (fresh) {
            *fresh = false;
        }

        if (auto* transaction = TransientTransactionMap_.Find(transactionId)) {
            return transaction;
        }
        if (auto* transaction = PersistentTransactionMap_.Find(transactionId)) {
            return transaction;
        }

        if (transient && AbortTransactionIdPool_.IsRegistered(transactionId)) {
            THROW_ERROR_EXCEPTION("Abort was requested for transaction %v",
                transactionId);
        }

        if (fresh) {
            *fresh = true;
        }

        auto transactionHolder = std::make_unique<TTransaction>(transactionId);
        transactionHolder->SetForeign(CellTagFromId(transactionId) != NativeCellTag_);
        transactionHolder->SetTimeout(timeout);
        transactionHolder->SetStartTimestamp(startTimestamp);
        transactionHolder->SetPersistentState(ETransactionState::Active);
        transactionHolder->SetTransient(transient);
        transactionHolder->AuthenticationIdentity() = NRpc::GetCurrentAuthenticationIdentity();

        ValidateNotDecommissioned(transactionHolder.get());

        auto& map = transient ? TransientTransactionMap_ : PersistentTransactionMap_;
        auto* transaction = map.Insert(transactionId, std::move(transactionHolder));

        if (IsLeader()) {
            CreateLease(transaction);
        }

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction started (TransactionId: %v, StartTimestamp: %llx, StartTime: %v, "
            "Timeout: %v, Transient: %v)",
            transactionId,
            startTimestamp,
            TimestampToInstant(startTimestamp).first,
            timeout,
            transient);

        return transaction;
    }

    TTransaction* MakeTransactionPersistent(TTransactionId transactionId)
    {
        if (auto* transaction = TransientTransactionMap_.Find(transactionId)) {
            ValidateNotDecommissioned(transaction);

            transaction->SetTransient(false);
            if (IsLeader()) {
                CreateLease(transaction);
            }
            auto transactionHolder = TransientTransactionMap_.Release(transactionId);
            PersistentTransactionMap_.Insert(transactionId, std::move(transactionHolder));
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction became persistent (TransactionId: %v)",
                transactionId);
            return transaction;
        }

        if (auto* transaction = PersistentTransactionMap_.Find(transactionId)) {
            YT_VERIFY(!transaction->GetTransient());
            return transaction;
        }

        YT_ABORT();
    }

    void DropTransaction(TTransaction* transaction)
    {
        YT_VERIFY(transaction->GetTransient());

        if (IsLeader()) {
            CloseLease(transaction);
        }

        auto transactionId = transaction->GetId();
        TransientTransactionMap_.Remove(transactionId);

        YT_LOG_DEBUG("Transaction dropped (TransactionId: %v)",
            transactionId);
    }

    std::vector<TTransaction*> GetTransactions()
    {
        std::vector<TTransaction*> transactions;
        for (auto [transactionId, transaction] : TransientTransactionMap_) {
            transactions.push_back(transaction);
        }
        for (auto [transactionId, transaction] : PersistentTransactionMap_) {
            transactions.push_back(transaction);
        }
        return transactions;
    }

    TFuture<void> RegisterTransactionActions(
        TTransactionId transactionId,
        TTimestamp transactionStartTimestamp,
        TDuration transactionTimeout,
        TTransactionSignature signature,
        ::google::protobuf::RepeatedPtrField<NTransactionClient::NProto::TTransactionActionData>&& actions)
    {
        NTabletNode::NProto::TReqRegisterTransactionActions request;
        ToProto(request.mutable_transaction_id(), transactionId);
        request.set_transaction_start_timestamp(transactionStartTimestamp);
        request.set_transaction_timeout(ToProto<i64>(transactionTimeout));
        request.set_signature(signature);
        request.mutable_actions()->Swap(&actions);
        NRpc::WriteAuthenticationIdentityToProto(&request, NRpc::GetCurrentAuthenticationIdentity());

        auto mutation = CreateMutation(HydraManager_, request);
        mutation->SetCurrentTraceContext();
        return mutation->CommitAndLog(Logger).AsVoid();
    }

    IYPathServicePtr GetOrchidService()
    {
        return OrchidService_;
    }


    // ITransactionManager implementation.

    TFuture<void> GetReadyToPrepareTransactionCommit(
        const std::vector<TTransactionId>& /*prerequisiteTransactionIds*/,
        const std::vector<TCellId>& /*cellIdsToSyncWith*/)
    {
        return VoidFuture;
    }

    void PrepareTransactionCommit(
        TTransactionId transactionId,
        bool persistent,
        TTimestamp prepareTimestamp,
        TClusterTag prepareTimestampClusterTag,
        const std::vector<TTransactionId>& /*prerequisiteTransactionIds*/)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        ValidateTimestampClusterTag(
            transactionId,
            prepareTimestampClusterTag,
            prepareTimestamp,
            /*canThrow*/ true);

        TTransaction* transaction;
        ETransactionState state;
        TTransactionSignature signature;
        if (persistent) {
            transaction = GetPersistentTransactionOrThrow(transactionId);
            state = transaction->GetPersistentState();
            signature = transaction->GetPersistentSignature();
        } else {
            transaction = GetTransactionOrThrow(transactionId);
            state = transaction->GetTransientState();
            signature = transaction->GetTransientSignature();
        }

        // Allow preparing transactions in Active and TransientCommitPrepared (for persistent mode) states.
        if (state != ETransactionState::Active &&
            !(persistent && state == ETransactionState::TransientCommitPrepared))
        {
            transaction->ThrowInvalidState();
        }

        if (signature != FinalTransactionSignature) {
            THROW_ERROR_EXCEPTION("Transaction %v is incomplete: expected signature %x, actual signature %x",
                transactionId,
                FinalTransactionSignature,
                signature);
        }

        NRpc::TCurrentAuthenticationIdentityGuard identityGuard(&transaction->AuthenticationIdentity());

        if (persistent) {
            const auto* context = GetCurrentMutationContext();
            // COMPAT(ifsmirnov)
            if (context->Request().Reign >= ToUnderlying(ETabletReign::DiscardStoresRevision)) {
                transaction->SetPrepareRevision(context->GetVersion().ToRevision());
            }
        }

        if (state == ETransactionState::Active) {
            YT_VERIFY(transaction->GetPrepareTimestamp() == NullTimestamp);
            transaction->SetPrepareTimestamp(prepareTimestamp);
            RegisterPrepareTimestamp(transaction);

            if (persistent) {
                transaction->SetPersistentState(ETransactionState::PersistentCommitPrepared);
            } else {
                transaction->SetTransientState(ETransactionState::TransientCommitPrepared);
            }

            TransactionPrepared_.Fire(transaction, persistent);
            RunPrepareTransactionActions(transaction, persistent);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction commit prepared (TransactionId: %v, Persistent: %v, "
                "PrepareTimestamp: %llx@%v)",
                transactionId,
                persistent,
                prepareTimestamp,
                prepareTimestampClusterTag);
        }
    }

    void PrepareTransactionAbort(TTransactionId transactionId, bool force)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        AbortTransactionIdPool_.Register(transactionId);

        auto* transaction = GetTransactionOrThrow(transactionId);

        NRpc::TCurrentAuthenticationIdentityGuard identityGuard(&transaction->AuthenticationIdentity());

        if (transaction->GetTransientState() != ETransactionState::Active && !force) {
            transaction->ThrowInvalidState();
        }

        if (transaction->GetTransientState() == ETransactionState::Active) {
            transaction->SetTransientState(ETransactionState::TransientAbortPrepared);

            YT_LOG_DEBUG("Transaction abort prepared (TransactionId: %v)",
                transactionId);
        }
    }

    void CommitTransaction(
        TTransactionId transactionId,
        TTimestamp commitTimestamp,
        TClusterTag commitTimestampClusterTag)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetPersistentTransactionOrThrow(transactionId);

        ValidateTimestampClusterTag(
            transactionId,
            commitTimestampClusterTag,
            transaction->GetPrepareTimestamp(),
            /*canThrow*/ false);

        transaction->SetCommitTimestampClusterTag(commitTimestampClusterTag);

        // Make a copy, transaction may die.
        auto identity = transaction->AuthenticationIdentity();
        NRpc::TCurrentAuthenticationIdentityGuard identityGuard(&identity);

        auto state = transaction->GetPersistentState();
        if (state == ETransactionState::Committed) {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction is already committed (TransactionId: %v)",
                transactionId);
            return;
        }

        if (state != ETransactionState::Active &&
            state != ETransactionState::PersistentCommitPrepared)
        {
            transaction->ThrowInvalidState();
        }

        if (IsLeader()) {
            CloseLease(transaction);
        }

        transaction->SetCommitTimestamp(commitTimestamp);
        transaction->SetPersistentState(ETransactionState::Committed);

        TransactionCommitted_.Fire(transaction);
        RunCommitTransactionActions(transaction);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction committed (TransactionId: %v, CommitTimestamp: %llx@%v)",
            transactionId,
            commitTimestamp,
            commitTimestampClusterTag);

        FinishTransaction(transaction);

        if (transaction->IsSerializationNeeded()) {
            auto heapTag = GetSerializingTransactionHeapTag(transaction);
            auto& heap = SerializingTransactionHeaps_[heapTag];
            heap.push_back(transaction);
            AdjustHeapBack(heap.begin(), heap.end(), SerializingTransactionHeapComparer);
            UpdateMinCommitTimestamp(heap);
        } else {
            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction removed without serialization (TransactionId: %v)",
                transactionId);
            PersistentTransactionMap_.Remove(transactionId);
        }
    }

    void AbortTransaction(TTransactionId transactionId, bool force)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = GetPersistentTransactionOrThrow(transactionId);

        // Make a copy, transaction may die.
        auto identity = transaction->AuthenticationIdentity();
        NRpc::TCurrentAuthenticationIdentityGuard identityGuard(&identity);

        auto state = transaction->GetPersistentState();
        if (state == ETransactionState::PersistentCommitPrepared && !force) {
            transaction->ThrowInvalidState();
        }

        if (IsLeader()) {
            CloseLease(transaction);
        }

        transaction->SetPersistentState(ETransactionState::Aborted);

        TransactionAborted_.Fire(transaction);
        RunAbortTransactionActions(transaction);

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction aborted (TransactionId: %v, Force: %v)",
            transactionId,
            force);

        FinishTransaction(transaction);
        PersistentTransactionMap_.Remove(transactionId);
    }

    void PingTransaction(TTransactionId transactionId, bool pingAncestors)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        LeaseTracker_->PingTransaction(transactionId, pingAncestors);
    }

    TTimestamp GetMinPrepareTimestamp() const
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return PreparedTransactions_.empty()
            ? Host_->GetLatestTimestamp()
            : PreparedTransactions_.begin()->first;
    }

    TTimestamp GetMinCommitTimestamp() const
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        return MinCommitTimestamp_.value_or(Host_->GetLatestTimestamp());
    }

    void Decommission()
    {
        YT_LOG_DEBUG("Decommission transaction manager");

        Decommissioned_ = true;
    }

    bool IsDecommissioned() const
    {
        return Decommissioned_ && PersistentTransactionMap_.empty();
    }

private:
    const ITransactionManagerHostPtr Host_;
    const TTransactionManagerConfigPtr Config_;
    const ITransactionLeaseTrackerPtr LeaseTracker_;
    const TCellTag NativeCellTag_;
    const NNative::IConnectionPtr NativeConnection_;
    const TClusterTag ClockClusterTag_;

    NProfiling::TEventTimer TransactionSerializationLagTimer_;

    TEntityMap<TTransaction> PersistentTransactionMap_;
    TEntityMap<TTransaction> TransientTransactionMap_;

    NConcurrency::TPeriodicExecutorPtr ProfilingExecutor_;
    NConcurrency::TPeriodicExecutorPtr BarrierCheckExecutor_;

    THashMap<TCellTag, std::vector<TTransaction*>> SerializingTransactionHeaps_;
    THashMap<TCellTag, TTimestamp> LastSerializedCommitTimestamps_;
    TTimestamp TransientBarrierTimestamp_ = MinTimestamp;
    std::optional<TTimestamp> MinCommitTimestamp_;

    bool Decommissioned_ = false;
    ETabletReign SnapshotReign_ = TEnumTraits<ETabletReign>::GetMaxValue();


    IYPathServicePtr OrchidService_;

    std::set<std::pair<TTimestamp, TTransaction*>> PreparedTransactions_;

    TTransactionIdPool AbortTransactionIdPool_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void BuildOrchidYson(IYsonConsumer* consumer)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto dumpTransaction = [&] (TFluentMap fluent, const std::pair<TTransactionId, TTransaction*>& pair) {
            auto* transaction = pair.second;
            fluent
                .Item(ToString(transaction->GetId())).BeginMap()
                    .Item("transient").Value(transaction->GetTransient())
                    .Item("timeout").Value(transaction->GetTimeout())
                    .Item("state").Value(transaction->GetTransientState())
                    .Item("start_timestamp").Value(transaction->GetStartTimestamp())
                    .Item("prepare_timestamp").Value(transaction->GetPrepareTimestamp())
                    // Omit CommitTimestamp, it's typically null.
                    .Item("locked_row_count").Value(transaction->LockedRows().size())
                    .Item("prelocked_row_count").Value(transaction->PrelockedRows().size())
                    .Item("immediate_locked_write_log_size").Value(transaction->ImmediateLockedWriteLog().Size())
                    .Item("immediate_lockless_write_log_size").Value(transaction->ImmediateLocklessWriteLog().Size())
                    .Item("delayed_write_log_size").Value(transaction->DelayedLocklessWriteLog().Size())
                .EndMap();
        };
        BuildYsonFluently(consumer)
            .BeginMap()
                .DoFor(TransientTransactionMap_, dumpTransaction)
                .DoFor(PersistentTransactionMap_, dumpTransaction)
            .EndMap();
    }

    void CreateLease(TTransaction* transaction)
    {
        if (transaction->GetHasLease()) {
            return;
        }

        auto invoker = Host_->GetEpochAutomatonInvoker();

        LeaseTracker_->RegisterTransaction(
            transaction->GetId(),
            NullTransactionId,
            transaction->GetTimeout(),
            /* deadline */ std::nullopt,
            BIND(&TImpl::OnTransactionExpired, MakeStrong(this))
                .Via(invoker));
        transaction->SetHasLease(true);
    }

    void CloseLease(TTransaction* transaction)
    {
        if (!transaction->GetHasLease()) {
            return;
        }

        LeaseTracker_->UnregisterTransaction(transaction->GetId());

        transaction->SetHasLease(false);
    }


    void OnTransactionExpired(TTransactionId id)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        auto* transaction = FindTransaction(id);
        if (!transaction) {
            return;
        }

        if (transaction->GetTransientState() != ETransactionState::Active) {
            return;
        }

        const auto& transactionSupervisor = Host_->GetTransactionSupervisor();
        transactionSupervisor->AbortTransaction(id).Subscribe(BIND([=] (const TError& error) {
            if (!error.IsOK()) {
                YT_LOG_DEBUG(error, "Error aborting expired transaction (TransactionId: %v)",
                    id);
            }
        }));
    }

    void FinishTransaction(TTransaction* transaction)
    {
        UnregisterPrepareTimestamp(transaction);
    }


    void OnAfterSnapshotLoaded() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TCompositeAutomatonPart::OnAfterSnapshotLoaded();

        SerializingTransactionHeaps_.clear();
        for (auto [transactionId, transaction] : PersistentTransactionMap_) {
            auto state = transaction->GetPersistentState();
            YT_VERIFY(transaction->GetTransientState() == state);
            YT_VERIFY(state != ETransactionState::Aborted);
            if (state == ETransactionState::Committed && transaction->IsSerializationNeeded()) {
                auto heapTag = GetSerializingTransactionHeapTag(transaction);
                SerializingTransactionHeaps_[heapTag].push_back(transaction);
            }
            if (state == ETransactionState::PersistentCommitPrepared) {
                RegisterPrepareTimestamp(transaction);
            }
        }
        for (auto& [_, heap] : SerializingTransactionHeaps_) {
            MakeHeap(heap.begin(), heap.end(), SerializingTransactionHeapComparer);
            UpdateMinCommitTimestamp(heap);
        }
    }

    void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TCompositeAutomatonPart::OnLeaderActive();

        YT_VERIFY(TransientTransactionMap_.GetSize() == 0);

        // Recreate leases for all active transactions.
        for (auto [transactionId, transaction] : PersistentTransactionMap_) {
            auto state = transaction->GetPersistentState();
            if (state == ETransactionState::Active ||
                state == ETransactionState::PersistentCommitPrepared)
            {
                CreateLease(transaction);
            }
        }

        TransientBarrierTimestamp_ = MinTimestamp;

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Host_->GetEpochAutomatonInvoker(),
            BIND(&TImpl::OnProfiling, MakeWeak(this)),
            ProfilingPeriod);
        ProfilingExecutor_->Start();

        BarrierCheckExecutor_ = New<TPeriodicExecutor>(
            Host_->GetEpochAutomatonInvoker(),
            BIND(&TImpl::OnPeriodicBarrierCheck, MakeWeak(this)),
            Config_->BarrierCheckPeriod);
        BarrierCheckExecutor_->Start();

        LeaseTracker_->Start();
    }

    void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TCompositeAutomatonPart::OnStopLeading();

        if (ProfilingExecutor_) {
            ProfilingExecutor_->Stop();
            ProfilingExecutor_.Reset();
        }

        if (BarrierCheckExecutor_) {
            BarrierCheckExecutor_->Stop();
            BarrierCheckExecutor_.Reset();
        }

        // Drop all transient transactions.
        for (auto [transactionId, transaction] : TransientTransactionMap_) {
            transaction->ResetFinished();
            TransactionTransientReset_.Fire(transaction);
            UnregisterPrepareTimestamp(transaction);
        }
        TransientTransactionMap_.Clear();

        // Reset all transiently prepared persistent transactions back into active state.
        // Mark all transactions as finished to release pending readers.
        for (auto [transactionId, transaction] : PersistentTransactionMap_) {
            if (transaction->GetTransientState() == ETransactionState::TransientCommitPrepared) {
                UnregisterPrepareTimestamp(transaction);
                transaction->SetPrepareTimestamp(NullTimestamp);
            }
            transaction->SetPersistentState(transaction->GetPersistentState());
            transaction->SetTransientSignature(transaction->GetPersistentSignature());
            transaction->SetTransientGeneration(transaction->GetPersistentGeneration());
            transaction->ResetFinished();
            TransactionTransientReset_.Fire(transaction);
            CloseLease(transaction);
        }

        LeaseTracker_->Stop();
    }


    void SaveKeys(TSaveContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        PersistentTransactionMap_.SaveKeys(context);
    }

    void SaveValues(TSaveContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        using NYT::Save;
        PersistentTransactionMap_.SaveValues(context);
        Save(context, LastSerializedCommitTimestamps_);
        Save(context, Decommissioned_);
    }

    TCallback<void(TSaveContext&)> SaveAsync()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        std::vector<std::pair<TTransactionId, TCallback<void(TSaveContext&)>>> capturedTransactions;
        for (auto [transactionId, transaction] : PersistentTransactionMap_) {
            capturedTransactions.push_back(std::make_pair(transaction->GetId(), transaction->AsyncSave()));
        }

        return BIND([capturedTransactions = std::move(capturedTransactions)] (TSaveContext& context) {
                using NYT::Save;
                // NB: This is not stable.
                for (const auto& [transactionId, callback] : capturedTransactions) {
                    Save(context, transactionId);
                    callback.Run(context);
                }
            });
    }


    void LoadKeys(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        PersistentTransactionMap_.LoadKeys(context);

        SnapshotReign_ = context.GetVersion();
        Automaton_->RememberReign(static_cast<NHydra::TReign>(SnapshotReign_));
    }

    void LoadValues(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        using NYT::Load;
        PersistentTransactionMap_.LoadValues(context);
        Load(context, LastSerializedCommitTimestamps_);
        Load(context, Decommissioned_);
    }

    void LoadAsync(TLoadContext& context)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        SERIALIZATION_DUMP_WRITE(context, "transactions[%v]", PersistentTransactionMap_.size());
        SERIALIZATION_DUMP_INDENT(context) {
            for (int index = 0; index < std::ssize(PersistentTransactionMap_); ++index) {
                auto transactionId = Load<TTransactionId>(context);
                SERIALIZATION_DUMP_WRITE(context, "%v =>", transactionId);
                SERIALIZATION_DUMP_INDENT(context) {
                    auto* transaction = GetPersistentTransaction(transactionId);
                    transaction->AsyncLoad(context);
                }
            }
        }
    }


    void Clear() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TCompositeAutomatonPart::Clear();

        TransientTransactionMap_.Clear();
        PersistentTransactionMap_.Clear();
        SerializingTransactionHeaps_.clear();
        PreparedTransactions_.clear();
        LastSerializedCommitTimestamps_.clear();
        MinCommitTimestamp_.reset();
    }


    void HydraRegisterTransactionActions(NTabletNode::NProto::TReqRegisterTransactionActions* request)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto transactionStartTimestamp = request->transaction_start_timestamp();
        auto transactionTimeout = FromProto<TDuration>(request->transaction_timeout());
        auto signature = request->signature();

        auto identity = NRpc::ParseAuthenticationIdentityFromProto(*request);
        NRpc::TCurrentAuthenticationIdentityGuard identityGuard(&identity);

        auto* transaction = GetOrCreateTransaction(
            transactionId,
            transactionStartTimestamp,
            transactionTimeout,
            false);

        auto state = transaction->GetPersistentState();
        if (state != ETransactionState::Active) {
            transaction->ThrowInvalidState();
        }

        for (const auto& protoData : request->actions()) {
            auto data = FromProto<TTransactionActionData>(protoData);
            transaction->Actions().push_back(data);

            YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction action registered (TransactionId: %v, ActionType: %v)",
                transactionId,
                data.Type);
        }

        transaction->SetPersistentSignature(transaction->GetPersistentSignature() + signature);
    }

    // COMPAT(babenko)
    void HydraRegisterTransactionActionsCompat(NTabletClient::NProto::TReqRegisterTransactionActions* request)
    {
        NTabletNode::NProto::TReqRegisterTransactionActions newRequest;
        newRequest.mutable_transaction_id()->CopyFrom(request->transaction_id());
        newRequest.set_transaction_start_timestamp(request->transaction_start_timestamp());
        newRequest.set_transaction_timeout(request->transaction_timeout());
        newRequest.set_signature(request->signature());
        newRequest.mutable_actions()->CopyFrom(request->actions());
        HydraRegisterTransactionActions(&newRequest);
    }

    void HydraHandleTransactionBarrier(NTabletNode::NProto::TReqHandleTransactionBarrier* request)
    {
        auto barrierTimestamp = request->timestamp();

        YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Handling transaction barrier (Timestamp: %llx)",
            barrierTimestamp);

        for (auto& [_, heap ]: SerializingTransactionHeaps_) {
            while (!heap.empty()) {
                auto* transaction = heap.front();
                auto commitTimestamp = transaction->GetCommitTimestamp();
                if (commitTimestamp > barrierTimestamp) {
                    break;
                }

                UpdateLastSerializedCommitTimestamp(transaction);

                auto transactionId = transaction->GetId();
                YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(), "Transaction serialized (TransactionId: %v, CommitTimestamp: %llx)",
                    transaction->GetId(),
                    commitTimestamp);

                transaction->SetPersistentState(ETransactionState::Serialized);
                BeforeTransactionSerialized_.Fire(transaction);

                // NB: Explicitly run serialize actions before actual serializing.
                RunSerializeTransactionActions(transaction);
                TransactionSerialized_.Fire(transaction);

                PersistentTransactionMap_.Remove(transactionId);

                ExtractHeap(heap.begin(), heap.end(), SerializingTransactionHeapComparer);
                heap.pop_back();
            }
        }

        MinCommitTimestamp_.reset();
        for (const auto& heap : SerializingTransactionHeaps_) {
            UpdateMinCommitTimestamp(heap.second);
        }

        // YT-8542: It is important to update this timestamp only _after_ all relevant transactions are serialized.
        // See TTableReplicator.
        // Note that runtime data may be missing in unittests.
        if (const auto& runtimeData = Host_->GetRuntimeData()) {
            runtimeData->BarrierTimestamp.store(barrierTimestamp);
        }

        TransactionBarrierHandled_.Fire(barrierTimestamp);
    }

    TDuration ComputeTransactionSerializationLag() const
    {
        if (PreparedTransactions_.empty()) {
            return TDuration::Zero();
        }

        auto latestTimestamp = Host_->GetLatestTimestamp();
        auto minPrepareTimestamp = PreparedTransactions_.begin()->first;
        if (minPrepareTimestamp > latestTimestamp) {
            return TDuration::Zero();
        }

        return TimestampDiffToDuration(minPrepareTimestamp, latestTimestamp).second;
    }


    void OnProfiling()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TransactionSerializationLagTimer_.Record(ComputeTransactionSerializationLag());
    }


    void OnPeriodicBarrierCheck()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        YT_LOG_DEBUG("Running periodic barrier check (BarrierTimestamp: %llx, MinPrepareTimestamp: %llx)",
            TransientBarrierTimestamp_,
            GetMinPrepareTimestamp());

        CheckBarrier();
    }

    void CheckBarrier()
    {
        if (!IsLeader()) {
            return;
        }

        auto minPrepareTimestamp = GetMinPrepareTimestamp();
        if (minPrepareTimestamp <= TransientBarrierTimestamp_) {
            return;
        }

        NTracing::TNullTraceContextGuard guard;

        YT_LOG_DEBUG("Committing transaction barrier (Timestamp: %llx -> %llx)",
            TransientBarrierTimestamp_,
            minPrepareTimestamp);

        TransientBarrierTimestamp_ = minPrepareTimestamp;

        NTabletNode::NProto::TReqHandleTransactionBarrier request;
        request.set_timestamp(TransientBarrierTimestamp_);
        CreateMutation(HydraManager_, request)
            ->CommitAndLog(Logger);
    }

    bool IsOldHydraContext(ETabletReign reign)
    {
        if (const auto* mutationContext = TryGetCurrentMutationContext();
            mutationContext && mutationContext->Request().Reign < ToUnderlying(reign))
        {
            return true;
        }

        if (const auto* snapshotContext = TryGetCurrentHydraContext();
            snapshotContext && SnapshotReign_ < reign)
        {
            return true;
        }

        return false;
    }

    void RegisterPrepareTimestamp(TTransaction* transaction)
    {
        // COMPAT(savrus)
        if (IsOldHydraContext(ETabletReign::SerializeForeign)) {
            if (transaction->GetForeign()) {
                return;
            }
        }

        auto prepareTimestamp = transaction->GetPrepareTimestamp();
        if (prepareTimestamp == NullTimestamp) {
            return;
        }
        YT_VERIFY(PreparedTransactions_.emplace(prepareTimestamp, transaction).second);
    }

    void UnregisterPrepareTimestamp(TTransaction* transaction)
    {
        // COMPAT(savrus)
        if (IsOldHydraContext(ETabletReign::SerializeForeign)) {
            if (transaction->GetForeign()) {
                return;
            }
        }

        auto prepareTimestamp = transaction->GetPrepareTimestamp();
        if (prepareTimestamp == NullTimestamp) {
            return;
        }
        auto pair = std::make_pair(prepareTimestamp, transaction);
        auto it = PreparedTransactions_.find(pair);
        YT_VERIFY(it != PreparedTransactions_.end());
        PreparedTransactions_.erase(it);
        CheckBarrier();
    }

    void UpdateLastSerializedCommitTimestamp(TTransaction* transaction)
    {
        auto commitTimestamp = transaction->GetCommitTimestamp();
        auto cellTag = transaction->GetCellTag();

        if (auto lastTimestampIt = LastSerializedCommitTimestamps_.find(cellTag)) {
            YT_VERIFY(commitTimestamp > lastTimestampIt->second);
            lastTimestampIt->second = commitTimestamp;
        } else {
            YT_VERIFY(LastSerializedCommitTimestamps_.emplace(cellTag, commitTimestamp).second);
        }
    }

    void UpdateMinCommitTimestamp(const std::vector<TTransaction*>& heap)
    {
        if (heap.empty()) {
            return;
        }

        auto timestamp = heap.front()->GetCommitTimestamp();
        MinCommitTimestamp_ = std::min(timestamp, MinCommitTimestamp_.value_or(timestamp));
    }

    void ValidateNotDecommissioned(TTransaction* transaction)
    {
        if (!Decommissioned_) {
            return;
        }

        if (TypeFromId(transaction->GetId()) == EObjectType::Transaction &&
            transaction->AuthenticationIdentity() == GetRootAuthenticationIdentity())
        {
            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Allow transaction in decommissioned state to proceed "
                "(TransactionId: %v, AuthenticationIdentity: %v)",
                transaction->GetId(),
                transaction->AuthenticationIdentity());
            return;
        }

        THROW_ERROR_EXCEPTION("Tablet cell is decommissioned");
    }

    void ValidateTimestampClusterTag(
        TTransactionId transactionId,
        TClusterTag timestampClusterTag,
        TTimestamp prepareTimestamp,
        bool canThrow)
    {
        if (prepareTimestamp == NullTimestamp) {
            return;
        }

        // COMPAT(savrus) Remove as soon as deployed on ada and socrates.
        if (IsMasterTransactionId(transactionId)) {
            canThrow = false;
        }

        if (ClockClusterTag_ == InvalidCellTag || timestampClusterTag == InvalidCellTag) {
            return;
        }

        if (ClockClusterTag_ != timestampClusterTag) {
            if (Config_->RejectIncorrectClockClusterTag && canThrow) {
                THROW_ERROR_EXCEPTION("Transaction timestamp is generated from unexpected clock")
                    << TErrorAttribute("transaction_id", transactionId)
                    << TErrorAttribute("timestamp_cluster_tag", timestampClusterTag)
                    << TErrorAttribute("clock_cluster_tag", ClockClusterTag_);
            }

            YT_LOG_ALERT_IF(IsMutationLoggingEnabled(), "Transaction timestamp is generated from unexpected clock (TransactionId: %v, TransactionClusterTag: %v, ClockClusterTag: %v)",
                transactionId,
                timestampClusterTag,
                ClockClusterTag_);
        }
    }

    TCellTag GetSerializingTransactionHeapTag(TTransaction* transaction)
    {
        // COMPAT(savrus)
        if (IsOldHydraContext(ETabletReign::SerializeReplicationProgress)) {
            return transaction->GetCellTag();
        }

        return transaction->GetCommitTimestampClusterTag() != InvalidCellTag
            ? transaction->GetCommitTimestampClusterTag()
            : transaction->GetCellTag();
    }

    static bool SerializingTransactionHeapComparer(
        const TTransaction* lhs,
        const TTransaction* rhs)
    {
        YT_ASSERT(lhs->GetPersistentState() == ETransactionState::Committed);
        YT_ASSERT(rhs->GetPersistentState() == ETransactionState::Committed);
        return lhs->GetCommitTimestamp() < rhs->GetCommitTimestamp();
    }
};

////////////////////////////////////////////////////////////////////////////////

TTransactionManager::TTransactionManager(
    TTransactionManagerConfigPtr config,
    ITransactionManagerHostPtr host,
    TClusterTag clockClusterTag,
    ITransactionLeaseTrackerPtr transactionLeaseTracker)
    : Impl_(New<TImpl>(
        std::move(config),
        std::move(host),
        clockClusterTag,
        std::move(transactionLeaseTracker)))
{ }

TTransactionManager::~TTransactionManager() = default;

IYPathServicePtr TTransactionManager::GetOrchidService()
{
    return Impl_->GetOrchidService();
}

TTransaction* TTransactionManager::GetOrCreateTransaction(
    TTransactionId transactionId,
    TTimestamp startTimestamp,
    TDuration timeout,
    bool transient,
    bool* fresh)
{
    return Impl_->GetOrCreateTransaction(
        transactionId,
        startTimestamp,
        timeout,
        transient,
        fresh);
}

TTransaction* TTransactionManager::MakeTransactionPersistent(TTransactionId transactionId)
{
    return Impl_->MakeTransactionPersistent(transactionId);
}

void TTransactionManager::DropTransaction(TTransaction* transaction)
{
    Impl_->DropTransaction(transaction);
}

std::vector<TTransaction*> TTransactionManager::GetTransactions()
{
    return Impl_->GetTransactions();
}

TFuture<void> TTransactionManager::RegisterTransactionActions(
    TTransactionId transactionId,
    TTimestamp transactionStartTimestamp,
    TDuration transactionTimeout,
    TTransactionSignature signature,
    ::google::protobuf::RepeatedPtrField<NTransactionClient::NProto::TTransactionActionData>&& actions)
{
    return Impl_->RegisterTransactionActions(
        transactionId,
        transactionStartTimestamp,
        transactionTimeout,
        signature,
        std::move(actions));
}

void TTransactionManager::RegisterTransactionActionHandlers(
    const TTransactionPrepareActionHandlerDescriptor<TTransaction>& prepareActionDescriptor,
    const TTransactionCommitActionHandlerDescriptor<TTransaction>& commitActionDescriptor,
    const TTransactionAbortActionHandlerDescriptor<TTransaction>& abortActionDescriptor)
{
    Impl_->RegisterTransactionActionHandlers(
        prepareActionDescriptor,
        commitActionDescriptor,
        abortActionDescriptor);
}

void TTransactionManager::RegisterTransactionActionHandlers(
    const TTransactionPrepareActionHandlerDescriptor<TTransaction>& prepareActionDescriptor,
    const TTransactionCommitActionHandlerDescriptor<TTransaction>& commitActionDescriptor,
    const TTransactionAbortActionHandlerDescriptor<TTransaction>& abortActionDescriptor,
    const TTransactionSerializeActionHandlerDescriptor<TTransaction>& serializeActionDescriptor)
{
    Impl_->RegisterTransactionActionHandlers(
        prepareActionDescriptor,
        commitActionDescriptor,
        abortActionDescriptor,
        serializeActionDescriptor);
}

TFuture<void> TTransactionManager::GetReadyToPrepareTransactionCommit(
    const std::vector<TTransactionId>& prerequisiteTransactionIds,
    const std::vector<TCellId>& cellIdsToSyncWith)
{
    return Impl_->GetReadyToPrepareTransactionCommit(prerequisiteTransactionIds, cellIdsToSyncWith);
}

void TTransactionManager::PrepareTransactionCommit(
    TTransactionId transactionId,
    bool persistent,
    TTimestamp prepareTimestamp,
    TClusterTag prepareTimestampClusterTag,
    const std::vector<TTransactionId>& prerequisiteTransactionIds)
{
    Impl_->PrepareTransactionCommit(transactionId, persistent, prepareTimestamp, prepareTimestampClusterTag, prerequisiteTransactionIds);
}

void TTransactionManager::PrepareTransactionAbort(TTransactionId transactionId, bool force)
{
    Impl_->PrepareTransactionAbort(transactionId, force);
}

void TTransactionManager::CommitTransaction(
    TTransactionId transactionId,
    TTimestamp commitTimestamp,
    TClusterTag commitTimestampClusterTag)
{
    Impl_->CommitTransaction(transactionId, commitTimestamp, commitTimestampClusterTag);
}

void TTransactionManager::AbortTransaction(TTransactionId transactionId, bool force)
{
    Impl_->AbortTransaction(transactionId, force);
}

void TTransactionManager::PingTransaction(TTransactionId transactionId, bool pingAncestors)
{
    Impl_->PingTransaction(transactionId, pingAncestors);
}

TTimestamp TTransactionManager::GetMinPrepareTimestamp()
{
    return Impl_->GetMinPrepareTimestamp();
}

TTimestamp TTransactionManager::GetMinCommitTimestamp()
{
    return Impl_->GetMinCommitTimestamp();
}

void TTransactionManager::Decommission()
{
    Impl_->Decommission();
}

bool TTransactionManager::IsDecommissioned() const
{
    return Impl_->IsDecommissioned();
}

DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionStarted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*, bool), TransactionPrepared, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionCommitted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionSerialized, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), BeforeTransactionSerialized, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionAborted, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTimestamp), TransactionBarrierHandled, *Impl_);
DELEGATE_SIGNAL(TTransactionManager, void(TTransaction*), TransactionTransientReset, *Impl_)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
