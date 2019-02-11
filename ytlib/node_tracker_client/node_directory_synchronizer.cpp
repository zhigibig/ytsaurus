#include "node_directory_synchronizer.h"
#include "config.h"
#include "private.h"

#include <yt/client/api/connection.h>
#include <yt/client/api/client.h>

#include <yt/client/node_tracker_client/node_directory.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/core/rpc/dispatcher.h>

#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/scheduler.h>

namespace NYT::NNodeTrackerClient {

using namespace NConcurrency;
using namespace NApi;
using namespace NYTree;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = NodeTrackerClientLogger;

////////////////////////////////////////////////////////////////////////////////

class TNodeDirectorySynchronizer::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TNodeDirectorySynchronizerConfigPtr config,
        IConnectionPtr directoryConnection,
        TNodeDirectoryPtr nodeDirectory)
        : Config_(config)
        , DirectoryClient_(directoryConnection->CreateClient(TClientOptions(NSecurityClient::RootUserName)))
        , NodeDirectory_(nodeDirectory)
        , SyncExecutor_(New<TPeriodicExecutor>(
            NRpc::TDispatcher::Get()->GetLightInvoker(),
            BIND(&TImpl::OnSync, MakeWeak(this)),
            Config_->SyncPeriod))
    { }

    void Start()
    {
        SyncExecutor_->Start();
    }

    TFuture<void> Stop()
    {
        TerminationPromise_.Set(TError("Node directory synchronizer terminated"));
        return SyncExecutor_->Stop();
    }

private:
    const TNodeDirectorySynchronizerConfigPtr Config_;
    const IClientPtr DirectoryClient_;
    const TNodeDirectoryPtr NodeDirectory_;

    const TPeriodicExecutorPtr SyncExecutor_;
    TPromise<TClusterMeta> TerminationPromise_ = NewPromise<TClusterMeta>();

    void DoSync()
    {
        try {
            YT_LOG_DEBUG("Started updating node directory");

            TGetClusterMetaOptions options;
            options.ReadFrom = EMasterChannelKind::Cache;
            options.PopulateNodeDirectory = true;
            auto asyncMeta = DirectoryClient_->GetClusterMeta(options);

            // NB(psushin): the trick with TerminationPromise_ allows us to immediately terminate synchronizer,
            // e.g. when we get stuck in very long sequence of retries.
            auto promise = NewPromise<TClusterMeta>();
            promise.TrySetFrom(TerminationPromise_.ToFuture());
            promise.TrySetFrom(asyncMeta);

            auto meta = WaitFor(promise.ToFuture())
                .ValueOrThrow();

            NodeDirectory_->MergeFrom(*meta.NodeDirectory);

            YT_LOG_DEBUG("Finished updating node directory");
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error updating node directory")
                << ex;
        }
    }

    void OnSync()
    {
        try {
            DoSync();
        } catch (const std::exception& ex) {
            YT_LOG_DEBUG(TError(ex));
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TNodeDirectorySynchronizer::TNodeDirectorySynchronizer(
    TNodeDirectorySynchronizerConfigPtr config,
    IConnectionPtr directoryConnection,
    TNodeDirectoryPtr nodeDirectory)
    : Impl_(New<TImpl>(
        config,
        directoryConnection,
        nodeDirectory))
{ }

TNodeDirectorySynchronizer::~TNodeDirectorySynchronizer() = default;

void TNodeDirectorySynchronizer::Start()
{
    Impl_->Start();
}

TFuture<void> TNodeDirectorySynchronizer::Stop()
{
    return Impl_->Stop();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNodeTrackerClient
