#include "action_manager.h"
#include "bootstrap.h"
#include "bundle_state.h"
#include "config.h"
#include "dynamic_config_manager.h"
#include "helpers.h"
#include "private.h"
#include "public.h"
#include "tablet_action.h"
#include "tablet_balancer.h"

#include <yt/yt/server/lib/cypress_election/election_manager.h>

#include <yt/yt/server/lib/tablet_balancer/config.h>
#include <yt/yt/server/lib/tablet_balancer/balancing_helpers.h>

#include <yt/yt/ytlib/api/native/client.h>

#include <yt/yt/core/tracing/trace_context.h>

namespace NYT::NTabletBalancer {

using namespace NApi;
using namespace NConcurrency;
using namespace NObjectClient;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTracing;
using namespace NYson;
using namespace NYPath;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletBalancerLogger;

////////////////////////////////////////////////////////////////////////////////

static const TString TabletCellBundlesPath("//sys/tablet_cell_bundles");

constexpr static TDuration MinBalanceFrequency = TDuration::Minutes(1);

////////////////////////////////////////////////////////////////////////////////

class TTabletBalancer
    :  public ITabletBalancer
{
public:
    TTabletBalancer(
        IBootstrap* bootstrap,
        TStandaloneTabletBalancerConfigPtr config,
        IInvokerPtr controlInvoker);

    void Start() override;
    void Stop() override;

    IYPathServicePtr GetOrchidService() override;

    void OnDynamicConfigChanged(
        const TTabletBalancerDynamicConfigPtr& oldConfig,
        const TTabletBalancerDynamicConfigPtr& newConfig) override;

private:
    IBootstrap* const Bootstrap_;
    const TStandaloneTabletBalancerConfigPtr Config_;
    const IInvokerPtr ControlInvoker_;
    const TPeriodicExecutorPtr PollExecutor_;
    THashMap<TString, TBundleStatePtr> Bundles_;
    THashSet<TString> BundleNamesToMoveOnNextIteration_;
    IThreadPoolPtr WorkerPool_;
    IActionManagerPtr ActionManager_;

    std::atomic<bool> Enable_{false};
    std::atomic<bool> EnableEverywhere_{false};
    TAtomicObject<TTimeFormula> ScheduleFormula_;

    TInstant CurrentIterationStartTime_;
    TInstant PreviousIterationStartTime_;
    i64 IterationIndex_;

    void BalancerIteration();
    void TryBalancerIteration();

    bool IsBalancingAllowed(const TBundleStatePtr& bundle) const;

    void BalanceViaReshard(const TBundleStatePtr& bundle) const;
    void BalanceViaMove(const TBundleStatePtr& bundle) const;
    void BalanceViaMoveInMemory(const TBundleStatePtr& bundle) const;
    void BalanceViaMoveOrdinary(const TBundleStatePtr& bundle) const;

    std::vector<TString> UpdateBundleList();
    bool HasUntrackedUnfinishedActions(
        const TBundleStatePtr& bundle,
        const IAttributeDictionary* attributes) const;

    bool DidBundleBalancingTimeHappen(const TTabletCellBundlePtr& bundle) const;
    TTimeFormula GetBundleSchedule(const TTabletCellBundlePtr& bundle) const;

    void BuildOrchid(IYsonConsumer* consumer) const;
};

////////////////////////////////////////////////////////////////////////////////

TTabletBalancer::TTabletBalancer(
    IBootstrap* bootstrap,
    TStandaloneTabletBalancerConfigPtr config,
    IInvokerPtr controlInvoker)
    : Bootstrap_(bootstrap)
    , Config_(std::move(config))
    , ControlInvoker_(std::move(controlInvoker))
    , PollExecutor_(New<TPeriodicExecutor>(
        ControlInvoker_,
        BIND(&TTabletBalancer::TryBalancerIteration, MakeWeak(this)),
        Config_->Period))
    , WorkerPool_(CreateThreadPool(
        Config_->WorkerThreadPoolSize,
        "TabletBalancer"))
    , ActionManager_(CreateActionManager(
        Config_->TabletActionExpirationTime,
        Config_->TabletActionPollingPeriod,
        Bootstrap_->GetClient(),
        Bootstrap_))
    , PreviousIterationStartTime_(TruncatedNow())
    , IterationIndex_(0)
{
    bootstrap->GetDynamicConfigManager()->SubscribeConfigChanged(BIND(&TTabletBalancer::OnDynamicConfigChanged, MakeWeak(this)));
}

void TTabletBalancer::Start()
{
    VERIFY_THREAD_AFFINITY_ANY();

    YT_LOG_INFO("Starting tablet balancer instance");

    BundleNamesToMoveOnNextIteration_.clear();

    PollExecutor_->Start();

    ActionManager_->Start(Bootstrap_->GetElectionManager()->GetPrerequisiteTransactionId());
}

void TTabletBalancer::Stop()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    YT_LOG_INFO("Stopping tablet balancer instance");

    PollExecutor_->Stop();
    ActionManager_->Stop();

    YT_LOG_INFO("Tablet balancer instance stopped");
}

void TTabletBalancer::BalancerIteration()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    if (!Enable_) {
        YT_LOG_DEBUG("Standalone tablet balancer is not enabled");
        return;
    }

    YT_LOG_INFO("Balancer iteration (IterationIndex: %v)", IterationIndex_);

    YT_LOG_DEBUG("Started fetching bundles");
    auto newBundles = UpdateBundleList();
    YT_LOG_DEBUG("Finished fetching bundles (NewBundleCount: %v)", newBundles.size());

    CurrentIterationStartTime_ = TruncatedNow();

    for (auto& [bundleName, bundle] : Bundles_) {
        if (bundle->GetHasUntrackedUnfinishedActions() || ActionManager_->HasUnfinishedActions(bundleName)) {
            YT_LOG_DEBUG("Skip balancing iteration since bundle has unfinished actions (BundleName: %v)", bundleName);
            continue;
        }

        YT_LOG_DEBUG("Started fetching (BundleName: %v)", bundleName);

        if (auto result = WaitFor(bundle->UpdateState()); !result.IsOK()) {
            YT_LOG_ERROR(result, "Failed to update meta registry (BundleName: %v)", bundleName);
            continue;
        }

        if (!IsBalancingAllowed(bundle)) {
            YT_LOG_DEBUG("Balancing is disabled (BundleName: %v)", bundleName);
            continue;
        }

        if (auto result = WaitFor(bundle->FetchStatistics()); !result.IsOK()) {
            YT_LOG_ERROR(result, "Fetch statistics failed (BundleName: %v)", bundleName);
            continue;
        }

        // TODO(alexelex): Use Tablets as tablets for each table.

        if (auto it = BundleNamesToMoveOnNextIteration_.find(bundleName); it != BundleNamesToMoveOnNextIteration_.end()) {
            BundleNamesToMoveOnNextIteration_.erase(it);
            BalanceViaMove(bundle);
        } else if (DidBundleBalancingTimeHappen(bundle->GetBundle())) {
            BundleNamesToMoveOnNextIteration_.insert(bundleName);
            BalanceViaReshard(bundle);
        } else {
            YT_LOG_DEBUG("Skip balancing iteration because the time has not yet come (BundleName: %v)", bundleName);
        }

        ActionManager_->CreateActions(bundleName);
    }

    ++IterationIndex_;
    PreviousIterationStartTime_ = CurrentIterationStartTime_;
}

void TTabletBalancer::TryBalancerIteration()
{
    TTraceContextGuard traceContextGuard(TTraceContext::NewRoot("TabletBalancer"));
    YT_PROFILE_TIMING("/tablet_balancer/balancer_iteration_time") {
        try {
            BalancerIteration();
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Balancer iteration failed");
        }
    }
}

bool TTabletBalancer::IsBalancingAllowed(const TBundleStatePtr& bundle) const
{
    return Enable_ &&
        bundle->GetHealth() == ETabletCellHealth::Good &&
        (EnableEverywhere_ ||
         bundle->GetBundle()->Config->EnableStandaloneTabletBalancer);
}

IYPathServicePtr TTabletBalancer::GetOrchidService()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    return IYPathService::FromProducer(BIND(&TTabletBalancer::BuildOrchid, MakeWeak(this)))
        ->Via(ControlInvoker_);
}

void TTabletBalancer::BuildOrchid(IYsonConsumer* consumer) const
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("config").Value(Config_)
        .EndMap();
}

void TTabletBalancer::OnDynamicConfigChanged(
    const TTabletBalancerDynamicConfigPtr& oldConfig,
    const TTabletBalancerDynamicConfigPtr& newConfig)
{
    // Order matters. Otherwise, the old Enable can be seen with the new EnableEverywhere
    // and balance everything, while EnableEverywhere has no effect if Enable is set to false.
    Enable_.store(newConfig->Enable);
    EnableEverywhere_.store(newConfig->EnableEverywhere);
    ScheduleFormula_.Store(newConfig->Schedule);

    YT_LOG_DEBUG(
        "Updated tablet balancer dynamic config (OldConfig: %v, NewConfig: %v)",
        ConvertToYsonString(oldConfig, EYsonFormat::Text),
        ConvertToYsonString(newConfig, EYsonFormat::Text));
}

std::vector<TString> TTabletBalancer::UpdateBundleList()
{
    TListNodeOptions options;
    options.Attributes = {"health", "tablet_balancer_config", "tablet_cell_ids", "tablet_actions"};

    auto bundles = WaitFor(Bootstrap_
        ->GetClient()
        ->ListNode(TabletCellBundlesPath, options))
        .ValueOrThrow();
    auto bundlesList = ConvertTo<IListNodePtr>(bundles);

    // Gather current bundles.
    THashSet<TString> currentBundles;
    std::vector<TString> newBundles;
    for (const auto& bundle : bundlesList->GetChildren()) {
        const auto& name = bundle->AsString()->GetValue();
        currentBundles.insert(bundle->AsString()->GetValue());

        auto [it, isNew] = Bundles_.emplace(
            name,
            New<TBundleState>(
                name,
                Bootstrap_->GetClient(),
                WorkerPool_->GetInvoker()));
        it->second->UpdateBundleAttributes(&bundle->Attributes());
        it->second->SetHasUntrackedUnfinishedActions(HasUntrackedUnfinishedActions(it->second, &bundle->Attributes()));

        if (isNew) {
            newBundles.push_back(name);
        }
    }

    // Find bundles that are not in the list of bundles (probably deleted) and erase them.
    DropMissingKeys(&Bundles_, currentBundles);
    return newBundles;
}

bool TTabletBalancer::HasUntrackedUnfinishedActions(
    const TBundleStatePtr& bundle,
    const IAttributeDictionary* attributes) const
{
    auto actions = attributes->Get<std::vector<IMapNodePtr>>("tablet_actions");
    for (auto actionMapNode : actions) {
        auto state = ConvertTo<ETabletActionState>(actionMapNode->FindChild("state"));
        if (IsTabletActionFinished(state)) {
            continue;
        }

        auto actionId = ConvertTo<TTabletActionId>(actionMapNode->FindChild("tablet_action_id"));
        if (!ActionManager_->IsKnownAction(bundle->GetBundle()->Name, actionId)) {
            return true;
        }
    }
    return false;
}

bool TTabletBalancer::DidBundleBalancingTimeHappen(const TTabletCellBundlePtr& bundle) const
{
    auto formula = GetBundleSchedule(bundle);

    try {
        if (Config_->Period >= MinBalanceFrequency) {
            TInstant timePoint = PreviousIterationStartTime_ + MinBalanceFrequency;
            if (timePoint > CurrentIterationStartTime_) {
                return false;
            }
            while (timePoint <= CurrentIterationStartTime_) {
                if (formula.IsSatisfiedBy(timePoint)) {
                    return true;
                }
                timePoint += MinBalanceFrequency;
            }
            return false;
        } else {
            return formula.IsSatisfiedBy(CurrentIterationStartTime_);
        }
    } catch (const std::exception& ex) {
        YT_LOG_ERROR(ex, "Failed to evaluate tablet balancer schedule formula");
        return false;
    }
}

TTimeFormula TTabletBalancer::GetBundleSchedule(const TTabletCellBundlePtr& bundle) const
{
    const auto& local = bundle->Config->TabletBalancerSchedule;
    if (!local.IsEmpty()) {
        YT_LOG_DEBUG("Using local balancer schedule for bundle (BundleName: %v, ScheduleFormula: %v)",
            bundle->Name,
            local.GetFormula());
        return local;
    }
    auto formula = ScheduleFormula_.Load();
    YT_LOG_DEBUG("Using global balancer schedule for bundle (BundleName: %v, ScheduleFormula: %v)",
        bundle->Name,
        formula.GetFormula());
    return formula;
}

void TTabletBalancer::BalanceViaMoveInMemory(const TBundleStatePtr& bundle) const
{
    YT_LOG_DEBUG("Balancing in memory tablets via move started (BundleName: %v)",
        bundle->GetBundle()->Name);

    if (!bundle->GetBundle()->Config->EnableInMemoryCellBalancer) {
        YT_LOG_DEBUG("Balancing in memory tablets via move is disabled (BundleName: %v)",
            bundle->GetBundle()->Name);
        return;
    }

    auto descriptors = ReassignInMemoryTablets(
        bundle->GetBundle(),
        /*movableTables*/ std::nullopt,
        /*ignoreTableWiseConfig*/ false,
        Logger);

    int actionCount = 0;

    if (!descriptors.empty()) {
        for (auto descriptor : descriptors) {
            YT_LOG_DEBUG("Move action created (TabletId: %v, CellId: %v)",
                descriptor.TabletId,
                descriptor.TabletCellId);
            ActionManager_->ScheduleActionCreation(bundle->GetBundle()->Name, descriptor);

            auto tablet = GetOrCrash(bundle->Tablets(), descriptor.TabletId);
            auto& profilingCounters = GetOrCrash(bundle->ProfilingCounters(), tablet->Table->Id);
            profilingCounters.InMemoryMoves.Increment(1);
        }

        actionCount += std::ssize(descriptors);
    }

    YT_LOG_DEBUG("Balancing in memory tablets via move finished (BundleName: %v, ActionCount: %v)",
        bundle->GetBundle()->Name,
        actionCount);
}

void TTabletBalancer::BalanceViaMoveOrdinary(const TBundleStatePtr& bundle) const
{
    YT_LOG_DEBUG("Balancing ordinary tablets via move started (BundleName: %v)",
        bundle->GetBundle()->Name);

    if (!bundle->GetBundle()->Config->EnableCellBalancer) {
        YT_LOG_DEBUG("Balancing ordinary tablets via move is disabled (BundleName: %v)",
            bundle->GetBundle()->Name);
        return;
    }

    auto descriptors = ReassignOrdinaryTablets(
        bundle->GetBundle(),
        /*movableTables*/ std::nullopt,
        Logger);

    int actionCount = 0;

    if (!descriptors.empty()) {
        for (auto descriptor : descriptors) {
            YT_LOG_DEBUG("Move action created (TabletId: %v, CellId: %v)",
                descriptor.TabletId,
                descriptor.TabletCellId);
            ActionManager_->ScheduleActionCreation(bundle->GetBundle()->Name, descriptor);
        }

        actionCount += std::ssize(descriptors);
    }

    YT_LOG_DEBUG("Balancing ordinary tablets via move finished (BundleName: %v, ActionCount: %v)",
        bundle->GetBundle()->Name,
        actionCount);
}

void TTabletBalancer::BalanceViaMove(const TBundleStatePtr& bundle) const
{
    BalanceViaMoveInMemory(bundle);
    BalanceViaMoveOrdinary(bundle);
}

void TTabletBalancer::BalanceViaReshard(const TBundleStatePtr& bundle) const
{
    YT_LOG_DEBUG("Balancing tablets via reshard started (BundleName: %v)",
        bundle->GetBundle()->Name);

    std::vector<TTabletPtr> tablets;
    for (const auto& [id, tablet] : bundle->Tablets()) {
        if (IsTabletReshardable(tablet, /*ignoreConfig*/ false)) {
            tablets.push_back(tablet);
        }
    }

    std::sort(
        tablets.begin(),
        tablets.end(),
        [&] (const TTabletPtr lhs, const TTabletPtr rhs) {
            return lhs->Table->Id < rhs->Table->Id;
        });

    int actionCount = 0;
    TTabletBalancerContext context;

    auto beginIt = tablets.begin();
    while (beginIt != tablets.end()) {
        auto endIt = beginIt;
        while (endIt != tablets.end() && (*beginIt)->Table == (*endIt)->Table) {
            ++endIt;
        }

        if (TypeFromId((*beginIt)->Table->Id) != EObjectType::Table) {
            beginIt = endIt;
            continue;
        }

        auto& profilingCounters = GetOrCrash(bundle->ProfilingCounters(), (*beginIt)->Table->Id);
        auto tabletRange = MakeRange(beginIt, endIt);
        beginIt = endIt;

        // TODO(alexelex): Check if the table has actions.

        auto descriptors = MergeSplitTabletsOfTable(
            tabletRange,
            &context,
            Logger);

        for (auto descriptor : descriptors) {
            YT_LOG_DEBUG("Reshard action created (TabletIds: %v, TabletCount: %v, DataSize: %v)",
                descriptor.Tablets,
                descriptor.TabletCount,
                descriptor.DataSize);
            ActionManager_->ScheduleActionCreation(bundle->GetBundle()->Name, descriptor);

            if (descriptor.TabletCount == 1) {
                profilingCounters.TabletMerges.Increment(1);
            } else if (std::ssize(descriptor.Tablets) == 1) {
                profilingCounters.TabletSplits.Increment(1);
            } else {
                profilingCounters.NonTrivialReshards.Increment(1);
            }
        }
        actionCount += std::ssize(descriptors);
    }

    YT_LOG_DEBUG("Balancing tablets via reshard finished (BundleName: %v, ActionCount: %v)",
        bundle->GetBundle()->Name,
        actionCount);
}

////////////////////////////////////////////////////////////////////////////////

ITabletBalancerPtr CreateTabletBalancer(
    IBootstrap* bootstrap,
    TStandaloneTabletBalancerConfigPtr config,
    IInvokerPtr controlInvoker)
{
    return New<TTabletBalancer>(bootstrap, config, std::move(controlInvoker));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletBalancer
