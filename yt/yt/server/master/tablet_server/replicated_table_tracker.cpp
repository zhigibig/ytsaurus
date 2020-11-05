#include "private.h"
#include "replicated_table_tracker.h"

#include <yt/core/concurrency/thread_pool.h>
#include <yt/core/concurrency/scheduler_thread.h>

#include <yt/core/ytree/ypath_proxy.h>

#include <yt/core/rpc/helpers.h>

#include <yt/core/misc/async_expiring_cache.h>

#include <yt/server/master/cell_master/bootstrap.h>
#include <yt/server/master/cell_master/config.h>
#include <yt/server/master/cell_master/config_manager.h>
#include <yt/server/master/cell_master/hydra_facade.h>
#include <yt/server/master/cell_master/world_initializer.h>

#include <yt/server/master/table_server/replicated_table_node.h>

#include <yt/server/master/tablet_server/config.h>
#include <yt/server/master/tablet_server/table_replica.h>

#include <yt/server/master/cypress_server/cypress_manager.h>

#include <yt/server/master/object_server/object_manager.h>

#include <yt/server/lib/hydra/composite_automaton.h>

#include <yt/server/master/hive/cluster_directory_synchronizer.h>
#include <yt/server/lib/hive/config.h>

#include <yt/ytlib/api/native/client.h>
#include <yt/ytlib/api/native/config.h>
#include <yt/ytlib/api/native/connection.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/ytlib/tablet_client/table_replica_ypath.h>

#include <yt/ytlib/hive/cluster_directory.h>

#include <yt/client/api/public.h>
#include <yt/client/transaction_client/timestamp_provider.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/ypath/token.h>

#include <yt/core/misc/atomic_object.h>

namespace NYT::NTabletServer {

using namespace NCellMaster;
using namespace NConcurrency;
using namespace NSecurityClient;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NTableServer;
using namespace NHiveClient;
using namespace NHiveServer;
using namespace NTabletClient;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NApi;
using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = TabletServerLogger;
static const auto& Profiler = TabletServerProfiler;

////////////////////////////////////////////////////////////////////////////////

struct TClusterStateKey
{
    NApi::IClientPtr Client;
    TString ClusterName; // for diagnostics only

    bool operator==(const TClusterStateKey& other) const
    {
        return Client == other.Client;
    }

    operator size_t() const
    {
        size_t result = 0;
        HashCombine(result, Client);
        return result;
    }
};

void FormatValue(TStringBuilderBase* builder, const TClusterStateKey& key, TStringBuf /*spec*/)
{
    builder->AppendFormat("%v", key.ClusterName);
}

TString ToString(const TClusterStateKey& key)
{
    return ToStringViaBuilder(key);
}

////////////////////////////////////////////////////////////////////////////////

struct TBundleHealthKey
{
    NApi::IClientPtr Client;
    TString ClusterName; // for diagnostics only
    TString BundleName;

    bool operator==(const TBundleHealthKey& other) const
    {
        return
            Client == other.Client &&
            BundleName == other.BundleName;
    }

    operator size_t() const
    {
        size_t result = 0;
        HashCombine(result, Client);
        HashCombine(result, BundleName);
        return result;
    }
};

void FormatValue(TStringBuilderBase* builder, const TBundleHealthKey& key, TStringBuf /*spec*/)
{
    builder->AppendFormat("%v@%v",
        key.BundleName,
        key.ClusterName);
}

TString ToString(const TBundleHealthKey& key)
{
    return ToStringViaBuilder(key);
}

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TBundleHealthCache)

class TBundleHealthCache
    : public TAsyncExpiringCache<TBundleHealthKey, ETabletCellHealth>
{
public:
    explicit TBundleHealthCache(TAsyncExpiringCacheConfigPtr config)
        : TAsyncExpiringCache(
            std::move(config),
            NLogging::TLogger(TabletServerLogger)
                .AddTag("Cache: BundleHealth"))
    { }

protected:
    virtual TFuture<ETabletCellHealth> DoGet(
        const TBundleHealthKey& key,
        bool /*isPeriodicUpdate*/) noexcept override
    {
        return key.Client->GetNode("//sys/tablet_cell_bundles/" + ToYPathLiteral(key.BundleName) + "/@health").ToUncancelable()
            .Apply(BIND([] (const TErrorOr<TYsonString>& error) {
                return ConvertTo<ETabletCellHealth>(error.ValueOrThrow());
            }));
    }
};

DEFINE_REFCOUNTED_TYPE(TBundleHealthCache)

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TClusterStateCache)

class TClusterStateCache
    : public TAsyncExpiringCache<TClusterStateKey, void>
{
public:
    explicit TClusterStateCache(TAsyncExpiringCacheConfigPtr config)
        : TAsyncExpiringCache(
            std::move(config),
            NLogging::TLogger(TabletServerLogger)
                .AddTag("Cache: ClusterLivenessCheck"))
    { }

protected:
    virtual TFuture<void> DoGet(
        const TClusterStateKey& key,
        bool /*isPeriodicUpdate*/) noexcept override
    {
        return AllSucceeded(std::vector<TFuture<void>>{
            CheckClusterLiveness(key),
            CheckClusterSafeMode(key),
            CheckHydraIsReadOnly(key)});
    }

private:
    TFuture<void> CheckClusterLiveness(const TClusterStateKey& key) const
    {
        TCheckClusterLivenessOptions options;
        options.CheckCypressRoot = true;
        return key.Client->CheckClusterLiveness(options)
            .Apply(BIND([clusterName = key.ClusterName] (const TErrorOr<void>& result) {
                THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error checking cluster %Qlv liveness",
                    clusterName);
            }));
    }

    TFuture<void> CheckClusterSafeMode(const TClusterStateKey& key) const
    {
        return key.Client->GetNode("//sys/@config/enable_safe_mode")
            .Apply(BIND([clusterName = key.ClusterName] (const TErrorOr<TYsonString>& error) {
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    error,
                    "Error getting enable_safe_mode attribute for cluster %Qlv",
                    clusterName);
                if (auto isSafeModeEnabled = ConvertTo<bool>(error.Value())) {
                    THROW_ERROR_EXCEPTION("Safe mode is enabled for cluster %Qlv", clusterName);
                }
            }));
    }

    TFuture<void> CheckHydraIsReadOnly(const TClusterStateKey& key) const
    {
        return key.Client->GetNode("//sys/@hydra_read_only")
            .Apply(BIND([clusterName = key.ClusterName] (const TErrorOr<TYsonString>& error) {
                if (error.FindMatching(NYTree::EErrorCode::ResolveError)) {
                    return;
                }
                THROW_ERROR_EXCEPTION_IF_FAILED(
                    error,
                    "Error getting hydra_read_only attribute for cluster %Qlv",
                    clusterName);
                if (auto isHydraReadOnly = ConvertTo<bool>(error.Value())) {
                    THROW_ERROR_EXCEPTION("Hydra read only mode is activated for cluster %Qlv", clusterName);
                }
            }));
    }
};

DEFINE_REFCOUNTED_TYPE(TClusterStateCache)

////////////////////////////////////////////////////////////////////////////////

class TReplicatedTableTracker::TImpl
    : public TMasterAutomatonPart
{
public:
    TImpl(TReplicatedTableTrackerConfigPtr config, TBootstrap* bootstrap)
        : TMasterAutomatonPart(bootstrap, EAutomatonThreadQueue::ReplicatedTableTracker)
        , Config_(std::move(config))
        , BundleHealthCache_(New<TBundleHealthCache>(BundleHealthCacheConfig_))
        , ClusterStateCache_(New<TClusterStateCache>(ClusterStateCacheConfig_))
        , CheckerThreadPool_(New<TThreadPool>(Config_->CheckerThreadCount, "RplTableTracker"))
    {
        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(EAutomatonThreadQueue::ReplicatedTableTracker), AutomatonThread);
        VERIFY_INVOKER_THREAD_AFFINITY(CheckerThreadPool_->GetInvoker(), CheckerThread);

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        cypressManager->SubscribeNodeCreated(BIND(&TImpl::OnNodeCreated, MakeStrong(this)));

        const auto& configManager = Bootstrap_->GetConfigManager();
        configManager->SubscribeConfigChanged(BIND(&TImpl::OnDynamicConfigChanged, MakeWeak(this)));
    }

private:
    const TReplicatedTableTrackerConfigPtr Config_;
    const TAsyncExpiringCacheConfigPtr BundleHealthCacheConfig_ = New<TAsyncExpiringCacheConfig>();
    const TAsyncExpiringCacheConfigPtr ClusterStateCacheConfig_ = New<TAsyncExpiringCacheConfig>();

    std::atomic<bool> Enabled_ = false;

    TAtomicObject<TBundleHealthCachePtr> BundleHealthCache_;
    TAtomicObject<TClusterStateCachePtr> ClusterStateCache_;

    class TReplica
        : public TRefCounted
    {
    public:
        TReplica(
            TObjectId id,
            ETableReplicaMode mode,
            const TString& clusterName,
            const TYPath& path,
            TBundleHealthCachePtr bundleHealthCache,
            TClusterStateCachePtr clusterStateCache,
            IConnectionPtr connection,
            IInvokerPtr checkerInvoker,
            TDuration lag,
            TDuration tabletCellBundleNameTtl,
            TDuration retryOnFailureInterval)
            : Id_(id)
            , Mode_(mode)
            , ClusterName_(clusterName)
            , Path_(path)
            , BundleHealthCache_(std::move(bundleHealthCache))
            , ClusterStateCache_(std::move(clusterStateCache))
            , Connection_(std::move(connection))
            , CheckerInvoker_(std::move(checkerInvoker))
            , Lag_(lag)
            , TabletCellBundleNameTtl_(tabletCellBundleNameTtl)
            , RetryOnFailureInterval_(retryOnFailureInterval)
        {
            CreateClient();
        }

        TObjectId GetId()
        {
            return Id_;
        }

        const TString& GetClusterName()
        {
            return ClusterName_;
        }

        const TYPath& GetPath()
        {
            return Path_;
        }

        TDuration GetLag() const
        {
            return Lag_;
        }

        bool IsSync() const
        {
            return Mode_ == ETableReplicaMode::Sync;
        }

        TFuture<void> Check()
        {
            if (!Client_) {
                static const auto NoClientResult = MakeFuture<void>(TError("No connection is available"));
                return NoClientResult;
            }

            return AllSucceeded(std::vector<TFuture<void>>{
                CheckClusterState(),
                CheckTableExists(),
                CheckBundleHealth()
            });
        }

        TFuture<void> SetMode(TBootstrap* const bootstrap, ETableReplicaMode mode)
        {
            YT_LOG_DEBUG("Switching table replica mode (Path: %v, ReplicaId: %v, Mode: %v)",
                Path_,
                Id_,
                mode);

            auto automatonInvoker = bootstrap
                ->GetHydraFacade()
                ->GetAutomatonInvoker(EAutomatonThreadQueue::TabletManager);

            return BIND([=, this_ = MakeStrong(this)] () {
                auto req = TTableReplicaYPathProxy::Alter(FromObjectId(Id_));
                GenerateMutationId(req);
                req->set_mode(static_cast<int>(mode));

                const auto& objectManager = bootstrap->GetObjectManager();
                auto rootService = objectManager->GetRootService();
                return ExecuteVerb(rootService, req);
            })
                .AsyncVia(automatonInvoker)
                .Run()
                .Apply(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TTableReplicaYPathProxy::TRspAlterPtr>& rspOrError) {
                    if (rspOrError.IsOK()) {
                        Mode_ = mode;
                        YT_LOG_DEBUG("Table replica mode switched (Path: %v, ReplicaId: %v, Mode: %v)",
                            Path_,
                            Id_,
                            mode);
                    } else {
                        YT_LOG_DEBUG(rspOrError, "Error switching table replica mode (Path: %v, ReplicaId: %v, Mode: %v)",
                            Path_,
                            Id_,
                            mode);
                    }
                })
                .Via(CheckerInvoker_));
        }

        bool operator == (const TReplica& other) const
        {
            return Id_ == other.Id_
                && ClusterName_ == other.ClusterName_
                && Path_ == other.Path_;
        }

        bool operator != (const TReplica& other) const
        {
            return !(*this == other);
        }

        void Merge(const TReplica& other)
        {
            Mode_ = other.Mode_;
            TabletCellBundleNameTtl_ = other.TabletCellBundleNameTtl_;
            RetryOnFailureInterval_ = other.RetryOnFailureInterval_;
            if (Connection_ != other.Connection_) {
                Connection_ = other.Connection_;
                CreateClient();
            }
            Lag_ = other.Lag_;
        }

    private:
        const TObjectId Id_;
        ETableReplicaMode Mode_;
        const TString ClusterName_;
        const TYPath Path_;

        TBundleHealthCachePtr BundleHealthCache_;
        TClusterStateCachePtr ClusterStateCache_;
        NApi::IConnectionPtr Connection_;
        NApi::IClientPtr Client_;
        const IInvokerPtr CheckerInvoker_;
        TDuration Lag_;
        TFuture<TString> AsyncTabletCellBundleName_ = MakeFuture<TString>(TError("<unknown>"));

        TDuration TabletCellBundleNameTtl_;
        TDuration RetryOnFailureInterval_;

        TInstant LastUpdateTime_;


        TFuture<void> CheckClusterState()
        {
            return ClusterStateCache_->Get({Client_, ClusterName_});
        }

        TFuture<void> CheckBundleHealth()
        {
            return GetAsyncTabletCellBundleName()
                .Apply(BIND([client = Client_, clusterName = ClusterName_, bundleHealthCache = BundleHealthCache_] (const TErrorOr<TString>& bundleNameOrError) {
                    THROW_ERROR_EXCEPTION_IF_FAILED(bundleNameOrError, "Error getting table bundle name");
                    const auto& bundleName = bundleNameOrError.Value();
                    return bundleHealthCache->Get({client, clusterName, bundleName});
                })).Apply(BIND([] (const TErrorOr<ETabletCellHealth>& healthOrError) {
                    THROW_ERROR_EXCEPTION_IF_FAILED(healthOrError, "Error getting tablet cell bundle health");
                    auto health = healthOrError.Value();
                    if (health != ETabletCellHealth::Good) {
                        THROW_ERROR_EXCEPTION("Bad tablet cell health %Qlv",
                            health);
                    }
                }));
        }

        TFuture<void> CheckTableExists()
        {
            return Client_->NodeExists(Path_)
                .Apply(BIND([] (const TErrorOr<bool>& result) {
                    THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error checking table existence");
                    auto exists = result.Value();
                    if (!exists) {
                        THROW_ERROR_EXCEPTION("Table does not exist");
                    }
                }));
        }

        TFuture<TString> GetAsyncTabletCellBundleName()
        {
            auto now = NProfiling::GetInstant();
            auto interval = (AsyncTabletCellBundleName_.IsSet() && !AsyncTabletCellBundleName_.Get().IsOK())
                ? RetryOnFailureInterval_
                : TabletCellBundleNameTtl_;

            if (LastUpdateTime_ + interval < now) {
                LastUpdateTime_ = now;
                AsyncTabletCellBundleName_ = Client_->GetNode(Path_ + "/@tablet_cell_bundle")
                    .Apply(BIND([] (const TErrorOr<TYsonString>& bundleNameOrError) {
                        THROW_ERROR_EXCEPTION_IF_FAILED(bundleNameOrError, "Error getting table bundle name");
                        return ConvertTo<TString>(bundleNameOrError.Value());
                    }));
            }

            return AsyncTabletCellBundleName_;
        }

        void CreateClient()
        {
            Client_ = Connection_
                ? Connection_->CreateClient(NApi::TClientOptions::FromUser(RootUserName))
                : IClientPtr();
        }
    };

    using TReplicaPtr = TIntrusivePtr<TReplica>;

    class TTable
        : public TRefCounted
    {
    public:
        TTable(TObjectId id, NProfiling::TTagId tabletCellBundleProfilingTag, TReplicatedTableOptionsPtr config = nullptr)
            : Id_(id)
            , TabletCellBundleProfilingTag_(tabletCellBundleProfilingTag)
            , Config_(std::move(config))
        { }

        TObjectId GetId() const
        {
            return Id_;
        }

        NProfiling::TTagId GetTabletCellBundleProfilingTag() const
        {
            return TabletCellBundleProfilingTag_;
        }

        bool IsEnabled() const
        {
            auto guard = Guard(Lock_);
            return Config_ && Config_->EnableReplicatedTableTracker;
        }

        void SetConfig(const NTableServer::TReplicatedTableOptionsPtr& config)
        {
            auto guard = Guard(Lock_);
            Config_ = config;
        }

        void SetReplicas(const std::vector<TReplicaPtr>& replicas)
        {
            auto guard = Guard(Lock_);
            Replicas_.resize(replicas.size());
            for (int i = 0; i < static_cast<int>(replicas.size()); ++i) {
                if (!Replicas_[i] || *Replicas_[i] != *replicas[i]) {
                    Replicas_[i] = replicas[i];
                } else {
                    Replicas_[i]->Merge(*replicas[i]);
                }
            }
        }

        TFuture<int> Check(TBootstrap* bootstrap)
        {
            if (!CheckFuture_ || CheckFuture_.IsSet()) {
                std::vector<TReplicaPtr> syncReplicas;
                std::vector<TReplicaPtr> asyncReplicas;
                int maxSyncReplicas;
                int minSyncReplicas;

                {
                    auto guard = Guard(Lock_);
                    std::tie(minSyncReplicas, maxSyncReplicas) = Config_->GetEffectiveMinMaxReplicaCount(static_cast<int>(Replicas_.size()));
                    asyncReplicas.reserve(Replicas_.size());
                    syncReplicas.reserve(Replicas_.size());
                    for (auto& replica : Replicas_) {
                        if (replica->IsSync()) {
                            syncReplicas.push_back(replica);
                        } else {
                            asyncReplicas.push_back(replica);
                        }
                    }
                }

                std::vector<TFuture<void>> futures;
                futures.reserve(syncReplicas.size() + asyncReplicas.size());

                for (const auto& syncReplica : syncReplicas) {
                    futures.push_back(syncReplica->Check());
                }
                for (const auto& asyncReplica : asyncReplicas) {
                    futures.push_back(asyncReplica->Check());
                }

                CheckFuture_ = AllSet(futures)
                    .Apply(BIND([
                        bootstrap,
                        syncReplicas,
                        asyncReplicas,
                        maxSyncReplicas,
                        minSyncReplicas,
                        id = Id_
                    ] (const std::vector<TErrorOr<void>>& results) mutable {
                        std::vector<TReplicaPtr> badSyncReplicas;
                        std::vector<TReplicaPtr> goodSyncReplicas;
                        std::vector<TReplicaPtr> goodAsyncReplicas;
                        goodSyncReplicas.reserve(syncReplicas.size());
                        badSyncReplicas.reserve(syncReplicas.size());
                        goodAsyncReplicas.reserve(asyncReplicas.size());

                        auto logLivenessCheckResult = [&] (const TError& error, const TReplicaPtr& replica) {
                            YT_LOG_DEBUG_IF(!error.IsOK(), error, "Replica liveness check failed (ReplicatedTableId: %v, ReplicaId: %v, "
                                "ReplicaTablePath: %v, ReplicaClusterName: %v)",
                                id,
                                replica->GetId(),
                                replica->GetPath(),
                                replica->GetClusterName());
                        };

                        {
                            int index = 0;
                            for (; index < static_cast<int>(syncReplicas.size()); ++index) {
                                const auto& result = results[index];
                                const auto& replica = syncReplicas[index];
                                logLivenessCheckResult(result, replica);
                                if (result.IsOK()) {
                                    goodSyncReplicas.push_back(replica);
                                } else {
                                    badSyncReplicas.push_back(replica);
                                }
                            }
                            for (; index < static_cast<int>(results.size()); ++index) {
                                const auto& result = results[index];
                                const auto& replica = asyncReplicas[index - syncReplicas.size()];
                                logLivenessCheckResult(result, replica);
                                if (result.IsOK()) {
                                    goodAsyncReplicas.push_back(replica);
                                }
                            }
                        }

                        std::sort(
                            goodAsyncReplicas.begin(),
                            goodAsyncReplicas.end(),
                            [] (const auto& lhs, const auto& rhs) {
                                return lhs->GetLag() > rhs->GetLag();
                            });

                        std::vector<TFuture<void>> futures;
                        futures.reserve(syncReplicas.size() + asyncReplicas.size());

                        int switchCount;
                        int totalSyncReplicas;
                        {
                            int index;
                            for (index = static_cast<int>(goodSyncReplicas.size()); index < maxSyncReplicas && !goodAsyncReplicas.empty(); ++index) {
                                futures.push_back(goodAsyncReplicas.back()->SetMode(bootstrap, ETableReplicaMode::Sync));
                                switchCount++;
                                goodAsyncReplicas.pop_back();
                            }

                            totalSyncReplicas = maxSyncReplicas < static_cast<int>(goodSyncReplicas.size())
                                ? maxSyncReplicas
                                : index;
                        }

                        YT_VERIFY(totalSyncReplicas <= maxSyncReplicas);

                        for (int index = Max(0, minSyncReplicas - totalSyncReplicas); index < static_cast<int>(badSyncReplicas.size()); ++index) {
                            futures.push_back(badSyncReplicas[index]->SetMode(bootstrap, ETableReplicaMode::Async));
                            switchCount++;
                        }
                        for (int index = maxSyncReplicas; index < static_cast<int>(goodSyncReplicas.size()); ++index) {
                            futures.push_back(goodSyncReplicas[index]->SetMode(bootstrap, ETableReplicaMode::Async));
                            switchCount++;
                        }

                        return AllSucceeded(futures)
                            .Apply(BIND([switchCount] () {
                                return switchCount;
                            }));
                    }));
            }

            return CheckFuture_;
        }

    private:
        const TObjectId Id_;
        const NProfiling::TTagId TabletCellBundleProfilingTag_;
        NTableServer::TReplicatedTableOptionsPtr Config_;

        TAdaptiveLock Lock_;
        std::vector<TReplicaPtr> Replicas_;

        TFuture<int> CheckFuture_;
    };

    using TTablePtr = TIntrusivePtr<TTable>;

    TAdaptiveLock Lock_;
    THashMap<TObjectId, TTablePtr> Tables_;

    TPeriodicExecutorPtr UpdaterExecutor_;

    TThreadPoolPtr CheckerThreadPool_;
    TPeriodicExecutorPtr CheckerExecutor_;

    const TClusterDirectoryPtr ClusterDirectory_ = New<TClusterDirectory>();
    const NHiveServer::TClusterDirectorySynchronizerConfigPtr ClusterDirectorySynchronizerConfig_ = New<NHiveServer::TClusterDirectorySynchronizerConfig>();
    NHiveServer::TClusterDirectorySynchronizerPtr ClusterDirectorySynchronizer_;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);
    DECLARE_THREAD_AFFINITY_SLOT(CheckerThread);


    void CheckEnabled()
    {
        Enabled_ = false;

        const auto& hydraFacade = Bootstrap_->GetHydraFacade();
        if (!hydraFacade->GetHydraManager()->IsActiveLeader()) {
            return;
        }

        const auto& worldInitializer = Bootstrap_->GetWorldInitializer();
        if (!worldInitializer->IsInitialized()) {
            return;
        }

        const auto& dynamicConfig = GetDynamicConfig();
        if (!dynamicConfig->EnableReplicatedTableTracker) {
            YT_LOG_INFO("Replicated table tracker is disabled, see //sys/@config");
            return;
        }

        Enabled_ = true;
    }

    void UpdateTables()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        THashMap<TObjectId, TTablePtr> capturedTables;

        {
            auto guard = Guard(Lock_);
            capturedTables = Tables_;
        }

        for (const auto& [id, table] : capturedTables) {
            auto* object = Bootstrap_->GetObjectManager()->FindObject(id);
            if (!IsObjectAlive(object)) {
                YT_LOG_DEBUG("Table no longer exists (TableId: %v)",
                    id);
                {
                    auto guard = Guard(Lock_);
                    Tables_.erase(id);
                }
                continue;
            }

            OnNodeCreated(object);
        }
    }

    void CheckTables()
    {
        VERIFY_THREAD_AFFINITY(CheckerThread);

        std::vector<TFuture<int>> futures;
        std::vector<NProfiling::TTagId> profilingTags;

        {
            auto guard = Guard(Lock_);
            futures.reserve(Tables_.size());
            profilingTags.reserve(Tables_.size());

            for (const auto& [id, table] : Tables_) {
                if (!table->IsEnabled()) {
                    YT_LOG_DEBUG("Replicated Table Tracker is disabled (TableId: %v)",
                        id);
                    continue;
                }

                auto future = table->Check(Bootstrap_);
                future.Subscribe(BIND([id = id] (const TErrorOr<int>& result) {
                    YT_LOG_DEBUG_UNLESS(result.IsOK(), result, "Error checking table (TableId: %v)",
                        id);
                }));
                futures.push_back(future);
                profilingTags.push_back(table->GetTabletCellBundleProfilingTag());
            }
        }

        auto switchCountTables = WaitFor(AllSet(futures))
            .ValueOrThrow();

        THashMap<NProfiling::TTagId, int> switchCount;
        for (int index = 0; index < switchCountTables.size(); index++) {
            int switchCountTable = switchCountTables[index].Value();
            switchCount[profilingTags[index]] += switchCountTable;
        }

        for (auto [profilingTag, switchMoves] : switchCount) {
            Profiler.Enqueue(
                "/switch_tablet_replica_mode",
                switchMoves,
                NProfiling::EMetricType::Counter,
                {profilingTag}
            );
        }
    }

    void UpdateIteration()
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        CheckEnabled();
        if (!Enabled_) {
            return;
        }

        try {
            UpdateTables();
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Cannot update tables");
        }
    }

    void CheckIteration()
    {
        VERIFY_THREAD_AFFINITY(CheckerThread);

        if (!Enabled_) {
            return;
        }

        try {
            CheckTables();
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Cannot check tables");
        }
    }


    virtual void OnLeaderActive() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        UpdaterExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetHydraFacade()->GetAutomatonInvoker(EAutomatonThreadQueue::ReplicatedTableTracker),
            BIND(&TImpl::UpdateIteration, MakeWeak(this)));
        UpdaterExecutor_->Start();

        CheckerExecutor_ = New<TPeriodicExecutor>(
            CheckerThreadPool_->GetInvoker(),
            BIND(&TImpl::CheckIteration, MakeWeak(this)));
        CheckerExecutor_->Start();

        OnDynamicConfigChanged();
    }

    virtual void OnStopLeading() override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (CheckerExecutor_) {
            CheckerExecutor_->Stop();
            CheckerExecutor_.Reset();
        }

        if (UpdaterExecutor_) {
            UpdaterExecutor_->Stop();
            UpdaterExecutor_.Reset();
        }

        if (ClusterDirectorySynchronizer_) {
            ClusterDirectorySynchronizer_->Stop();
            ClusterDirectorySynchronizer_.Reset();
        }

        Enabled_ = false;
    }

    virtual void OnAfterSnapshotLoaded() noexcept override
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        TCompositeAutomatonPart::OnAfterSnapshotLoaded();

        const auto& cypressManager = Bootstrap_->GetCypressManager();
        for (auto [id, node] : cypressManager->Nodes()) {
            if (node->IsTrunk() && node->GetType() == EObjectType::ReplicatedTable) {
                auto* object = node->As<NTableServer::TReplicatedTableNode>();
                ProcessReplicatedTable(object);
            }
        }
    }


    void ProcessReplicatedTable(NTableServer::TReplicatedTableNode* object)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (object->IsExternal()) {
            return;
        }

        auto id = object->GetId();
        const auto& config = object->GetReplicatedTableOptions();
        auto tabletCellBundle = object->GetTabletCellBundle();

        TTablePtr table;

        bool newTable = false;

        {
            auto guard = Guard(Lock_);
            auto it = Tables_.find(id);
            if (it == Tables_.end()) {
                table = New<TTable>(id, tabletCellBundle->GetProfilingTag(), config);
                Tables_.emplace(id, table);
                newTable = true;
            } else {
                table = it->second;
            }
        }

        std::vector<TReplicaPtr> replicas;
        replicas.reserve(object->Replicas().size());

        auto lastestTimestamp = Bootstrap_->GetTimestampProvider()->GetLatestTimestamp();

        int skippedReplicas = 0;
        int syncReplicas = 0;
        int asyncReplicas = 0;

        for (const auto& replica : object->Replicas()) {
            if (!replica->GetEnableReplicatedTableTracker()) {
                skippedReplicas += 1;
                continue;
            }

            switch (replica->GetMode()) {
                case ETableReplicaMode::Sync:
                    syncReplicas += 1;
                    break;
                case ETableReplicaMode::Async:
                    asyncReplicas += 1;
                    break;
                default:
                    YT_ABORT();
            }

            auto connection = ClusterDirectory_->FindConnection(replica->GetClusterName());
            if (!connection) {
                YT_LOG_WARNING("Unknown replica cluster (Name: %v, ReplicaId: %v, TableId: %v)",
                    replica->GetClusterName(),
                    replica->GetId(),
                    table->GetId());
            }

            replicas.push_back(New<TReplica>(
                replica->GetId(),
                replica->GetMode(),
                replica->GetClusterName(),
                replica->GetReplicaPath(),
                BundleHealthCache_.Load(),
                ClusterStateCache_.Load(),
                connection,
                CheckerThreadPool_->GetInvoker(),
                replica->ComputeReplicationLagTime(lastestTimestamp),
                config->TabletCellBundleNameTtl,
                config->RetryOnFailureInterval));
        }

        const auto [maxSyncReplicas,  minSyncReplicas] = config->GetEffectiveMinMaxReplicaCount(static_cast<int>(replicas.size()));

        YT_LOG_DEBUG("Table %v (TableId: %v, Replicas: %v, SyncReplicas: %v, AsyncReplicas: %v, SkippedReplicas: %v, DesiredMaxSyncReplicas: %v, DesiredMinSyncReplicas: %v)",
            newTable ? "added" : "updated",
            object->GetId(),
            object->Replicas().size(),
            syncReplicas,
            asyncReplicas,
            skippedReplicas,
            maxSyncReplicas,
            minSyncReplicas);

        table->SetConfig(config);
        table->SetReplicas(replicas);
    }

    void OnNodeCreated(TObject* object)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        if (object->IsTrunk() && object->GetType() == EObjectType::ReplicatedTable) {
            auto* replicatedTable = object->As<NTableServer::TReplicatedTableNode>();
            ProcessReplicatedTable(replicatedTable);
        }
    }


    const TDynamicReplicatedTableTrackerConfigPtr& GetDynamicConfig()
    {
        return Bootstrap_->GetConfigManager()->GetConfig()->TabletManager->ReplicatedTableTracker;
    }

    void OnDynamicConfigChanged(TDynamicClusterConfigPtr /*oldConfig*/ = nullptr)
    {
        VERIFY_THREAD_AFFINITY(AutomatonThread);

        const auto& dynamicConfig = GetDynamicConfig();

        if (UpdaterExecutor_) {
            UpdaterExecutor_->SetPeriod(dynamicConfig->UpdatePeriod);
        }

        if (CheckerExecutor_) {
            CheckerExecutor_->SetPeriod(dynamicConfig->CheckPeriod);
        }

        if (ReconfigureYsonSerializable(BundleHealthCacheConfig_, dynamicConfig->BundleHealthCache)) {
            BundleHealthCache_.Store(New<TBundleHealthCache>(BundleHealthCacheConfig_));
        }

        if (ReconfigureYsonSerializable(ClusterStateCacheConfig_, dynamicConfig->ClusterStateCache)) {
            ClusterStateCache_.Store(New<TClusterStateCache>(ClusterStateCacheConfig_));
        }

        if (IsLeader() && (ReconfigureYsonSerializable(ClusterDirectorySynchronizerConfig_, dynamicConfig->ClusterDirectorySynchronizer) || !ClusterDirectorySynchronizer_)) {
            if (ClusterDirectorySynchronizer_) {
                ClusterDirectorySynchronizer_->Stop();
            }
            ClusterDirectorySynchronizer_ = New<NHiveServer::TClusterDirectorySynchronizer>(
                dynamicConfig->ClusterDirectorySynchronizer,
                Bootstrap_,
                ClusterDirectory_);
            ClusterDirectorySynchronizer_->Start();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TReplicatedTableTracker::TReplicatedTableTracker(
    TReplicatedTableTrackerConfigPtr config,
    TBootstrap* bootstrap)
    : Impl_(New<TImpl>(std::move(config), bootstrap))
{ }

TReplicatedTableTracker::~TReplicatedTableTracker() = default;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletServer
