#include "job_workspace_builder.h"

#include "job_gpu_checker.h"
#include "job_directory_manager.h"

#include <yt/yt/server/lib/exec_node/helpers.h>

#include <yt/yt/core/actions/cancelable_context.h>

#include <yt/yt/core/concurrency/thread_affinity.h>
#include <yt/yt/core/concurrency/delayed_executor.h>

#include <yt/yt/core/misc/fs.h>

namespace NYT::NExecNode
{

using namespace NConcurrency;
using namespace NContainers;
using namespace NJobAgent;
using namespace NFS;

////////////////////////////////////////////////////////////////////////////////

static const TString MountSuffix = "mount";

////////////////////////////////////////////////////////////////////////////////

TJobWorkspaceBuilder::TJobWorkspaceBuilder(
    IInvokerPtr invoker,
    TJobWorkspaceBuildingContext context,
    IJobDirectoryManagerPtr directoryManager)
    : Invoker_(std::move(invoker))
    , Context_(std::move(context))
    , DirectoryManager_(std::move(directoryManager))
    , Logger(Context_.Logger)
{
    YT_VERIFY(Context_.Slot);
    YT_VERIFY(Context_.Job);
    YT_VERIFY(DirectoryManager_);

    if (Context_.NeedGpuCheck) {
        YT_VERIFY(Context_.GpuCheckBinaryPath);
        YT_VERIFY(Context_.GpuCheckBinaryArgs);
    }
}

template<TFuture<void>(TJobWorkspaceBuilder::*Step)()>
TFuture<void> TJobWorkspaceBuilder::GuardedAction()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    auto jobPhase = Context_.Job->GetPhase();

    switch (jobPhase) {
        case EJobPhase::WaitingAbort:
        case EJobPhase::Cleanup:
        case EJobPhase::Finished:
            YT_LOG_DEBUG(
                "Skip workspace building action (JobPhase: %v, ActionName: %v)",
                jobPhase,
                GetStepName<Step>());
            return VoidFuture;

        case EJobPhase::Created:
            YT_VERIFY(Context_.Job->GetState() == EJobState::Waiting);
            break;

        default:
            YT_VERIFY(Context_.Job->GetState() == EJobState::Running);
            break;
    }

    TForbidContextSwitchGuard contextSwitchGuard;

    YT_LOG_DEBUG(
        "Run guarded workspace building action (JobPhase: %v, ActionName: %v)",
        jobPhase,
        GetStepName<Step>());

    return (*this.*Step)();
}

template<TFuture<void>(TJobWorkspaceBuilder::*Step)()>
constexpr const char* TJobWorkspaceBuilder::GetStepName()
{
    if (Step == &TJobWorkspaceBuilder::DoPrepareRootVolume) {
        return "DoPrepareRootVolume";
    } else if (Step == &TJobWorkspaceBuilder::DoRunSetupCommand) {
        return "DoRunSetupCommand";
    } else if (Step == &TJobWorkspaceBuilder::DoRunGpuCheckCommand) {
        return "DoRunGpuCheckCommand";
    }
}

template<TFuture<void>(TJobWorkspaceBuilder::*Method)()>
TCallback<TFuture<void>()> TJobWorkspaceBuilder::MakeStep()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return BIND([this, this_ = MakeStrong(this)] () {
        return GuardedAction<Method>();
    }).AsyncVia(Invoker_);
}

void TJobWorkspaceBuilder::ValidateJobPhase(EJobPhase expectedPhase) const
{
    VERIFY_THREAD_AFFINITY(JobThread);

    auto jobPhase = Context_.Job->GetPhase();
    if (jobPhase != expectedPhase) {
        YT_LOG_DEBUG(
            "Unexpected job phase during workspace preparation (Actual: %v, Expected: %v)",
            jobPhase,
            expectedPhase);

        THROW_ERROR_EXCEPTION("Unexpected job phase")
            << TErrorAttribute("expected_phase", expectedPhase)
            << TErrorAttribute("actual_phase", jobPhase);
    }
}

void TJobWorkspaceBuilder::SetJobPhase(EJobPhase phase)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    UpdateBuilderPhase_.Fire(phase);
}

void TJobWorkspaceBuilder::UpdateArtifactStatistics(i64 compressedDataSize, bool cacheHit)
{
    VERIFY_THREAD_AFFINITY(JobThread);

    UpdateArtifactStatistics_.Fire(compressedDataSize, cacheHit);
}

TFuture<TJobWorkspaceBuildingResult> TJobWorkspaceBuilder::Run()
{
    VERIFY_THREAD_AFFINITY(JobThread);

    return BIND(&TJobWorkspaceBuilder::DoPrepareSandboxDirectories, MakeStrong(this))
        .AsyncVia(Invoker_)
        .Run()
        .Apply(MakeStep<&TJobWorkspaceBuilder::DoPrepareRootVolume>())
        .Apply(MakeStep<&TJobWorkspaceBuilder::DoRunSetupCommand>())
        .Apply(MakeStep<&TJobWorkspaceBuilder::DoRunGpuCheckCommand>())
        .Apply(BIND([this, this_ = MakeStrong(this)] (const TError& result) {
            YT_LOG_DEBUG(result, "Job workspace building finished");

            ResultHolder_.Result = result;
            return std::move(ResultHolder_);
        }).AsyncVia(Invoker_));
}

////////////////////////////////////////////////////////////////////////////////

class TSimpleJobWorkspaceBuilder
    : public TJobWorkspaceBuilder
{
public:
    TSimpleJobWorkspaceBuilder(
        IInvokerPtr invoker,
        TJobWorkspaceBuildingContext context,
        IJobDirectoryManagerPtr directoryManager)
        : TJobWorkspaceBuilder(
            std::move(invoker),
            std::move(context),
            std::move(directoryManager))
    {
        YT_LOG_DEBUG("Creating simple job workspace builder");
    }

    ~TSimpleJobWorkspaceBuilder()
    {
        YT_LOG_DEBUG("Destroying simple job workspace builder");
    }

private:
    void MakeArtifactSymlinks()
    {
        const auto& slot = Context_.Slot;

        YT_LOG_DEBUG(
            "Making artifact symlinks (SymlinkCount: %v)",
            std::size(Context_.Artifacts));

        for (const auto& artifact : Context_.Artifacts) {
            // Artifact is passed into the job via symlink.
            if (!artifact.BypassArtifactCache && !artifact.CopyFile) {
                YT_VERIFY(artifact.Chunk);

                YT_LOG_INFO(
                    "Making symlink for artifact (FileName: %v, Executable: "
                    "%v, SandboxKind: %v, CompressedDataSize: %v)",
                    artifact.Name,
                    artifact.Executable,
                    artifact.SandboxKind,
                    artifact.Key.GetCompressedDataSize());

                auto sandboxPath = slot->GetSandboxPath(artifact.SandboxKind);
                auto symlinkPath =
                    CombinePaths(sandboxPath, artifact.Name);

                WaitFor(slot->MakeLink(
                    Context_.Job->GetId(),
                    artifact.Name,
                    artifact.SandboxKind,
                    artifact.Chunk->GetFileName(),
                    symlinkPath,
                    artifact.Executable))
                    .ThrowOnError();

                YT_LOG_INFO(
                    "Symlink for artifact is successfully made (FileName: %v, Executable: "
                    "%v, SandboxKind: %v, CompressedDataSize: %v)",
                    artifact.Name,
                    artifact.Executable,
                    artifact.SandboxKind,
                    artifact.Key.GetCompressedDataSize());
            } else {
                YT_VERIFY(artifact.SandboxKind == ESandboxKind::User);
            }
        }

        YT_LOG_DEBUG("Artifact symlinks are made");
    }

    TRootFS MakeWritableRootFS()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_VERIFY(ResultHolder_.RootVolume);

        auto binds = Context_.Binds;

        for (const auto& bind : ResultHolder_.RootBinds) {
            binds.push_back(bind);
        }

        return TRootFS {
            .RootPath = ResultHolder_.RootVolume->GetPath(),
            .IsRootReadOnly = false,
            .Binds = std::move(binds),
        };
    }

    TFuture<void> DoPrepareSandboxDirectories()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::DownloadingArtifacts);
        SetJobPhase(EJobPhase::PreparingSandboxDirectories);

        YT_LOG_INFO("Started preparing sandbox directories");

        const auto& slot = Context_.Slot;

        ResultHolder_.TmpfsPaths = WaitFor(slot->PrepareSandboxDirectories(Context_.UserSandboxOptions))
            .ValueOrThrow();

        MakeArtifactSymlinks();

        YT_LOG_INFO("Finished preparing sandbox directories");

        return VoidFuture;
    }

    TFuture<void> DoPrepareRootVolume()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_LOG_DEBUG("Root volume preparation is not supported in simple workspace");

        ValidateJobPhase(EJobPhase::PreparingSandboxDirectories);
        SetJobPhase(EJobPhase::PreparingRootVolume);

        return VoidFuture;
    }

    TFuture<void> DoRunSetupCommand()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_LOG_DEBUG("Setup command is not supported in simple workspace");

        ValidateJobPhase(EJobPhase::PreparingRootVolume);
        SetJobPhase(EJobPhase::RunningSetupCommands);

        return VoidFuture;
    }

    TFuture<void> DoRunGpuCheckCommand()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_LOG_DEBUG("Gpu check is not supported in simple workspace");

        ValidateJobPhase(EJobPhase::RunningSetupCommands);
        SetJobPhase(EJobPhase::RunningGpuCheckCommand);

        return VoidFuture;
    }
};

////////////////////////////////////////////////////////////////////////////////

TJobWorkspaceBuilderPtr CreateSimpleJobWorkspaceBuilder(
    IInvokerPtr invoker,
    TJobWorkspaceBuildingContext context,
    IJobDirectoryManagerPtr directoryManager)
{
    return New<TSimpleJobWorkspaceBuilder>(
        std::move(invoker),
        std::move(context),
        std::move(directoryManager));
}

////////////////////////////////////////////////////////////////////////////////

#ifdef _linux_

class TPortoJobWorkspaceBuilder
    : public TJobWorkspaceBuilder
{
public:
    TPortoJobWorkspaceBuilder(
        IInvokerPtr invoker,
        TJobWorkspaceBuildingContext context,
        IJobDirectoryManagerPtr directoryManager)
        : TJobWorkspaceBuilder(
            std::move(invoker),
            std::move(context),
            std::move(directoryManager))
    {
        YT_LOG_DEBUG("Creating porto job workspace builder");
    }

    ~TPortoJobWorkspaceBuilder()
    {
        YT_LOG_DEBUG("Destroying porto job workspace builder");
    }

private:
    void MakeArtifactSymlinks()
    {
        const auto& slot = Context_.Slot;

        YT_LOG_DEBUG(
            "Making artifact symlinks (ArtifactCount: %v)",
            std::size(Context_.Artifacts));

        for (const auto& artifact : Context_.Artifacts) {
            // Artifact is passed into the job via symlink.
            if (!artifact.BypassArtifactCache && !artifact.CopyFile) {
                YT_VERIFY(artifact.Chunk);

                YT_LOG_INFO(
                    "Making symlink for artifact (FileName: %v, Executable: "
                    "%v, SandboxKind: %v, CompressedDataSize: %v)",
                    artifact.Name,
                    artifact.Executable,
                    artifact.SandboxKind,
                    artifact.Key.GetCompressedDataSize());

                auto sandboxPath = slot->GetSandboxPath(artifact.SandboxKind);
                auto symlinkPath =
                    CombinePaths(sandboxPath, artifact.Name);

                WaitFor(slot->MakeLink(
                    Context_.Job->GetId(),
                    artifact.Name,
                    artifact.SandboxKind,
                    artifact.Chunk->GetFileName(),
                    symlinkPath,
                    artifact.Executable))
                    .ThrowOnError();

                YT_LOG_INFO(
                    "Symlink for artifact is successfully made(FileName: %v, Executable: "
                    "%v, SandboxKind: %v, CompressedDataSize: %v)",
                    artifact.Name,
                    artifact.Executable,
                    artifact.SandboxKind,
                    artifact.Key.GetCompressedDataSize());
            } else {
                YT_VERIFY(artifact.SandboxKind == ESandboxKind::User);
            }
        }

        YT_LOG_DEBUG("Artifact symlinks are made");
    }

    void SetArtifactPermissions()
    {
        YT_LOG_DEBUG(
            "Setting permissions for artifactifacts (ArctifactCount: %v)",
            std::size(Context_.Artifacts));

        for (const auto& artifact : Context_.Artifacts) {
            if (!artifact.BypassArtifactCache && !artifact.CopyFile) {
                YT_VERIFY(artifact.Chunk);

                int permissions = artifact.Executable ? 0755 : 0644;

                YT_LOG_INFO(
                    "Set permissions for artifact (FileName: %v, Permissions: "
                    "%v, SandboxKind: %v, CompressedDataSize: %v)",
                    artifact.Name,
                    permissions,
                    artifact.SandboxKind,
                    artifact.Key.GetCompressedDataSize());

                SetPermissions(
                    artifact.Chunk->GetFileName(),
                    permissions);
            } else {
                YT_VERIFY(artifact.SandboxKind == ESandboxKind::User);
            }
        }

        YT_LOG_DEBUG("Permissions for artifactifacts set");
    }

    TFuture<void> DoPrepareSandboxDirectories()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::DownloadingArtifacts);
        SetJobPhase(EJobPhase::PreparingSandboxDirectories);

        YT_LOG_INFO("Started preparing sandbox directories");

        const auto& slot = Context_.Slot;
        ResultHolder_.TmpfsPaths = WaitFor(slot->PrepareSandboxDirectories(Context_.UserSandboxOptions))
            .ValueOrThrow();

        if (Context_.LayerArtifactKeys.empty() || !Context_.UserSandboxOptions.EnableArtifactBinds) {
            MakeArtifactSymlinks();
        } else {
            SetArtifactPermissions();
        }

        YT_LOG_INFO("Finished preparing sandbox directories");

        return VoidFuture;
    }

    TRootFS MakeWritableRootFS()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_VERIFY(ResultHolder_.RootVolume);

        auto binds = Context_.Binds;

        for (const auto& bind : ResultHolder_.RootBinds) {
            binds.push_back(bind);
        }

        return TRootFS{
            .RootPath = ResultHolder_.RootVolume->GetPath(),
            .IsRootReadOnly = false,
            .Binds = binds
        };
    }

    TFuture<void> DoPrepareRootVolume()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::PreparingSandboxDirectories);
        SetJobPhase(EJobPhase::PreparingRootVolume);

        const auto& slot = Context_.Slot;
        const auto& layerArtifactKeys = Context_.LayerArtifactKeys;

        if (!layerArtifactKeys.empty()) {
            VolumePrepareStartTime_ = TInstant::Now();
            UpdateTimers_.Fire(MakeStrong(this));

            YT_LOG_INFO("Preparing root volume (LayerCount: %v)", layerArtifactKeys.size());

            for (const auto& layer : layerArtifactKeys) {
                i64 layerSize = layer.GetCompressedDataSize();
                UpdateArtifactStatistics(layerSize, slot->IsLayerCached(layer));
            }

            return slot->PrepareRootVolume(
                layerArtifactKeys,
                Context_.ArtifactDownloadOptions,
                Context_.UserSandboxOptions)
                .Apply(BIND([this, this_ = MakeStrong(this)] (const TErrorOr<IVolumePtr>& volumeOrError) {
                    if (!volumeOrError.IsOK()) {
                        YT_LOG_DEBUG("Failed to prepare root volume");

                        THROW_ERROR_EXCEPTION(
                            TError(EErrorCode::RootVolumePreparationFailed, "Failed to prepare artifacts")
                                << volumeOrError);
                    }

                    YT_LOG_DEBUG("Root volume prepared");

                    VolumePrepareFinishTime_ = TInstant::Now();
                    UpdateTimers_.Fire(MakeStrong(this));
                    ResultHolder_.RootVolume = volumeOrError.Value();
                }));
        } else {
            YT_LOG_DEBUG("Root volume preparation is not needed");
            return VoidFuture;
        }
    }

    TFuture<void> DoRunSetupCommand()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::PreparingRootVolume);
        SetJobPhase(EJobPhase::RunningSetupCommands);

        if (Context_.LayerArtifactKeys.empty()) {
            return VoidFuture;
        }

        const auto &slot = Context_.Slot;

        const auto& commands = Context_.SetupCommands;
        ResultHolder_.SetupCommandCount = commands.size();

        if (commands.empty()) {
            YT_LOG_DEBUG("No setup command is needed");

            return VoidFuture;
        }

        YT_LOG_INFO("Running setup commands");

        return slot->RunSetupCommands(
            Context_.Job->GetId(),
            commands,
            MakeWritableRootFS(),
            Context_.CommandUser,
            /*devices*/ std::nullopt,
            /*startIndex*/ 0);
    }

    TFuture<void> DoRunGpuCheckCommand()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ValidateJobPhase(EJobPhase::RunningSetupCommands);
        SetJobPhase(EJobPhase::RunningGpuCheckCommand);

        if (Context_.NeedGpuCheck) {
            TJobGpuCheckerContext settings {
                .Slot = Context_.Slot,
                .Job = Context_.Job,
                .RootFs = MakeWritableRootFS(),
                .CommandUser = Context_.CommandUser,

                .GpuCheckBinaryPath = *Context_.GpuCheckBinaryPath,
                .GpuCheckBinaryArgs = *Context_.GpuCheckBinaryArgs,
                .GpuCheckType = Context_.GpuCheckType,
                .CurrentStartIndex = ResultHolder_.SetupCommandCount,
                // It is preliminary (not extra) GPU check.
                .TestExtraGpuCheckCommandFailure = false,
                .GpuDevices = Context_.GpuDevices
            };

            auto checker = New<TJobGpuChecker>(std::move(settings), Logger);

            checker->SubscribeRunCheck(BIND_NO_PROPAGATE([this, this_ = MakeStrong(this)] () {
                GpuCheckStartTime_ = TInstant::Now();
                UpdateTimers_.Fire(MakeStrong(this));
            }));

            checker->SubscribeFinishCheck(BIND_NO_PROPAGATE([this, this_ = MakeStrong(this)] () {
                GpuCheckFinishTime_ = TInstant::Now();
                UpdateTimers_.Fire(MakeStrong(this));
            }));

            YT_LOG_DEBUG("Starting preliminary gpu check");

            return BIND(&TJobGpuChecker::RunGpuCheck, std::move(checker))
                .AsyncVia(Invoker_)
                .Run()
                .Apply(BIND([this, this_ = MakeStrong(this)] (const TError& result) {
                    ValidateJobPhase(EJobPhase::RunningGpuCheckCommand);
                    if (!result.IsOK()) {
                        YT_LOG_WARNING(result, "Preliminary GPU check command failed");

                        auto checkError = TError(EErrorCode::GpuCheckCommandFailed, "Preliminary GPU check command failed")
                            << result;
                        THROW_ERROR checkError;
                    }

                    YT_LOG_DEBUG("GPU check command finished");
                }).AsyncVia(Invoker_));
        } else {
            YT_LOG_DEBUG("No preliminary gpu check is needed");

            return VoidFuture;
        }
    }
};

TJobWorkspaceBuilderPtr CreatePortoJobWorkspaceBuilder(
    IInvokerPtr invoker,
    TJobWorkspaceBuildingContext context,
    IJobDirectoryManagerPtr directoryManager)
{
    return New<TPortoJobWorkspaceBuilder>(
        std::move(invoker),
        std::move(context),
        std::move(directoryManager));
}

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecNode
