#include "slot.h"
#include "chunk_cache.h"
#include "private.h"
#include "job_environment.h"
#include "slot_location.h"
#include "volume_manager.h"

#include <yt/yt/server/lib/exec_node/config.h>

#include <yt/yt/ytlib/tools/tools.h>

#include <yt/yt/core/bus/tcp/client.h>

#include <yt/yt/core/logging/log_manager.h>

#include <yt/yt/core/misc/proc.h>

#include <util/folder/dirut.h>

#include <util/system/fs.h>

namespace NYT::NExecNode {

static NLogging::TLogger Logger("SlotLogger");

using namespace NBus;
using namespace NConcurrency;
using namespace NContainers;
using namespace NDataNode;
using namespace NTools;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TSlot
    : public ISlot
{
public:
    TSlot(
        int slotIndex,
        TSlotLocationPtr location,
        IJobEnvironmentPtr environment,
        IVolumeManagerPtr volumeManager,
        const TString& nodeTag)
        : SlotIndex_(slotIndex)
        , JobEnvironment_(std::move(environment))
        , Location_(std::move(location))
        , VolumeManager_(std::move(volumeManager))
        , NodeTag_(nodeTag)
        , JobProxyUnixDomainSocketPath_(GetJobProxyUnixDomainSocketPath())
    {
        Location_->IncreaseSessionCount();
    }

    virtual void CleanProcesses() override
    {
        // First kill all processes that may hold open handles to slot directories.
        JobEnvironment_->CleanProcesses(SlotIndex_);
    }

    virtual void CleanSandbox()
    {
        WaitFor(Location_->CleanSandboxes(
            SlotIndex_))
            .ThrowOnError();
        Location_->DecreaseSessionCount();
    }

    virtual void CancelPreparation() override
    {
        PreparationCanceled_ = true;

        for (const auto& future : PreparationFutures_) {
            future.Cancel(TError("Job preparation canceled"));
        }
    }

    virtual TFuture<void> RunJobProxy(
        NJobProxy::TJobProxyConfigPtr config,
        TJobId jobId,
        TOperationId operationId) override
    {
        return RunPrepareAction<void>([&] {
                {
                    auto error = WaitFor(Location_->MakeConfig(SlotIndex_, ConvertToNode(config)));
                    THROW_ERROR_EXCEPTION_IF_FAILED(error, "Failed to create job proxy config");
                }

                return JobEnvironment_->RunJobProxy(
                    SlotIndex_,
                    Location_->GetSlotPath(SlotIndex_),
                    jobId,
                    operationId,
                    config->StderrPath);
            },
            // Job proxy preparation is uncancelable, otherwise we might try to kill
            // a never-started job proxy process.
            true);
    }

    virtual TFuture<void> MakeLink(
        TJobId jobId,
        const TString& artifactName,
        ESandboxKind sandboxKind,
        const TString& targetPath,
        const TString& linkName,
        bool executable) override
    {
        return RunPrepareAction<void>([&] {
                return Location_->MakeSandboxLink(
                    jobId,
                    SlotIndex_,
                    artifactName,
                    sandboxKind,
                    targetPath,
                    linkName,
                    executable);
            });
    }

    virtual TFuture<void> MakeCopy(
        TJobId jobId,
        const TString& artifactName,
        ESandboxKind sandboxKind,
        const TString& sourcePath,
        const TFile& destinationFile) override
    {
        return RunPrepareAction<void>([&] {
                return Location_->MakeSandboxCopy(
                    jobId,
                    SlotIndex_,
                    artifactName,
                    sandboxKind,
                    sourcePath,
                    destinationFile);
            });
    }

    virtual TFuture<void> MakeFile(
        TJobId jobId,
        const TString& artifactName,
        ESandboxKind sandboxKind,
        const std::function<void(IOutputStream*)>& producer,
        const TFile& destinationFile) override
    {
        return RunPrepareAction<void>([&] {
                return Location_->MakeSandboxFile(
                    jobId,
                    SlotIndex_,
                    artifactName,
                    sandboxKind,
                    producer,
                    destinationFile);
            });
    }

    virtual bool IsLayerCached(const TArtifactKey& artifactKey) const override
    {
        return VolumeManager_->IsLayerCached(artifactKey);
    }

    virtual TFuture<IVolumePtr> PrepareRootVolume(
        const std::vector<TArtifactKey>& layers,
        const TArtifactDownloadOptions& downloadOptions) override
    {
        if (!VolumeManager_) {
            return MakeFuture<IVolumePtr>(TError("Porto layers and custom root FS are not supported"));
        }
        return RunPrepareAction<IVolumePtr>([&] {
                return VolumeManager_->PrepareVolume(layers, downloadOptions);
            });
    }

    virtual int GetSlotIndex() const override
    {
        return SlotIndex_;
    }

    virtual TString GetSandboxPath(ESandboxKind sandbox) const override
    {
        return Location_->GetSandboxPath(SlotIndex_, sandbox);
    }

    virtual TString GetMediumName() const override
    {
        return Location_->GetMediumName();
    }

    virtual TTcpBusServerConfigPtr GetBusServerConfig() const override
    {
        return TTcpBusServerConfig::CreateUnixDomain(JobProxyUnixDomainSocketPath_);
    }

    virtual TTcpBusClientConfigPtr GetBusClientConfig() const override
    {
        return TTcpBusClientConfig::CreateUnixDomain(JobProxyUnixDomainSocketPath_);
    }

    virtual TFuture<std::vector<TString>> PrepareSandboxDirectories(const TUserSandboxOptions& options)
    {
        return RunPrepareAction<std::vector<TString>>([&] {
                return Location_->PrepareSandboxDirectories(
                    SlotIndex_,
                    options);
            },
            // Includes quota setting and tmpfs creation.
            true);
    }

    virtual TFuture<void> RunSetupCommands(
        TJobId jobId,
        const std::vector<NJobAgent::TShellCommandConfigPtr>& commands,
        const NContainers::TRootFS& rootFS,
        const TString& user,
        const std::optional<std::vector<TDevice>>& devices,
        int startIndex)
    {
        return RunPrepareAction<void>([&] {
                return JobEnvironment_->RunSetupCommands(SlotIndex_, jobId, commands, rootFS, user, devices, startIndex);
            },
            // Setup commands are uncancelable since they are run in separate processes.
            true);
    }

    virtual void OnArtifactPreparationFailed(
        TJobId jobId,
        const TString& artifactName,
        ESandboxKind sandboxKind,
        const TString& artifactPath,
        const TError& error)
    {
        Location_->OnArtifactPreparationFailed(
            jobId,
            SlotIndex_,
            artifactName,
            sandboxKind,
            artifactPath,
            error);
    }

private:
    const int SlotIndex_;
    const IJobEnvironmentPtr JobEnvironment_;
    const TSlotLocationPtr Location_;
    const IVolumeManagerPtr VolumeManager_;

    //! Uniquely identifies a node process on the current host.
    //! Used for unix socket name generation, to communicate between node and job proxies.
    const TString NodeTag_;

    std::vector<TFuture<void>> PreparationFutures_;
    bool PreparationCanceled_ = false;

    const TString JobProxyUnixDomainSocketPath_;

    template <class T>
    TFuture<T> RunPrepareAction(std::function<TFuture<T>()> action, bool uncancelable = false)
    {
        if (PreparationCanceled_) {
            return MakeFuture<T>(TError("Job preparation canceled")
                << TErrorAttribute("slot_index", SlotIndex_));
        } else {
            auto future = action();
            auto preparationFuture = future.template As<void>();
            PreparationFutures_.push_back(uncancelable
                ? preparationFuture.ToUncancelable()
                : preparationFuture);
            return future;
        }
    }

    TString GetJobProxyUnixDomainSocketPath() const
    {
        return NFS::CombinePaths({
            Location_->GetSlotPath(SlotIndex_),
            "pipes",
            Format("%v-job-proxy-%v", NodeTag_, SlotIndex_)});
    }
};

////////////////////////////////////////////////////////////////////////////////

ISlotPtr CreateSlot(
    int slotIndex,
    TSlotLocationPtr location,
    IJobEnvironmentPtr environment,
    IVolumeManagerPtr volumeManager,
    const TString& nodeTag)
{
    auto slot = New<TSlot>(
        slotIndex,
        std::move(location),
        std::move(environment),
        std::move(volumeManager),
        nodeTag);

    return slot;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecNode
