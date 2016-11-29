#include "node_directory_synchronizer.h"
#include "node_directory.h"
#include "config.h"
#include "private.h"

#include <yt/ytlib/api/connection.h>
#include <yt/ytlib/api/client.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/core/rpc/dispatcher.h>

#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/scheduler.h>

namespace NYT {
namespace NNodeTrackerClient {

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

    void Stop()
    {
        SyncExecutor_->Stop();
    }

private:
    const TNodeDirectorySynchronizerConfigPtr Config_;
    const IClientPtr DirectoryClient_;
    const TNodeDirectoryPtr NodeDirectory_;

    const TPeriodicExecutorPtr SyncExecutor_;


    void DoSync()
    {
        try {
            LOG_DEBUG("Started updating node directory");

            TGetClusterMetaOptions options;
            options.ReadFrom = EMasterChannelKind::Cache;
            options.PopulateNodeDirectory = true;
            auto meta = WaitFor(DirectoryClient_->GetClusterMeta(options))
                .ValueOrThrow();

            NodeDirectory_->MergeFrom(*meta.NodeDirectory);

            LOG_DEBUG("Finished updating node directory");
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
            LOG_ERROR(ex, "Node directory synchronization failed");
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

void TNodeDirectorySynchronizer::Stop()
{
    Impl_->Stop();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerClient
} // namespace NYT
