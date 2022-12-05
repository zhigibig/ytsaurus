#pragma once

#include <yt/yt/server/lib/containers/instance.h>
#include <yt/yt/server/lib/containers/public.h>

#include <yt/yt/ytlib/cgroup/cgroup.h>

#include <yt/yt/core/misc/singleton.h>
#include <yt/yt/core/net/address.h>
#include <yt/yt/core/ytree/public.h>

#include <yt/yt/library/process/process.h>
#include <yt/yt/library/profiling/producer.h>

namespace NYT::NContainers {

using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

static constexpr auto ResourceUsageUpdatePeriod = TDuration::MilliSeconds(1000);

////////////////////////////////////////////////////////////////////////////////

using TCpuStatistics = NCGroup::TCpuAccounting::TStatistics;
using TBlockIOStatistics = NCGroup::TBlockIO::TStatistics;
using TMemoryStatistics = NCGroup::TMemory::TStatistics;
using TNetworkStatistics = NCGroup::TNetwork::TStatistics;

struct TTotalStatistics
{
public:
    TCpuStatistics CpuStatistics;
    TMemoryStatistics MemoryStatistics;
    TBlockIOStatistics BlockIOStatistics;
    TNetworkStatistics NetworkStatistics;
};

#ifdef _linux_

////////////////////////////////////////////////////////////////////////////////

class TPortoResourceTracker
    : public TRefCounted
{
public:
    TPortoResourceTracker(
        IInstancePtr instance,
        TDuration updatePeriod,
        bool isDeltaTracker = false);

    TCpuStatistics GetCpuStatistics() const;

    TBlockIOStatistics GetBlockIOStatistics() const;

    TMemoryStatistics GetMemoryStatistics() const;

    TNetworkStatistics GetNetworkStatistics() const;

    TTotalStatistics GetTotalStatistics() const;

    bool AreResourceUsageStatisticsExpired() const;

    TInstant GetLastUpdateTime() const;

private:
    const IInstancePtr Instance_;
    const TDuration UpdatePeriod_;
    const bool IsDeltaTracker_;

    mutable std::atomic<TInstant> LastUpdateTime_ = {};

    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, SpinLock_);
    mutable TResourceUsage ResourceUsage_;
    mutable TResourceUsage ResourceUsageDelta_;

    mutable std::optional<TCpuStatistics> CachedCpuStatistics_;
    mutable std::optional<TMemoryStatistics> CachedMemoryStatistics_;
    mutable std::optional<TBlockIOStatistics> CachedBlockIOStatistics_;
    mutable std::optional<TNetworkStatistics> CachedNetworkStatistics_;
    mutable std::optional<TTotalStatistics> CachedTotalStatistics_;
    mutable TErrorOr<ui64> PeakThreadCount_ = 0;

    template <class T, class F>
    T GetStatistics(
        std::optional<T>& cachedStatistics,
        const TString& statisticsKind,
        F func) const;

    TCpuStatistics ExtractCpuStatistics(TResourceUsage& resourceUsage) const;
    TMemoryStatistics ExtractMemoryStatistics(TResourceUsage& resourceUsage) const;
    TBlockIOStatistics ExtractBlockIOStatistics(TResourceUsage& resourceUsage) const;
    TNetworkStatistics ExtractNetworkStatistics(TResourceUsage& resourceUsage) const;
    TTotalStatistics ExtractTotalStatistics(TResourceUsage& resourceUsage) const;

    TErrorOr<ui64> CalculateCounterDelta(TErrorOr<ui64>& oldValue, TErrorOr<ui64>& newValue) const;

    TResourceUsage CalculateResourceUsageDelta(
        TResourceUsage& oldResourceUsage,
        TResourceUsage& newResourceUsage) const;

    void UpdateResourceUsageStatisticsIfExpired() const;

    void DoUpdateResourceUsage() const;
};

DECLARE_REFCOUNTED_TYPE(TPortoResourceTracker)
DEFINE_REFCOUNTED_TYPE(TPortoResourceTracker)

////////////////////////////////////////////////////////////////////////////////

class TPortoResourceProfiler
    : public ISensorProducer
{
public:
    TPortoResourceProfiler(
        TPortoResourceTrackerPtr tracker,
        const TProfiler& profiler = TProfiler{"/porto"});

    void CollectSensors(ISensorWriter* writer) override;

private:
    const TPortoResourceTrackerPtr ResourceTracker_;

    void WriteCpuMetrics(
        ISensorWriter* writer,
        TTotalStatistics& totalStatistics,
        i64 timeDeltaUsec);

    void WriteMemoryMetrics(
        ISensorWriter* writer,
        TTotalStatistics& totalStatistics);

    void WriteBlockingIOMetrics(
        ISensorWriter* writer,
        TTotalStatistics& totalStatistics,
        i64 timeDeltaUsec);

    void WriteNetworkMetrics(
        ISensorWriter* writer,
        TTotalStatistics& totalStatistics);
};

DECLARE_REFCOUNTED_TYPE(TPortoResourceProfiler)
DEFINE_REFCOUNTED_TYPE(TPortoResourceProfiler)

////////////////////////////////////////////////////////////////////////////////

#endif

} // namespace NYT::NContainers
