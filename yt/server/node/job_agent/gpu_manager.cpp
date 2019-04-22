#include "gpu_manager.h"
#include "private.h"

#include <yt/server/node/cell_node/bootstrap.h>
#include <yt/server/node/data_node/master_connector.h>

#include <yt/server/lib/job_agent/gpu_helpers.h>

#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/misc/finally.h>
#include <yt/core/misc/proc.h>
#include <yt/core/misc/subprocess.h>

#include <util/folder/iterator.h>

#include <util/string/strip.h>

namespace NYT::NJobAgent {

using namespace NConcurrency;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = JobAgentServerLogger;

////////////////////////////////////////////////////////////////////////////////

TGpuSlot::TGpuSlot(int deviceNumber)
    : DeviceNumber_(deviceNumber)
{ }

TString TGpuSlot::GetDeviceName() const
{
    return NJobAgent::GetGpuDeviceName(DeviceNumber_);
}

int TGpuSlot::GetDeviceNumber() const
{
    return DeviceNumber_;
}

////////////////////////////////////////////////////////////////////////////////

TGpuManager::TGpuManager(TBootstrap* bootstrap, TGpuManagerConfigPtr config)
    : Bootstrap_(bootstrap)
    , Config_(std::move(config))
{
    auto descriptors = NJobAgent::ListGpuDevices();
    if (descriptors.empty()) {
        return;
    }

    for (const auto& descriptor : descriptors) {
        GpuDevices_.push_back(descriptor.DeviceName);
        FreeSlots_.emplace_back(descriptor.DeviceNumber);
        YCHECK(HealthyGpuDeviceNumbers_.insert(descriptor.DeviceNumber).second);
    }

    HealthCheckExecutor_ = New<TPeriodicExecutor>(
        Bootstrap_->GetControlInvoker(),
        BIND(&TGpuManager::OnHealthCheck, MakeWeak(this)),
        Config_->HealthCheckPeriod);
    HealthCheckExecutor_->Start();
}

void TGpuManager::OnHealthCheck()
{
    try {
        auto healthyGpuDeviceNumbers = GetHealthyGpuDeviceNumbers(Config_->HealthCheckTimeout);
        YT_LOG_DEBUG("Found healthy GPU devices (DeviceNumbers: %v)",
            healthyGpuDeviceNumbers);

        std::vector<TError> newAlerts;

        {
            TGuard<TSpinLock> guard(SpinLock_);
            HealthyGpuDeviceNumbers_ = std::move(healthyGpuDeviceNumbers);

            std::vector<TGpuSlot> healthySlots;
            for (auto& slot: FreeSlots_) {
                if (HealthyGpuDeviceNumbers_.find(slot.GetDeviceNumber()) == HealthyGpuDeviceNumbers_.end()) {
                    YT_LOG_WARNING("Found lost GPU device (DeviceName: %v)",
                        slot.GetDeviceName());
                    newAlerts.push_back(TError("Found lost GPU device %v",
                        slot.GetDeviceName()));
                } else {
                    healthySlots.emplace_back(std::move(slot));
                }
            }

            FreeSlots_ = std::move(healthySlots);
        }

        for (const auto& alert: newAlerts) {
            Bootstrap_->GetMasterConnector()->RegisterAlert(alert);
        }

    } catch (const std::exception& ex) {
        YT_LOG_WARNING(ex, "Failed to get healthy GPU devices");
        Bootstrap_->GetMasterConnector()->RegisterAlert(TError("All GPU devices are disabled")
            << ex);
        HealthCheckExecutor_->Stop();

        TGuard<TSpinLock> guard(SpinLock_);
        Disabled_ = true;
    }
}

int TGpuManager::GetTotalGpuCount() const
{
    auto guard = Guard(SpinLock_);
    return Disabled_ ? 0 : HealthyGpuDeviceNumbers_.size();
}

int TGpuManager::GetFreeGpuCount() const
{
    auto guard = Guard(SpinLock_);
    return Disabled_ ? 0 : FreeSlots_.size();
}

const std::vector<TString>& TGpuManager::ListGpuDevices() const
{
    return GpuDevices_;
}

TGpuManager::TGpuSlotPtr TGpuManager::AcquireGpuSlot()
{
    YCHECK(!FreeSlots_.empty());

    auto deleter = [this, this_ = MakeStrong(this)] (TGpuSlot* slot) {
        YT_LOG_DEBUG("Released GPU slot (DeviceName: %v)",
            slot->GetDeviceName());

        auto guard = Guard(this_->SpinLock_);
        if (HealthyGpuDeviceNumbers_.find(slot->GetDeviceNumber()) == HealthyGpuDeviceNumbers_.end()) {
            guard.Release();
            YT_LOG_WARNING("Found lost GPU device (DeviceName: %v)",
                slot->GetDeviceName());
            Bootstrap_->GetMasterConnector()->RegisterAlert(TError("Found lost GPU device %v",
                slot->GetDeviceName()));
        } else {
            this_->FreeSlots_.emplace_back(std::move(*slot));
        }

        delete slot;
    };

    auto guard = Guard(SpinLock_);
    TGpuSlotPtr slot(new TGpuSlot(std::move(FreeSlots_.back())), deleter);
    FreeSlots_.pop_back();

    YT_LOG_DEBUG("Acquired GPU slot (DeviceName: %v)",
        slot->GetDeviceName());
    return slot;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent
