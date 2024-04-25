#include "job_resource_manager.h"

#include "private.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>
#include <yt/yt/server/node/cluster_node/config.h>
#include <yt/yt/server/node/cluster_node/dynamic_config_manager.h>
#include <yt/yt/server/node/cluster_node/node_resource_manager.h>

#include <yt/yt/server/node/exec_node/bootstrap.h>
#include <yt/yt/server/node/exec_node/job_controller.h>
#include <yt/yt/server/node/exec_node/chunk_cache.h>
#include <yt/yt/server/node/exec_node/gpu_manager.h>
#include <yt/yt/server/node/exec_node/slot.h>
#include <yt/yt/server/node/exec_node/slot_manager.h>

#include <yt/yt/server/lib/exec_node/config.h>

#include <yt/yt/server/lib/job_agent/config.h>

#include <yt/yt/ytlib/node_tracker_client/helpers.h>

#include <yt/yt/ytlib/misc/memory_usage_tracker.h>

#include <yt/yt/core/actions/new_with_offloaded_dtor.h>

#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/misc/atomic_object.h>
#include <yt/yt/core/misc/proc.h>

#include <yt/yt/core/net/helpers.h>

#include <library/cpp/yt/memory/atomic_intrusive_ptr.h>

namespace NYT::NJobAgent {

////////////////////////////////////////////////////////////////////////////////

using namespace NConcurrency;
using namespace NClusterNode;
using namespace NProfiling;
using namespace NNodeTrackerClient;
using namespace NNet;
using namespace NLogging;
using namespace NYson;
using namespace NYTree;

using NNodeTrackerClient::NProto::TNodeResourceLimitsOverrides;
using NNodeTrackerClient::NProto::TDiskResources;

////////////////////////////////////////////////////////////////////////////////

static const TLogger Logger("JobResourceManager");

////////////////////////////////////////////////////////////////////////////////

class TResourceHolder::TAcquiredResources
{
public:
    TAcquiredResources(
        TJobResourceManager::TImpl* jobResourceManagerImpl,
        TMemoryUsageTrackerGuard&& userMemoryGuard,
        TMemoryUsageTrackerGuard&& systemMemoryGuard,
        ISlotPtr&& userSlot,
        std::vector<ISlotPtr>&& gpuSlots,
        std::vector<int> ports) noexcept;
    ~TAcquiredResources();

    TMemoryUsageTrackerGuard UserMemoryGuard;
    TMemoryUsageTrackerGuard SystemMemoryGuard;
    ISlotPtr UserSlot;
    std::vector<ISlotPtr> GpuSlots;
    std::vector<int> Ports;

private:
    TJobResourceManager::TImpl* const JobResourceManagerImpl_;
};

////////////////////////////////////////////////////////////////////////////////

class TJobResourceManager::TImpl
    : public TJobResourceManager
{
public:
    DEFINE_SIGNAL_OVERRIDE(void(), ResourcesAcquired);
    DEFINE_SIGNAL_OVERRIDE(void(EResourcesConsumerType, bool), ResourcesReleased);
    DEFINE_SIGNAL_OVERRIDE(void(TResourceHolderPtr), ResourceUsageOverdraftOccurred);

    DEFINE_SIGNAL_OVERRIDE(
        void(i64 mapped),
        ReservedMemoryOvercommited);

public:
    explicit TImpl(IBootstrapBase* bootstrap)
        : Bootstrap_(bootstrap)
        , StaticConfig_(bootstrap->GetConfig()->JobResourceManager)
        , DynamicConfig_(New<TJobResourceManagerDynamicConfig>())
        , NodeMemoryUsageTracker_(Bootstrap_->GetNodeMemoryUsageTracker())
        , SystemMemoryUsageTracker_(NodeMemoryUsageTracker_->WithCategory(EMemoryCategory::SystemJobs))
        , UserMemoryUsageTracker_(NodeMemoryUsageTracker_->WithCategory(EMemoryCategory::UserJobs))
        , Profiler_("/job_controller")
        , MajorPageFaultsGauge_(Profiler_.Gauge("/major_page_faults"))
        , FreeMemoryWatermarkMultiplierGauge_(Profiler_.Gauge("/free_memory_watermark_multiplier"))
        , FreeMemoryWatermarkAddedMemoryGauge_(Profiler_.Gauge("/free_memory_watermark_added_memory"))
        , FreeMemoryWatermarkIsIncreasedGauge_(Profiler_.Gauge("/free_memory_watermark_is_increased"))
    {
        YT_VERIFY(StaticConfig_);
        VERIFY_INVOKER_THREAD_AFFINITY(Bootstrap_->GetJobInvoker(), JobThread);

        Profiler_.AddProducer("/resource_limits", ResourceLimitsBuffer_);
        Profiler_.AddProducer("/resource_usage", ResourceUsageBuffer_);
        Profiler_.AddProducer("/pending_resource_usage", PendingResourceUsageBuffer_);

        if (StaticConfig_->PortSet) {
            FreePorts_ = *StaticConfig_->PortSet;
        } else {
            for (int index = 0; index < StaticConfig_->PortCount; ++index) {
                FreePorts_.insert(StaticConfig_->StartPort + index);
            }
        }
    }

    void Initialize() override
    {
        auto dynamicConfig = GetDynamicConfig();

        ProfilingExecutor_ = New<TPeriodicExecutor>(
            Bootstrap_->GetJobInvoker(),
            BIND_NO_PROPAGATE(&TImpl::OnProfiling, MakeWeak(this)),
            dynamicConfig->ProfilingPeriod);

        ReservedMappedMemoryChecker_ = New<TPeriodicExecutor>(
            Bootstrap_->GetJobInvoker(),
            BIND_NO_PROPAGATE(&TImpl::CheckReservedMappedMemory, MakeWeak(this)),
            std::nullopt);

        MemoryPressureDetector_ = New<TPeriodicExecutor>(
            Bootstrap_->GetJobInvoker(),
            BIND_NO_PROPAGATE(&TImpl::CheckMemoryPressure, MakeWeak(this)),
            std::nullopt);
    }

    void Start() override
    {
        ProfilingExecutor_->Start();
        ReservedMappedMemoryChecker_->Start();

        if (Bootstrap_->IsExecNode()) {
            MemoryPressureDetector_->Start();
        }
    }

    void OnDynamicConfigChanged(
        const TJobResourceManagerDynamicConfigPtr& /*oldConfig*/,
        const TJobResourceManagerDynamicConfigPtr& newConfig) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        ProfilingExecutor_->SetPeriod(
            newConfig->ProfilingPeriod);

        MemoryPressureDetector_->SetPeriod(newConfig->MemoryPressureDetector->CheckPeriod);

        if (newConfig->MappedMemoryController) {
            ReservedMappedMemoryChecker_->SetPeriod(newConfig->MappedMemoryController->CheckPeriod);
        }

        DynamicConfig_.Store(newConfig);
    }

    void OnProfiling()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        ResourceUsageBuffer_->Update([this] (ISensorWriter* writer) {
            ProfileResources(writer, GetResourceUsage());
        });

        PendingResourceUsageBuffer_->Update([this] (ISensorWriter* writer) {
            auto guard = ReaderGuard(ResourcesLock_);
            ProfileResources(writer, PendingResourceUsage_);
        });

        ResourceLimitsBuffer_->Update([this] (ISensorWriter* writer) {
            ProfileResources(writer, GetResourceLimits());
        });

        if (Bootstrap_->IsExecNode()) {
            MajorPageFaultsGauge_.Update(LastMajorPageFaultCount_);

            auto dynamicConfig = GetDynamicConfig();
            if (FreeMemoryWatermarkMultiplier_ != 1.0 && dynamicConfig->MemoryPressureDetector->Enabled) {
                FreeMemoryWatermarkMultiplierGauge_.Update(FreeMemoryWatermarkMultiplier_);
                FreeMemoryWatermarkAddedMemoryGauge_.Update(GetFreeMemoryWatermark() - dynamicConfig->FreeMemoryWatermark);
                FreeMemoryWatermarkIsIncreasedGauge_.Update(1);
            }
        }
    }

    TJobResourceManagerDynamicConfigPtr GetDynamicConfig() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return DynamicConfig_.Acquire();
    }

    void SetActualVcpu(TJobResources& resources) const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        resources.VCpu = static_cast<double>(NVectorHdrf::TCpuResource(resources.Cpu * GetCpuToVCpuFactor()));
    }

    TJobResources GetResourceLimits() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TJobResources result;
        auto resourceLimitsOverrides = ComputeEffectiveResourceLimitsOverrides();

        #define XX(name, Name) \
            result.Name = (resourceLimitsOverrides.has_##name() \
                ? resourceLimitsOverrides.name() \
                : StaticConfig_->ResourceLimits->Name);
        ITERATE_NODE_RESOURCE_LIMITS_OVERRIDES(XX)
        #undef XX

        const auto& flavors = Bootstrap_->GetConfig()->Flavors;

        if (std::find(flavors.begin(), flavors.end(), ENodeFlavor::Data) == flavors.end()) {
            result.MergeSlots = 0;
            result.RemovalSlots = 0;
            result.ReplicationSlots = 0;
            result.RepairSlots = 0;
            result.SealSlots = 0;
            result.AutotomySlots = 0;
            result.ReincarnationSlots = 0;
        }

        if (Bootstrap_->IsExecNode()) {
            const auto& execNodeBootstrap = Bootstrap_->GetExecNodeBootstrap();
            auto slotManager = execNodeBootstrap->GetSlotManager();
            auto gpuManager = execNodeBootstrap->GetGpuManager();

            auto scheduleJobEnabled =
                execNodeBootstrap->GetChunkCache()->IsEnabled() &&
                !execNodeBootstrap->GetJobController()->AreJobsDisabled() &&
                !Bootstrap_->IsReadOnly() &&
                slotManager->IsEnabled();

            result.UserSlots = scheduleJobEnabled
                ? slotManager->GetSlotCount()
                : 0;

            result.Gpu = resourceLimitsOverrides.has_gpu()
                ? std::min(gpuManager->GetTotalGpuCount(), resourceLimitsOverrides.gpu())
                : gpuManager->GetTotalGpuCount();
        }

        // NB: Some categories can have no explicit limit.
        // Therefore we need bound memory limit by actually available memory.
        auto getUsedMemory = [&] (IMemoryUsageTracker* memoryUsageTracker) {
            return std::max<i64>(
                0,
                memoryUsageTracker->GetUsed() + NodeMemoryUsageTracker_->GetTotalFree() - GetFreeMemoryWatermark());
        };
        result.UserMemory = std::min(
            UserMemoryUsageTracker_->GetLimit(),
            getUsedMemory(UserMemoryUsageTracker_.Get()));
        result.SystemMemory = std::min(
            SystemMemoryUsageTracker_->GetLimit(),
            getUsedMemory(SystemMemoryUsageTracker_.Get()));

        const auto& nodeResourceManager = Bootstrap_->GetNodeResourceManager();
        result.Cpu = nodeResourceManager->GetJobsCpuLimit();
        result.VCpu = static_cast<double>(NVectorHdrf::TCpuResource(result.Cpu * GetCpuToVCpuFactor()));

        return result;
    }

    TJobResources LoadResourceUsage() const
    {
        auto guard = ReaderGuard(ResourcesLock_);
        return ResourceUsage_;
    }

    TJobResources GetResourceUsage(bool includePending = false) const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TJobResources resourceUsage;

        std::optional<TJobResources> maybePendingResourceUsage;

        {
            auto guard = ReaderGuard(ResourcesLock_);

            resourceUsage = ResourceUsage_;

            if (includePending) {
                maybePendingResourceUsage = PendingResourceUsage_;
            }
        }

        if (maybePendingResourceUsage) {
            resourceUsage += PendingResourceUsage_;

            resourceUsage.UserSlots = ResourceUsage_.UserSlots;
            resourceUsage.Gpu = ResourceUsage_.Gpu;
        }

        SetActualVcpu(resourceUsage);

        return resourceUsage;
    }

    TJobResources CalculateFreeResources(const TJobResources& resourceLimits, const TJobResources& resourceUsage) const
    {
        return resourceLimits - resourceUsage;
    }

    TJobResources CalculateSpareResources(const TJobResources& resourceLimits, const TJobResources& resourceUsage) const
    {
        return MakeNonnegative(CalculateFreeResources(resourceLimits, resourceUsage));
    }

    TJobResources GetFreeResources() const
    {
        return CalculateFreeResources(GetResourceLimits(), GetResourceUsage());
    }

    bool CheckMemoryOverdraft(const TJobResources& delta) override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        // Only "cpu" and "user_memory" can be increased.
        // Network decreases by design. Cpu increasing is handled in AdjustResources.
        // Other resources are not reported by job proxy (see TSupervisorService::UpdateResourceUsage).

        if (delta.UserMemory > 0) {
            bool watermarkReached = NodeMemoryUsageTracker_->GetTotalFree() <= GetFreeMemoryWatermark();
            if (watermarkReached) {
                return true;
            }

            auto error = UserMemoryUsageTracker_->TryAcquire(delta.UserMemory);
            if (!error.IsOK()) {
                return true;
            }

            UserMemoryUsageTracker_->Release(delta.UserMemory);
        }

        return false;
    }

    i64 GetFreeMemoryWatermark() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto dynamicConfig = GetDynamicConfig();

        return dynamicConfig->MemoryPressureDetector->Enabled
            ? dynamicConfig->FreeMemoryWatermark * FreeMemoryWatermarkMultiplier_
            : dynamicConfig->FreeMemoryWatermark;
    }


    TDiskResources GetDiskResources() const override
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        if (Bootstrap_->IsExecNode()) {
            const auto& slotManager = Bootstrap_->GetExecNodeBootstrap()->GetSlotManager();
            return slotManager->GetDiskResources();
        } else {
            return TDiskResources{};
        }
    }

    void SetResourceLimitsOverrides(const TNodeResourceLimitsOverrides& resourceLimits) override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        ResourceLimitsOverrides_.Store(resourceLimits);
    }

    TNodeResourceLimitsOverrides ComputeEffectiveResourceLimitsOverrides() const
    {
        TNodeResourceLimitsOverrides resourceLimits;
        const auto& resourceLimitsOverrides = ResourceLimitsOverrides_.Load();
        const auto& dynamicConfigOverrides = Bootstrap_->GetDynamicConfigManager()->GetConfig()->ResourceLimits->Overrides;

        #define XX(name, Name) \
            if (resourceLimitsOverrides.has_##name()) { \
                resourceLimits.set_##name(resourceLimitsOverrides.name()); \
            } else if (dynamicConfigOverrides->Name) { \
                resourceLimits.set_##name(*dynamicConfigOverrides->Name); \
            }
        ITERATE_NODE_RESOURCE_LIMITS_DYNAMIC_CONFIG_OVERRIDES(XX)
        #undef XX
        return resourceLimits;
    }

    double GetCpuToVCpuFactor() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto dynamicConfig = DynamicConfig_.Acquire();
        if (dynamicConfig->EnableCpuToVCpuFactor) {
            if (dynamicConfig->CpuToVCpuFactor) {
                return dynamicConfig->CpuToVCpuFactor.value();
            }
            if (StaticConfig_->CpuToVCpuFactor) {
                return StaticConfig_->CpuToVCpuFactor.value();
            }
            if (dynamicConfig->CpuModelToCpuToVCpuFactor && StaticConfig_->CpuModel) {
                const auto& cpuModel = *StaticConfig_->CpuModel;
                const auto& cpuModelToCpuToVCpuFactor = *dynamicConfig->CpuModelToCpuToVCpuFactor;
                if (auto it = cpuModelToCpuToVCpuFactor.find(cpuModel); it != cpuModelToCpuToVCpuFactor.end()) {
                    return it->second;
                }
            }
        }

        return 1.0;
    }

    ISlotPtr AcquireUserSlot(
        const TJobResources& neededResources,
        const TJobResourceAttributes& resourceAttributes)
    {
        YT_VERIFY(Bootstrap_->IsExecNode());

        NScheduler::NProto::TDiskRequest diskRequest;
        diskRequest.set_disk_space(neededResources.DiskSpaceRequest);
        diskRequest.set_inode_count(neededResources.InodeRequest);

        if (resourceAttributes.MediumIndex) {
            diskRequest.set_medium_index(resourceAttributes.MediumIndex.value());
        }

        NScheduler::NProto::TCpuRequest cpuRequest;
        cpuRequest.set_cpu(neededResources.Cpu);
        cpuRequest.set_allow_idle_cpu_policy(resourceAttributes.AllowIdleCpuPolicy);

        YT_LOG_INFO(
            "Acquiring slot (DiskRequest: %v, CpuRequest: %v)",
            diskRequest,
            cpuRequest);

        auto slotManager = Bootstrap_->GetExecNodeBootstrap()->GetSlotManager();
        auto userSlot = slotManager->AcquireSlot(diskRequest, cpuRequest);

        YT_VERIFY(userSlot);

        return userSlot;
    }

    std::vector<ISlotPtr> AcquireGpuSlots(const TJobResources& neededResources)
    {
        YT_VERIFY(Bootstrap_->IsExecNode());

        int gpuCount = neededResources.Gpu;
        YT_LOG_DEBUG("Acquiring GPU slots (Count: %v)", gpuCount);
        auto acquireResult = Bootstrap_
            ->GetExecNodeBootstrap()
            ->GetGpuManager()
            ->AcquireGpuSlots(gpuCount);

        THROW_ERROR_EXCEPTION_IF(
            !acquireResult.IsOK(),
            TError("GPU slot acquisition failed", gpuCount)
                << TErrorAttribute("gpu_count", gpuCount)
                << acquireResult);

        auto result = acquireResult.Value();

        std::vector<ISlotPtr> slots;
        std::vector<int> deviceIndices;

        slots.reserve(result.size());
        deviceIndices.reserve(result.size());

        for (auto& slot : result) {
            deviceIndices.push_back(slot->GetDeviceIndex());
            slots.push_back(std::move(slot));
        }

        YT_LOG_DEBUG("GPU slots acquired (DeviceIndices: %v)", deviceIndices);

        return slots;
    }

    void OnResourceAcquiringStarted()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_VERIFY(!std::exchange(HasActiveResourceAcquiring_, true));

        YT_VERIFY(!std::exchange(ShouldNotifyResourcesUpdated_, false));
    }

    void OnResourceAcquiringFinished()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_VERIFY(std::exchange(HasActiveResourceAcquiring_, false));

        if (ShouldNotifyResourcesUpdated_) {
            ResourcesAcquired_.Fire();
            ShouldNotifyResourcesUpdated_ = false;
        }
    }

    void OnResourceHolderRegistered(const TLogger& Logger, const TResourceHolder* resourceHolder)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        TJobResources currentResourceUsage;
        TJobResources pendingResourceUsage;

        YT_VERIFY(resourceHolder->State_ == EResourcesState::Pending);
        const auto& resources = resourceHolder->BaseResourceUsage_;
        {
            auto guard = WriterGuard(ResourcesLock_);
            PendingResourceUsage_ += resources;
            ++PendingResourceHolderCount_;

            currentResourceUsage = ResourceUsage_;
            pendingResourceUsage = PendingResourceUsage_;
        }

        YT_LOG_DEBUG(
            "Resource holder registered (Resources: %v, ResourceUsage: %v, PendingResourceUsage: %v)",
            FormatResources(resources),
            FormatResources(currentResourceUsage),
            FormatResources(pendingResourceUsage));
    }

    bool TryReserveResources(const TLogger& Logger, const TJobResources& resources)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        TJobResources currentResourceUsage;
        TJobResources pendingResourceUsage;

        auto resourceLimits = GetResourceLimits();

        {
            auto guard = WriterGuard(ResourcesLock_);

            if (!HasEnoughResources(resources, ResourceUsage_, resourceLimits)) {
                YT_LOG_DEBUG(
                    "Not enough resources (NeededResources: %v, ResourceUsage: %v)",
                    FormatResources(resources),
                    FormatResourceUsage(ResourceUsage_, resourceLimits));

                return false;
            }

            ResourceUsage_ += resources;
            PendingResourceUsage_ -= resources;
            --PendingResourceHolderCount_;

            currentResourceUsage = ResourceUsage_;
            pendingResourceUsage = PendingResourceUsage_;
        }

        YT_LOG_DEBUG(
            "Resources reserved (Resources: %v, ResourceUsage: %v, PendingResourceUsage: %v)",
            FormatResources(resources),
            FormatResources(currentResourceUsage),
            FormatResources(pendingResourceUsage));

        return true;
    }

    void OnResourcesAcquisitionFailed(
        TResourceHolderPtr resourceHolder,
        ISlotPtr&& userSlot,
        std::vector<ISlotPtr>&& gpuSlots,
        std::vector<int>&& ports,
        TJobResources resources)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        const auto& Logger = resourceHolder->GetLogger();

        userSlot.Reset();
        gpuSlots.clear();

        TJobResources currentResourceUsage;
        TJobResources pendingResourceUsage;

        {
            auto guard = WriterGuard(ResourcesLock_);
            PendingResourceUsage_ += resources;
            ResourceUsage_ -= resources;
            ++PendingResourceHolderCount_;

            DoReleasePorts(Logger, ports);

            currentResourceUsage = ResourceUsage_;
            pendingResourceUsage = PendingResourceUsage_;
        }

        YT_LOG_DEBUG(
            "Resources acquisition failed (Resources: %v, ResourceUsage: %v, PendingResourceUsage: %v)",
            FormatResources(resources),
            FormatResources(currentResourceUsage),
            FormatResources(pendingResourceUsage));

        NotifyResourcesReleased(resourceHolder->ResourcesConsumerType, /*fullyReleased*/ true);
    }

    bool AcquireResourcesFor(TResourceHolderPtr resourceHolder)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        const auto neededResources = resourceHolder->GetResourceUsage();
        const auto& resourceAttributes = resourceHolder->ResourceAttributes_;
        auto portCount = resourceHolder->PortCount_;

        const auto& Logger = resourceHolder->GetLogger();

        YT_LOG_DEBUG(
            "Trying to acquire resources (NeededResources: %v, PortCount: %v)",
            FormatResources(neededResources),
            portCount);

        ISlotPtr userSlot;
        std::vector<ISlotPtr> gpuSlots;
        std::vector<int> ports;

        if (!TryReserveResources(Logger, neededResources)) {
            return false;
        }

        auto resourceAcquisitionFailedGuard = Finally([&] {
            OnResourcesAcquisitionFailed(
                resourceHolder,
                std::move(userSlot),
                std::move(gpuSlots),
                std::move(ports),
                neededResources);
        });

        if (resourceAttributes.CudaToolkitVersion) {
            YT_VERIFY(Bootstrap_->IsExecNode());

            Bootstrap_
                ->GetExecNodeBootstrap()
                ->GetGpuManager()
                ->VerifyCudaToolkitDriverVersion(resourceAttributes.CudaToolkitVersion.value());
        }

        i64 userMemory = neededResources.UserMemory;
        i64 systemMemory = neededResources.SystemMemory;
        if (userMemory > 0 || systemMemory > 0) {
            bool reachedWatermark = NodeMemoryUsageTracker_->GetTotalFree() <= GetFreeMemoryWatermark();
            if (reachedWatermark) {
                YT_LOG_DEBUG("Not enough memory; reached free memory watermark");
                return false;
            }
        }

        TMemoryUsageTrackerGuard userMemoryGuard;
        TMemoryUsageTrackerGuard systemMemoryGuard;

        if (userMemory > 0) {
            auto errorOrGuard = TMemoryUsageTrackerGuard::TryAcquire(UserMemoryUsageTracker_, userMemory);
            if (!errorOrGuard.IsOK()) {
                YT_LOG_DEBUG(errorOrGuard, "Not enough user memory");
                return false;
            }

            userMemoryGuard = std::move(errorOrGuard.Value());
        }

        if (systemMemory > 0) {
            auto errorOrGuard = TMemoryUsageTrackerGuard::TryAcquire(SystemMemoryUsageTracker_, systemMemory);
            if (!errorOrGuard.IsOK()) {
                YT_LOG_DEBUG(errorOrGuard, "Not enough system memory");
                return false;
            }

            systemMemoryGuard = std::move(errorOrGuard.Value());
        }

        if (neededResources.UserSlots == 0 && SystemMemoryUsageTracker_->IsExceeded()) {
            YT_LOG_DEBUG("Not enough system memory");
            return false;
        }

        if (portCount > 0) {
            YT_LOG_INFO("Allocating ports (PortCount: %v)", portCount);

            try {
                THashSet<int> freePorts;
                {
                    auto guard = ReaderGuard(ResourcesLock_);
                    freePorts = FreePorts_;
                }
                ports = AllocateFreePorts(portCount, freePorts, Logger);
            } catch (const std::exception& ex) {
                YT_LOG_ERROR(ex, "Error while allocating free ports (PortCount: %v)", portCount);
                return false;
            }

            if (std::ssize(ports) < portCount) {
                ports.clear();

                YT_LOG_DEBUG(
                    "Not enough bindable free ports (PortCount: %v, FreePortCount: %v)",
                    portCount,
                    ports.size());
                return false;
            }

            {
                auto guard = WriterGuard(ResourcesLock_);

                for (int port : ports) {
                    FreePorts_.erase(port);
                }
            }

            YT_LOG_DEBUG("Ports allocated (PortCount: %v, Ports: %v)", ports.size(), ports);
        }

        if (Bootstrap_->IsExecNode()) {
            auto slotManager = Bootstrap_->GetExecNodeBootstrap()->GetSlotManager();
            auto slotManagerCount = slotManager->GetUsedSlotCount();
            auto slotManagerLimit = slotManager->GetSlotCount();
            auto jobResourceManagerCount = LoadResourceUsage().UserSlots - neededResources.UserSlots;

            YT_LOG_FATAL_IF(
                slotManagerCount != jobResourceManagerCount,
                "Used slot count in slot manager must be equal JobResourceManager count (SlotManagerCount: %v/%v, JobResourceManagerCount: %v)",
                slotManagerCount,
                slotManagerLimit,
                jobResourceManagerCount);
        }

        try {
            if (neededResources.UserSlots > 0) {
                YT_VERIFY(Bootstrap_->IsExecNode());

                userSlot = AcquireUserSlot(neededResources, resourceAttributes);
            }

            if (neededResources.Gpu > 0) {
                YT_VERIFY(Bootstrap_->IsExecNode());

                gpuSlots = AcquireGpuSlots(neededResources);
            }
        } catch (const std::exception& ex) {
            // Provide job abort.
            THROW_ERROR_EXCEPTION(ex);
        }

        resourceAcquisitionFailedGuard.Release();
        ShouldNotifyResourcesUpdated_ = true;

        resourceHolder->SetAcquiredResources({
            this,
            std::move(userMemoryGuard),
            std::move(systemMemoryGuard),
            std::move(userSlot),
            std::move(gpuSlots),
            std::move(ports)});

        YT_LOG_DEBUG("Resources successfully allocated");

        return true;
    }

    void OnBaseResourcesReleased(
        EResourcesConsumerType resourcesConsumerType,
        const TLogger& Logger,
        const TJobResources& resources,
        const std::vector<int>& ports,
        bool resourceHolderStarted)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_VERIFY(resourceHolderStarted || ports.empty());

        TJobResources currentResourceUsage;
        TJobResources pendingResourceUsage;

        {
            auto guard = WriterGuard(ResourcesLock_);
            if (resourceHolderStarted) {
                ResourceUsage_ -= resources;
            } else {
                PendingResourceUsage_ -= resources;
                --PendingResourceHolderCount_;
            }

            DoReleasePorts(Logger, ports);

            currentResourceUsage = ResourceUsage_;
            pendingResourceUsage = PendingResourceUsage_;
        }

        if (resourceHolderStarted && resources.SystemMemory) {
            auto systemMemory = resources.SystemMemory;
            YT_VERIFY(systemMemory >= 0);

            SystemMemoryUsageTracker_->Release(systemMemory);
        }

        if (resourceHolderStarted && resources.UserMemory) {
            auto userMemory = resources.UserMemory;
            YT_VERIFY(userMemory >= 0);

            UserMemoryUsageTracker_->Release(userMemory);
        }

        YT_LOG_DEBUG(
            "Resources released (ResourceHolderStarted: %v, Delta: %v, ResourceUsage: %v, PendingResourceUsage: %v)",
            resourceHolderStarted,
            FormatResources(resources),
            FormatResources(currentResourceUsage),
            FormatResources(pendingResourceUsage));

        if (resourceHolderStarted) {
            NotifyResourcesReleased(resourcesConsumerType, /*fullyReleased*/ true);
        }
    }

    bool OnResourcesUpdated(
        TResourceHolder* resourceHolder,
        EResourcesConsumerType resourcesConsumerType,
        const TLogger& Logger,
        const TJobResources& resourceDelta)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        TJobResources currentResourceUsage;
        TJobResources pendingResourceUsage;

        {
            auto guard = WriterGuard(ResourcesLock_);
            ResourceUsage_ += resourceDelta;

            currentResourceUsage = ResourceUsage_;
            pendingResourceUsage = PendingResourceUsage_;
        }

        bool resourceUsageOverdraftOccurred = false;

        auto systemMemory = resourceDelta.SystemMemory;
        if (systemMemory > 0) {
            resourceUsageOverdraftOccurred |= !SystemMemoryUsageTracker_->Acquire(systemMemory);
        } else if (systemMemory < 0) {
            SystemMemoryUsageTracker_->Release(-systemMemory);
        }

        auto userMemory = resourceDelta.UserMemory;
        if (userMemory > 0) {
            resourceUsageOverdraftOccurred |= !UserMemoryUsageTracker_->Acquire(userMemory);
        } else if (userMemory < 0) {
            UserMemoryUsageTracker_->Release(-userMemory);
        }

        auto resourceLimits = GetResourceLimits();


        if (!Dominates(resourceDelta, ZeroJobResources())) {
            NotifyResourcesReleased(resourcesConsumerType, /*fullyReceived*/ false);
        }

        if (resourceUsageOverdraftOccurred) {
            YT_LOG_INFO(
                "Resource usage overdraft detected during updating resource usage (Delta: %v, ResourceUsage: %v, PendingResourceUsage: %v)",
                FormatResources(resourceDelta),
                FormatResourceUsage(currentResourceUsage, resourceLimits),
                FormatResources(pendingResourceUsage));

            ResourceUsageOverdraftOccurred_.Fire(MakeStrong(resourceHolder));
        } else {
            YT_LOG_DEBUG(
                "Resource usage updated (Delta: %v, ResourceUsage: %v, PendingResourceUsage: %v)",
                FormatResources(resourceDelta),
                FormatResourceUsage(currentResourceUsage, resourceLimits),
                FormatResources(pendingResourceUsage));
        }

        return resourceUsageOverdraftOccurred;
    }

    void ReleasePorts(const TLogger& Logger, const std::vector<int>& ports)
    {
        auto guard = WriterGuard(ResourcesLock_);

        DoReleasePorts(Logger, ports);
    }

    TResourceAcquiringContext GetResourceAcquiringContext() override
    {
        return TResourceAcquiringContext{this};
    }

    int GetPendingResourceHolderCount() final
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        auto guard = ReaderGuard(ResourcesLock_);
        return PendingResourceHolderCount_;
    }

    void RegisterResourcesConsumer(TClosure onResourcesReleased, EResourcesConsumerType consumerType) override
    {
        ResourcesConsumerCallbacks_[consumerType].Subscribe(std::move(onResourcesReleased));
    }

    IYPathServicePtr GetOrchidService() const override
    {
        VERIFY_THREAD_AFFINITY_ANY();

        return IYPathService::FromProducer(BIND_NO_PROPAGATE(
            &TJobResourceManager::TImpl::BuildOrchid,
            MakeStrong(this)));
    }

    void RegisterResourceHolder(const TLogger& Logger, const TResourceHolder* resourceHolder)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        OnResourceHolderRegistered(Logger, resourceHolder);

        EmplaceOrCrash(ResourceHolders_, resourceHolder);
    }

    void UnregisterResourceHolder(TResourceHolder* resourceHolder)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        EraseOrCrash(ResourceHolders_, resourceHolder);
    }

private:
    IBootstrapBase* const Bootstrap_;

    const TJobResourceManagerConfigPtr StaticConfig_;
    TAtomicIntrusivePtr<TJobResourceManagerDynamicConfig> DynamicConfig_;

    TAtomicObject<TNodeResourceLimitsOverrides> ResourceLimitsOverrides_;

    const INodeMemoryTrackerPtr NodeMemoryUsageTracker_;
    const IMemoryUsageTrackerPtr SystemMemoryUsageTracker_;
    const IMemoryUsageTrackerPtr UserMemoryUsageTracker_;
    THashSet<int> FreePorts_;

    TEnumIndexedArray<EResourcesConsumerType, TCallbackList<void()>> ResourcesConsumerCallbacks_;

    TProfiler Profiler_;
    TPeriodicExecutorPtr ProfilingExecutor_;
    TBufferedProducerPtr ResourceLimitsBuffer_ = New<TBufferedProducer>();
    TBufferedProducerPtr ResourceUsageBuffer_ = New<TBufferedProducer>();
    TBufferedProducerPtr PendingResourceUsageBuffer_ = New<TBufferedProducer>();

    TGauge MajorPageFaultsGauge_;
    TGauge FreeMemoryWatermarkMultiplierGauge_;
    TGauge FreeMemoryWatermarkAddedMemoryGauge_;
    TGauge FreeMemoryWatermarkIsIncreasedGauge_;

    YT_DECLARE_SPIN_LOCK(NThreading::TReaderWriterSpinLock, ResourcesLock_);

    TJobResources ResourceUsage_ = ZeroJobResources();
    TJobResources PendingResourceUsage_ = ZeroJobResources();
    int PendingResourceHolderCount_ = 0;

    TPeriodicExecutorPtr ReservedMappedMemoryChecker_;
    TPeriodicExecutorPtr MemoryPressureDetector_;

    i64 LastMajorPageFaultCount_ = 0;
    double FreeMemoryWatermarkMultiplier_ = 1.0;

    bool ShouldNotifyResourcesUpdated_ = false;

    bool HasActiveResourceAcquiring_ = false;

    struct TJobResourceManagerInfo
    {
        NClusterNode::TJobResources ResourceLimits;
        NClusterNode::TJobResources ResourceUsage;

        NClusterNode::TJobResources PendingResourceUsage;

        int PendingResourceHolderCount;

        i64 LastMajorPageFaultCount;

        double FreeMemoryWatermarkMultiplier;

        double CpuToVCpuFactor;

        std::vector<int> FreePorts;
    };

    THashSet<const TResourceHolder*> ResourceHolders_;

    DECLARE_THREAD_AFFINITY_SLOT(JobThread);

    TJobResourceManagerInfo BuildResourceManagerInfo() const
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        TJobResources pendingResourceUsage;
        int pendingResourceHolderCount;

        std::vector<int> ports;

        {
            auto guard = ReaderGuard(ResourcesLock_);

            pendingResourceUsage = PendingResourceUsage_;
            pendingResourceHolderCount = PendingResourceHolderCount_;

            ports = GetFreePorts();
        }

        return {
            .ResourceLimits = GetResourceLimits(),
            .ResourceUsage = GetResourceUsage(),
            .PendingResourceUsage = pendingResourceUsage,
            .PendingResourceHolderCount = pendingResourceHolderCount,
            .LastMajorPageFaultCount = LastMajorPageFaultCount_,
            .FreeMemoryWatermarkMultiplier = FreeMemoryWatermarkMultiplier_,
            .CpuToVCpuFactor = GetCpuToVCpuFactor(),
            .FreePorts = std::move(ports),
        };
    }

    auto BuildResourceHoldersInfo() const
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        std::vector<TResourceHolder::TResourceHolderInfo> result;
        result.reserve(std::size(ResourceHolders_));

        for (const auto& resourceHolder : ResourceHolders_) {
            result.push_back(resourceHolder->BuildResourceHolderInfo());
        }

        return result;
    }

    auto DoGetStateSnapshot() const
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        return std::tuple(
            BuildResourceManagerInfo(),
            BuildResourceHoldersInfo());
    }

    auto GetStateSnapshot() const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto infoOrError = WaitFor(BIND(
            &TJobResourceManager::TImpl::DoGetStateSnapshot,
            MakeStrong(this))
            .AsyncVia(Bootstrap_->GetJobInvoker())
            .Run());

        YT_LOG_FATAL_UNLESS(
            infoOrError.IsOK(),
            infoOrError,
            "Unexpected failure while making job resource manager info snapshot");

        return std::move(infoOrError.Value());
    }

    void BuildOrchid(IYsonConsumer* consumer) const
    {
        VERIFY_THREAD_AFFINITY_ANY();

        auto [
            jobResourceManagerInfo,
            resourceHoldersInfo
        ] = GetStateSnapshot();

        BuildYsonFluently(consumer).BeginMap()
            .Item("resource_limits").Value(jobResourceManagerInfo.ResourceLimits)
            .Item("resource_usage").Value(ToNodeResources(jobResourceManagerInfo.ResourceUsage))
            .Item("pending_resources").Value(jobResourceManagerInfo.PendingResourceUsage)
            .Item("pending_resource_holder_count").Value(jobResourceManagerInfo.PendingResourceHolderCount)
            .Item("last_major_page_fault_count").Value(jobResourceManagerInfo.LastMajorPageFaultCount)
            .Item("free_memory_multiplier").Value(jobResourceManagerInfo.FreeMemoryWatermarkMultiplier)
            .Item("cpu_to_vcpu_factor").Value(jobResourceManagerInfo.CpuToVCpuFactor)
            .Item("free_ports").Value(jobResourceManagerInfo.FreePorts)
            .Item("resource_holders").DoMapFor(
                resourceHoldersInfo,
                [] (auto fluent, const auto& resourceHolderInfo) {
                    fluent.Item(ToString(resourceHolderInfo.Id)).BeginMap()
                        .Item("resources_counsumer_type").Value(resourceHolderInfo.ResourcesConsumerType)
                        .Item("base_resource_usage").Value(resourceHolderInfo.BaseResourceUsage)
                        .Item("additional_resource_usage").Value(resourceHolderInfo.AdditionalResourceUsage)
                    .EndMap();
                })
        .EndMap();
    }

    void DoReleasePorts(const TLogger& Logger, const std::vector<int>& ports)
    {
        VERIFY_WRITER_SPINLOCK_AFFINITY(ResourcesLock_);

        YT_LOG_INFO_UNLESS(
            ports.empty(),
            "Releasing ports (PortCount: %v, Ports: %v)",
            ports.size(),
            ports);
        for (auto port : ports) {
            InsertOrCrash(FreePorts_, port);
        }
    }

    std::vector<int> GetFreePorts() const
    {
        VERIFY_SPINLOCK_AFFINITY(ResourcesLock_);

        return std::vector<int>(begin(FreePorts_), end(FreePorts_));
    }

    void NotifyResourcesReleased(EResourcesConsumerType resourcesConsumerType, bool fullyReleased)
    {
        ResourcesReleased_.Fire(resourcesConsumerType, fullyReleased);
        for (const auto& callbacks : ResourcesConsumerCallbacks_) {
            callbacks.Fire();
        }
    }

    void CheckReservedMappedMemory()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        YT_LOG_INFO("Check mapped memory usage");

        THashMap<TString, i64> vmstat;
        try {
            vmstat = GetVmstat();
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Failed to read /proc/vmstat; skipping mapped memory check");
            return;
        }

        auto mappedIt = vmstat.find("nr_mapped");
        if (mappedIt == vmstat.end()) {
            YT_LOG_WARNING("Field \"nr_mapped\" is not found in /proc/vmstat; skipping mapped memory check");
            return;
        }

        i64 mappedMemory = mappedIt->second;
        auto dynamicConfig = GetDynamicConfig();

        YT_LOG_INFO(
            "Mapped memory usage (Usage: %v, Reserved: %v)",
            mappedMemory,
            dynamicConfig->MappedMemoryController->ReservedMemory);

        if (mappedMemory <= dynamicConfig->MappedMemoryController->ReservedMemory) {
            return;
        }

        ReservedMemoryOvercommited_.Fire(mappedMemory);
    }

    void CheckMemoryPressure()
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        try {
            auto currentFaultCount = Bootstrap_->GetExecNodeBootstrap()->GetSlotManager()->GetMajorPageFaultCount();
            if (currentFaultCount != LastMajorPageFaultCount_) {
                HandleMajorPageFaultsRateIncrease(currentFaultCount);
            }
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(
                ex,
                "Error getting information about major page faults");
        }
    }

    void HandleMajorPageFaultsRateIncrease(i64 currentFaultCount)
    {
        auto config = DynamicConfig_.Acquire()->MemoryPressureDetector;
        YT_LOG_DEBUG(
            "Increased rate of major page faults in node container detected (MajorPageFaultCount: %v -> %v, Delta: %v, Threshold: %v, Period: %v)",
            LastMajorPageFaultCount_,
            currentFaultCount,
            currentFaultCount - LastMajorPageFaultCount_,
            config->MajorPageFaultCountThreshold,
            config->CheckPeriod);

        if (config->Enabled &&
            currentFaultCount - LastMajorPageFaultCount_ > config->MajorPageFaultCountThreshold)
        {
            auto previousMemoryWatermarkMultiplier = FreeMemoryWatermarkMultiplier_;
            FreeMemoryWatermarkMultiplier_ = std::min(
                FreeMemoryWatermarkMultiplier_ + config->MemoryWatermarkMultiplierIncreaseStep,
                config->MaxMemoryWatermarkMultiplier);

            YT_LOG_DEBUG(
                "Increasing memory watermark multiplier "
                "(MemoryWatermarkMultiplier: %v -> %v, "
                "UpdatedFreeMemoryWatermark: %v, "
                "UserMemoryUsageTrackerLimit: %v, "
                "UserMemoryUsageTrackerUsed: %v, "
                "NodeMemoryUsageTrackerTotalFree: %v)",
                previousMemoryWatermarkMultiplier,
                FreeMemoryWatermarkMultiplier_,
                GetFreeMemoryWatermark(),
                UserMemoryUsageTracker_->GetLimit(),
                UserMemoryUsageTracker_->GetUsed(),
                NodeMemoryUsageTracker_->GetTotalFree());
        }

        LastMajorPageFaultCount_ = currentFaultCount;
    }

    //! Returns |true| if a acquiring with given #neededResources can succeed.
    //! Takes special care with ReplicationDataSize and RepairDataSize enabling
    //! an arbitrary large overdraft for the
    //! first acquiring.
    bool HasEnoughResources(
        const TJobResources& neededResources,
        const TJobResources& usedResources,
        const TJobResources& totalResources)
    {
        VERIFY_THREAD_AFFINITY(JobThread);

        auto spareResources = CalculateSpareResources(totalResources, usedResources);
        // Allow replication/repair/merge data size overcommit.
        spareResources.ReplicationDataSize = InfiniteJobResources().ReplicationDataSize;
        spareResources.RepairDataSize = InfiniteJobResources().RepairDataSize;
        spareResources.MergeDataSize = InfiniteJobResources().MergeDataSize;

        // JRM doesn't track disk resources
        // TODO(pogorelov): Add disk resources support
        spareResources.DiskSpaceRequest = InfiniteJobResources().DiskSpaceRequest;
        return Dominates(spareResources, neededResources);
    }

    friend class TResourceHolder;
};

////////////////////////////////////////////////////////////////////////////////

TJobResourceManagerPtr TJobResourceManager::CreateJobResourceManager(IBootstrapBase* bootstrap)
{
    return New<TJobResourceManager::TImpl>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

class TJobMemoryUsageTracker
    : public IMemoryUsageTracker
{
public:
    TJobMemoryUsageTracker(
        TResourceHolderPtr resourceHolder,
        EMemoryCategory memotyCategory)
        : ResourceHolder_(std::move(resourceHolder))
        , MemoryCategory_(memotyCategory)
    {
        YT_VERIFY(MemoryCategory_ == EMemoryCategory::SystemJobs || MemoryCategory_ == EMemoryCategory::UserJobs);
    }

    bool Acquire(i64 size) override
    {
        TJobResources resources;
        GetMemory(resources) = size;
        return ResourceHolder_->UpdateAdditionalResourceUsage(resources);
    }

    TError TryAcquire(i64 /*size*/) override
    {
        YT_UNIMPLEMENTED();
    }

    TError TryChange(i64 /*size*/) override
    {
        YT_UNIMPLEMENTED();
    }

    void Release(i64 size) override
    {
        TJobResources resources;
        GetMemory(resources) = -size;
        ResourceHolder_->UpdateAdditionalResourceUsage(resources);
    }

    i64 GetFree() const override
    {
        return GetMemory(ResourceHolder_->GetFreeResources());
    }

    void SetLimit(i64 /*size*/) override
    {
        YT_UNIMPLEMENTED();
    }

    i64 GetLimit() const override
    {
        auto resource = ResourceHolder_->GetResourceLimits();
        return GetMemory(resource) ? GetMemory(resource) : std::numeric_limits<i64>::max();
    }

    i64 GetUsed() const override
    {
        auto resource = ResourceHolder_->GetResourceUsage();
        return GetMemory(resource);
    }

    bool IsExceeded() const override
    {
        return GetFree() <= 0;
    }

    TSharedRef Track(TSharedRef reference, bool /*keepExistingTracking*/) override
    {
        // TODO(pogorelov): Support shared ref tracking.
        return reference;
    }

private:
    const TResourceHolderPtr ResourceHolder_;

    const EMemoryCategory MemoryCategory_;

    auto GetJobResourcesMemberReference() const
    {
        switch (MemoryCategory_) {
            case EMemoryCategory::SystemJobs:
                return &TJobResources::SystemMemory;
            case EMemoryCategory::UserJobs:
                return &TJobResources::UserMemory;
            default:
                YT_ABORT();
        }
    }

    i64& GetMemory(TJobResources& resources) const
    {
        return resources.*GetJobResourcesMemberReference();
    }

    i64 GetMemory(const TJobResources& resources) const
    {
        return resources.*GetJobResourcesMemberReference();
    }
};

////////////////////////////////////////////////////////////////////////////////

TJobResourceManager::TResourceAcquiringContext::TResourceAcquiringContext(
    TJobResourceManager* resourceManager)
    : ResourceManagerImpl_(static_cast<TJobResourceManager::TImpl*>(resourceManager))
{
    ResourceManagerImpl_->OnResourceAcquiringStarted();
}

TJobResourceManager::TResourceAcquiringContext::~TResourceAcquiringContext()
{
    ResourceManagerImpl_->OnResourceAcquiringFinished();
}

bool TJobResourceManager::TResourceAcquiringContext::TryAcquireResourcesFor(const TResourceHolderPtr& resourceHolder) &
{
    try {
        if (!ResourceManagerImpl_->AcquireResourcesFor(resourceHolder)) {
            return false;
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION(ex);
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////

TResourceOwner::TResourceOwner(
    TGuid holderId,
    TJobResourceManager* jobResourceManager,
    EResourcesConsumerType resourceConsumerType,
    const NClusterNode::TJobResources& jobResources)
    : ResourceHolder_(TResourceHolder::CreateResourceHolder(
        holderId,
        jobResourceManager,
        resourceConsumerType,
        jobResources))
{
    ResourceHolder_->ResetOwner(MakeStrong(this));
}

void TResourceOwner::ReleaseResources()
{
    ResourceHolder_->ResetOwner({});
    ResourceHolder_.Reset();
}

TResourceHolderPtr TResourceHolder::CreateResourceHolder(
    TGuid id,
    TJobResourceManager* jobResourceManager,
    EResourcesConsumerType resourceConsumerType,
    const NClusterNode::TJobResources& jobResources)
{
    return NewWithOffloadedDtor<TResourceHolder>(
        static_cast<TJobResourceManager::TImpl*>(jobResourceManager)->Bootstrap_->GetJobInvoker(),
        id,
        jobResourceManager,
        resourceConsumerType,
        jobResources);
}

TResourceHolder::TResourceHolder(
    TGuid id,
    TJobResourceManager* jobResourceManager,
    EResourcesConsumerType resourceConsumerType,
    const TJobResources& resources)
    : ResourcesConsumerType(resourceConsumerType)
    , Id_(id)
    , Logger(NJobAgent::Logger.WithTag("ResourceHolderId: %v", id))
    , ResourceManagerImpl_(static_cast<TJobResourceManager::TImpl*>(jobResourceManager))
    , BaseResourceUsage_(resources)
    , AdditionalResourceUsage_(ZeroJobResources())
{
    Register();
}

TResourceHolder::~TResourceHolder()
{
    if (State_ != EResourcesState::Released) {
        YT_LOG_DEBUG(
            "Destroying unreleased resource holder (State: %v, Resources: %v)",
            State_,
            FormatResources(GetResourceUsage()));

        ReleaseBaseResources();
    }

    ReleaseAdditionalResources();

    Unregister();
}

TGuid TResourceHolder::GetId() const noexcept
{
    return Id_;
}

void TResourceHolder::Register()
{
    ResourceManagerImpl_->RegisterResourceHolder(Logger, this);
}

void TResourceHolder::Unregister()
{
    ResourceManagerImpl_->UnregisterResourceHolder(this);
}

void TResourceHolder::SetAcquiredResources(TAcquiredResources&& acquiredResources)
{
    auto guard = WriterGuard(ResourcesLock_);

    YT_VERIFY(State_ == EResourcesState::Pending);

    Ports_ = std::move(acquiredResources.Ports);

    YT_VERIFY(PortCount_ == std::ssize(Ports_));

    acquiredResources.SystemMemoryGuard.ReleaseNoReclaim();
    acquiredResources.UserMemoryGuard.ReleaseNoReclaim();

    UserSlot_ = std::move(acquiredResources.UserSlot);
    GpuSlots_ = std::move(acquiredResources.GpuSlots);

    State_ = EResourcesState::Acquired;
}

void TResourceHolder::ReleaseAdditionalResources()
{
    auto guard = WriterGuard(ResourcesLock_);

    DoSetResourceUsage(
        -AdditionalResourceUsage_,
        "AdditionalResourceUsageDelta",
        [&] (const TJobResources& resourceUsageDelta) {
            AdditionalResourceUsage_ += resourceUsageDelta;

            return resourceUsageDelta;
        });
}

void TResourceHolder::ReleaseNonSlotResources()
{
    auto usedSlotResources = ZeroJobResources();
    auto resources = GetResourceUsage();
    usedSlotResources.UserSlots = resources.UserSlots;
    usedSlotResources.Gpu = resources.Gpu;

    auto guard = WriterGuard(ResourcesLock_);

    DoSetResourceUsage(
        usedSlotResources,
        "NewResourceUsage",
        [&] (const TJobResources& newResourceUsage) {
            auto resourcesDelta = newResourceUsage - CumulativeResourceUsage();

            BaseResourceUsage_ = newResourceUsage;

            return resourcesDelta;
        });
}

void TResourceHolder::ReleaseBaseResources()
{
    TJobResources resources;
    {
        auto guard = ReaderGuard(ResourcesLock_);

        YT_VERIFY(State_ != EResourcesState::Released);

        resources = BaseResourceUsage_;
    }

    YT_LOG_FATAL_IF(
        UserSlot_ && resources.UserSlots != 1,
        "User slot not matched with UserSlots (UserSlotExist: %v, UserSlots: %v)",
        UserSlot_ != nullptr,
        resources.UserSlots);

    YT_LOG_FATAL_IF(
        std::ssize(GpuSlots_) > resources.Gpu,
        "GPU slots not matched with Gpu");

    YT_LOG_INFO("Reset resource holder slots");

    auto guard = WriterGuard(ResourcesLock_);

    if (UserSlot_) {
        UserSlot_->ResetState();
    }

    UserSlot_.Reset();
    GpuSlots_.clear();

    ResourceManagerImpl_->OnBaseResourcesReleased(
        ResourcesConsumerType,
        Logger,
        BaseResourceUsage_,
        Ports_,
        /*resourceHolderStarted*/ State_ == EResourcesState::Acquired);
    State_ = EResourcesState::Released;

    BaseResourceUsage_ = ZeroJobResources();
}

const std::vector<int>& TResourceHolder::GetPorts() const noexcept
{
    auto guard = ReaderGuard(ResourcesLock_);

    return Ports_;
}

const ISlotPtr& TResourceHolder::GetUserSlot() const noexcept
{
    auto guard = ReaderGuard(ResourcesLock_);

    return UserSlot_;
}

const std::vector<ISlotPtr>& TResourceHolder::GetGpuSlots() const noexcept
{
    auto guard = ReaderGuard(ResourcesLock_);

    return GpuSlots_;
}

bool TResourceHolder::SetBaseResourceUsage(TJobResources newResourceUsage)
{
    auto guard = WriterGuard(ResourcesLock_);

    YT_LOG_FATAL_IF(
        State_ != EResourcesState::Acquired,
        "Resource holder is not acquired");

    return DoSetResourceUsage(
        newResourceUsage,
        "NewResourceUsage",
        [&] (const TJobResources& newResourceUsage) {
            auto resourceDelta = newResourceUsage - BaseResourceUsage_;
            BaseResourceUsage_ = newResourceUsage;

            return resourceDelta;
        });
}

bool TResourceHolder::UpdateAdditionalResourceUsage(TJobResources additionalResourceUsageDelta)
{
    auto guard = WriterGuard(ResourcesLock_);

    return DoSetResourceUsage(
        additionalResourceUsageDelta,
        "AdditionalResourceUsageDelta",
        [&] (const TJobResources& resourceUsageDelta) {
            AdditionalResourceUsage_ += resourceUsageDelta;

            return resourceUsageDelta;
        });
}

IMemoryUsageTrackerPtr TResourceHolder::GetAdditionalMemoryUsageTracker(EMemoryCategory memoryCategory)
{
    return New<TJobMemoryUsageTracker>(MakeStrong(this), memoryCategory);
}

TJobResources TResourceHolder::GetResourceLimits() const noexcept
{
    return ResourceManagerImpl_->GetResourceLimits();
}

TJobResources TResourceHolder::GetFreeResources() const noexcept
{
    return ResourceManagerImpl_->GetFreeResources();
}

void TResourceHolder::UpdateResourceDemand(
    const NClusterNode::TJobResources& resources,
    const NClusterNode::TJobResourceAttributes& resourceAttributes,
    int portCount)
{
    auto guard = WriterGuard(ResourcesLock_);

    YT_VERIFY(State_ == EResourcesState::Pending);
    YT_VERIFY(AdditionalResourceUsage_ == ZeroJobResources());

    YT_LOG_DEBUG(
        "Resource demand updated (NewRecourceDemand: %v, NewPortCount: %v)",
        FormatResources(resources),
        portCount);

    BaseResourceUsage_ = resources;
    ResourceAttributes_ = resourceAttributes;
    PortCount_ = portCount;
}

TJobResources TResourceHolder::GetResourceUsage() const noexcept
{
    auto guard = ReaderGuard(ResourcesLock_);

    return CumulativeResourceUsage();
}

std::pair<TJobResources, TJobResources> TResourceHolder::GetDetailedResourceUsage() const noexcept
{
    auto guard = ReaderGuard(ResourcesLock_);

    return std::pair(BaseResourceUsage_, AdditionalResourceUsage_);
}

const TJobResourceAttributes& TResourceHolder::GetResourceAttributes() const noexcept
{
    auto guard = ReaderGuard(ResourcesLock_);

    return ResourceAttributes_;
}

const NLogging::TLogger& TResourceHolder::GetLogger() const noexcept
{
    return Logger;
}

TResourceOwnerPtr TResourceHolder::GetOwner() const noexcept
{
    auto guard = ReaderGuard(ResourcesLock_);

    return Owner_.Lock();
}

void TResourceHolder::ResetOwner(const TResourceOwnerPtr& owner)
{
    auto guard = ReaderGuard(ResourcesLock_);

    Owner_ = owner;
}

TJobResources TResourceHolder::CumulativeResourceUsage() const noexcept
{
    VERIFY_SPINLOCK_AFFINITY(ResourcesLock_);

    return BaseResourceUsage_ + AdditionalResourceUsage_;
}

TResourceHolder::TResourceHolderInfo TResourceHolder::BuildResourceHolderInfo() const noexcept
{
    VERIFY_THREAD_AFFINITY_ANY();

    auto [
        baseResourceUsage,
        additionalResourceUsage
    ] = GetDetailedResourceUsage();

    return {
        .Id = Id_,
        .BaseResourceUsage = baseResourceUsage,
        .AdditionalResourceUsage = additionalResourceUsage,
        .ResourcesConsumerType = ResourcesConsumerType,
    };
}

template <class TResourceUsageUpdater>
    requires std::is_invocable_r_v<
        TJobResources,
        TResourceUsageUpdater,
        const TJobResources&>
bool TResourceHolder::DoSetResourceUsage(
    const NClusterNode::TJobResources& newResourceUsage,
    TStringBuf argumentName,
    TResourceUsageUpdater resourceUsageUpdater)
{
    VERIFY_WRITER_SPINLOCK_AFFINITY(ResourcesLock_);

    YT_LOG_DEBUG(
        "Setting resources to holder (CurrentState: %v, %v: %v)",
        State_,
        argumentName,
        FormatResources(newResourceUsage));

    auto resourceUsageDelta = std::invoke(resourceUsageUpdater, newResourceUsage);

    auto overdraftOccurred = ResourceManagerImpl_->OnResourcesUpdated(
        this,
        ResourcesConsumerType,
        GetLogger(),
        resourceUsageDelta);

    return !overdraftOccurred;
}

////////////////////////////////////////////////////////////////////////////////

TResourceHolder::TAcquiredResources::TAcquiredResources(
    TJobResourceManager::TImpl* jobResourceManagerImpl,
    TMemoryUsageTrackerGuard&& userMemoryGuard,
    TMemoryUsageTrackerGuard&& systemMemoryGuard,
    ISlotPtr&& userSlot,
    std::vector<ISlotPtr>&& gpuSlots,
    std::vector<int> ports) noexcept
    : UserMemoryGuard(std::move(userMemoryGuard))
    , SystemMemoryGuard(std::move(systemMemoryGuard))
    , UserSlot(std::move(userSlot))
    , GpuSlots(std::move(gpuSlots))
    , Ports(std::move(ports))
    , JobResourceManagerImpl_(jobResourceManagerImpl)
{ }

TResourceHolder::TAcquiredResources::~TAcquiredResources()
{
    if (!std::empty(Ports)) {
        JobResourceManagerImpl_->ReleasePorts(NJobAgent::Logger, Ports);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent
