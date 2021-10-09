#pragma once

#include "table_mount_cache.h"

#include <yt/yt/core/concurrency/spinlock.h>

#include <yt/yt/core/misc/async_expiring_cache.h>

#include <yt/yt/core/logging/log.h>

#include <yt/yt/core/profiling/public.h>

namespace NYT::NTabletClient {

////////////////////////////////////////////////////////////////////////////////

class TTabletInfoCache
{
public:
    TTabletInfoPtr Find(TTabletId tabletId);
    TTabletInfoPtr Insert(const TTabletInfoPtr& tabletInfo);
    void Clear();

private:
    void SweepExpiredEntries();

    std::atomic<NProfiling::TCpuInstant> ExpiredEntriesSweepDeadline_ = 0;

    YT_DECLARE_SPINLOCK(NConcurrency::TReaderWriterSpinLock, MapLock_);
    THashMap<TTabletId, TWeakPtr<TTabletInfo>> Map_;
};

///////////////////////////////////////////////////////////////////////////////

struct TTableMountCacheKey
{
    NYPath::TYPath Path;
    NHydra::TRevision RefreshPrimaryRevision;
    NHydra::TRevision RefreshSecondaryRevision;

    TTableMountCacheKey(
        const NYPath::TYPath& path,
        NHydra::TRevision refreshPrimaryRevision = NHydra::NullRevision,
        NHydra::TRevision refreshSecondaryRevision = NHydra::NullRevision);

    operator size_t() const;
    bool operator == (const TTableMountCacheKey& other) const;
};

void FormatValue(TStringBuilderBase* builder, const TTableMountCacheKey& key, TStringBuf /*spec*/);
TString ToString(const TTableMountCacheKey& key);

///////////////////////////////////////////////////////////////////////////////

class TTableMountCacheBase
    : public ITableMountCache
    , public TAsyncExpiringCache<TTableMountCacheKey, TTableMountInfoPtr>
{
public:
    TTableMountCacheBase(
        TTableMountCacheConfigPtr config,
        NLogging::TLogger logger);

    TFuture<TTableMountInfoPtr> GetTableInfo(const NYPath::TYPath& path) override;
    TTabletInfoPtr FindTabletInfo(TTabletId tabletId) override;
    void InvalidateTablet(TTabletInfoPtr tabletInfo) override;
    std::pair<bool, TTabletInfoPtr> InvalidateOnError(const TError& error, bool forceRetry) override;
    void Clear() override;

protected:
    const TTableMountCacheConfigPtr Config_;
    const NLogging::TLogger Logger;

    TTabletInfoCache TabletInfoCache_;

    virtual void InvalidateTable(const TTableMountInfoPtr& tableInfo) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletClient
