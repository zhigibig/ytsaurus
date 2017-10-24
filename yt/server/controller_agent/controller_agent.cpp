#include "controller_agent.h"
#include "operation_controller.h"

#include <yt/server/cell_scheduler/bootstrap.h>

#include <yt/server/scheduler/config.h>

#include <yt/ytlib/api/transaction.h>
#include <yt/ytlib/api/native_connection.h>

#include <yt/ytlib/chunk_client/throttler_manager.h>

#include <yt/core/concurrency/async_semaphore.h>
#include <yt/core/concurrency/thread_affinity.h>
#include <yt/core/concurrency/thread_pool.h>

#include <yt/core/ytree/convert.h>

namespace NYT {
namespace NControllerAgent {

using namespace NScheduler;
using namespace NConcurrency;
using namespace NYTree;
using namespace NChunkClient;

static const auto& Logger = ControllerAgentLogger;

////////////////////////////////////////////////////////////////////

class TControllerAgent::TImpl
    : public TRefCounted
{
public:
    TImpl(TSchedulerConfigPtr config, NCellScheduler::TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , ControllerThreadPool_(New<TThreadPool>(Config_->ControllerThreadCount, "Controller"))
        , ChunkLocationThrottlerManager_(New<TThrottlerManager>(
            Config_->ChunkLocationThrottler,
            ControllerAgentLogger))
        , CoreSemaphore_(New<TAsyncSemaphore>(Config_->MaxConcurrentSafeCoreDumps))
    { }

    void Disconnect()
    {
        Connected_.store(false);

        ControllerAgentMasterConnector_.Reset();
    }

    void Connect(IInvokerPtr invoker)
    {
        // TODO(ignat): fix it!
        Invoker_ = invoker;

        ConnectionTime_ = TInstant::Now();

        ControllerAgentMasterConnector_ = New<TMasterConnector>(
            invoker,
            Config_,
            Bootstrap_);

        Connected_.store(true);
    }

    void ValidateConnected()
    {
        if (!Connected_) {
            THROW_ERROR_EXCEPTION(GetMasterDisconnectedError());
        }
    }

    TInstant GetConnectionTime() const
    {
        return ConnectionTime_;
    }

    const IInvokerPtr& GetInvoker()
    {
        return Bootstrap_->GetControllerAgentInvoker();
    }

    const IInvokerPtr& GetControllerThreadPoolInvoker()
    {
        return ControllerThreadPool_->GetInvoker();
    }

    TMasterConnector* GetMasterConnector()
    {
        return ControllerAgentMasterConnector_.Get();
    }

    const TSchedulerConfigPtr& GetConfig() const
    {
        return Config_;
    }

    const NApi::INativeClientPtr& GetMasterClient() const
    {
        return Bootstrap_->GetMasterClient();
    }

    const TThrottlerManagerPtr& GetChunkLocationThrottlerManager() const
    {
        return ChunkLocationThrottlerManager_;
    }

    const TCoreDumperPtr& GetCoreDumper() const
    {
        return Bootstrap_->GetCoreDumper();
    }

    const TAsyncSemaphorePtr& GetCoreSemaphore() const
    {
        return CoreSemaphore_;
    }

    void UpdateConfig(const TSchedulerConfigPtr& config)
    {
        Config_ = config;
        ChunkLocationThrottlerManager_->Reconfigure(Config_->ChunkLocationThrottler);
        if (ControllerAgentMasterConnector_) {
            ControllerAgentMasterConnector_->UpdateConfig(config);
        }
    }

    void RegisterOperation(const TOperationId& operationId, IOperationControllerPtr controller)
    {
        TWriterGuard guard(ControllersLock_);
        Controllers_.emplace(operationId, controller);
    }

    void UnregisterOperation(const TOperationId& operationId)
    {
        TWriterGuard guard(ControllersLock_);
        YCHECK(Controllers_.erase(operationId) == 1);
    }

    std::vector<TErrorOr<TSharedRef>> GetJobSpecs(const std::vector<std::pair<TOperationId, TJobId>>& jobSpecRequests)
    {
        VERIFY_INVOKER_AFFINITY(Bootstrap_->GetControllerAgentInvoker());

        std::vector<TFuture<TSharedRef>> asyncJobSpecs;

        for (const auto& pair : jobSpecRequests) {
            const auto& operationId = pair.first;
            const auto& jobId = pair.second;

            LOG_DEBUG("Retrieving job spec (OperationId: %v, JobId: %v)",
                operationId,
                jobId);

            auto controller = FindController(operationId);

            if (!controller) {
                asyncJobSpecs.push_back(MakeFuture<TSharedRef>(TError("No such operation %v", operationId)));
                continue;
            }

            auto asyncJobSpec = BIND(&IOperationController::ExtractJobSpec,
                controller,
                jobId)
                .AsyncVia(controller->GetCancelableInvoker())
                .Run();

            asyncJobSpecs.push_back(asyncJobSpec);
        }

        auto results = WaitFor(CombineAll(asyncJobSpecs))
            .ValueOrThrow();

        int index = 0;
        for (const auto& result : results) {
            if (!result.IsOK()) {
                const auto& jobId = jobSpecRequests[index].second;
                LOG_DEBUG(result, "Failed to extract job spec (JobId: %v)", jobId);
            }
            ++index;
        }

        return results;
    }

    void AttachJobContext(
        const TYPath& path,
        const TChunkId& chunkId,
        const TOperationId& operationId,
        const TJobId& jobId)
    {
        ControllerAgentMasterConnector_->AttachJobContext(path, chunkId, operationId, jobId);
    }

private:
    TSchedulerConfigPtr Config_;
    NCellScheduler::TBootstrap* Bootstrap_;
    IInvokerPtr Invoker_;

    const TThreadPoolPtr ControllerThreadPool_;

    const TThrottlerManagerPtr ChunkLocationThrottlerManager_;

    const TAsyncSemaphorePtr CoreSemaphore_;

    std::atomic<bool> Connected_ = {false};
    TInstant ConnectionTime_;
    TMasterConnectorPtr ControllerAgentMasterConnector_;

    TReaderWriterSpinLock ControllersLock_;
    yhash<TOperationId, IOperationControllerPtr> Controllers_;

    // TODO: Move this method to some common place to avoid copy/paste.
    TError GetMasterDisconnectedError()
    {
        return TError(
            NRpc::EErrorCode::Unavailable,
            "Master is not connected");
    }

    IOperationControllerPtr FindController(const TOperationId& operationId)
    {
        TReaderGuard guard(ControllersLock_);
        auto it = Controllers_.find(operationId);
        if (it == Controllers_.end()) {
            return nullptr;
        }
        return it->second;
    }
};

////////////////////////////////////////////////////////////////////

TControllerAgent::TControllerAgent(
    NScheduler::TSchedulerConfigPtr config,
    NCellScheduler::TBootstrap* bootstrap)
    : Impl_(New<TImpl>(config, bootstrap))
{ }

void TControllerAgent::Connect(IInvokerPtr invoker)
{
    Impl_->Connect(invoker);
}

void TControllerAgent::Disconnect()
{
    Impl_->Disconnect();
}

void TControllerAgent::ValidateConnected() const
{
    Impl_->ValidateConnected();
}

TInstant TControllerAgent::GetConnectionTime() const
{
    return Impl_->GetConnectionTime();
}

const IInvokerPtr& TControllerAgent::GetInvoker()
{
    return Impl_->GetInvoker();
}

const IInvokerPtr& TControllerAgent::GetControllerThreadPoolInvoker()
{
    return Impl_->GetControllerThreadPoolInvoker();
}

TMasterConnector* TControllerAgent::GetMasterConnector()
{
    return Impl_->GetMasterConnector();
}

const TSchedulerConfigPtr& TControllerAgent::GetConfig() const
{
    return Impl_->GetConfig();
}

const NApi::INativeClientPtr& TControllerAgent::GetMasterClient() const
{
    return Impl_->GetMasterClient();
}

const TThrottlerManagerPtr& TControllerAgent::GetChunkLocationThrottlerManager() const
{
    return Impl_->GetChunkLocationThrottlerManager();
}

const TCoreDumperPtr& TControllerAgent::GetCoreDumper() const
{
    return Impl_->GetCoreDumper();
}

const TAsyncSemaphorePtr& TControllerAgent::GetCoreSemaphore() const
{
    return Impl_->GetCoreSemaphore();
}

void TControllerAgent::UpdateConfig(const TSchedulerConfigPtr& config)
{
    Impl_->UpdateConfig(config);
}

void TControllerAgent::RegisterOperation(const TOperationId& operationId, IOperationControllerPtr controller)
{
    Impl_->RegisterOperation(operationId, controller);
}

void TControllerAgent::UnregisterOperation(const TOperationId& operationId)
{
    Impl_->UnregisterOperation(operationId);
}

std::vector<TErrorOr<TSharedRef>> TControllerAgent::GetJobSpecs(const std::vector<std::pair<TOperationId, TJobId>>& jobSpecRequests)
{
    return Impl_->GetJobSpecs(jobSpecRequests);
}

void TControllerAgent::AttachJobContext(
    const TYPath& path,
    const TChunkId& chunkId,
    const TOperationId& operationId,
    const TJobId& jobId)
{
    Impl_->AttachJobContext(path, chunkId, operationId, jobId);
}

////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT
