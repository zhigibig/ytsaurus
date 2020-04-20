#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

#include <cmath>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TSlruCacheConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! The maximum number of weight units cached items are allowed to occupy.
    //! Zero means that no items are cached.
    i64 Capacity;

    //! The fraction of total capacity given to the younger segment.
    double YoungerSizeFraction;

    //! Capacity of internal buffer used to amortize and de-contend touch operations.
    int TouchBufferCapacity;

    //! Number of shards.
    int ShardCount;

    explicit TSlruCacheConfig(i64 capacity = 0)
    {
        RegisterParameter("capacity", Capacity)
            .Default(capacity)
            .GreaterThanOrEqual(0);
        RegisterParameter("younger_size_fraction", YoungerSizeFraction)
            .Default(0.25)
            .InRange(0.0, 1.0);
        RegisterParameter("touch_buffer_capacity", TouchBufferCapacity)
            .Default(65536)
            .GreaterThan(0);
        RegisterParameter("shard_count", ShardCount)
            .Default(16)
            .GreaterThan(0);
    }
};

DEFINE_REFCOUNTED_TYPE(TSlruCacheConfig)

////////////////////////////////////////////////////////////////////////////////

//! Cache which removes entries after a while.
/*!
 * TAsyncExpiringCache acts like a proxy between a client and a remote service:
 * requests are sent to the service and responses are saved in the cache as entries.
 * Next time the client makes a request, the response can be taken from the cache
 * unless it is expired.
 *
 * An entry is considered expired if at least one of the following conditions is true:
 * 1) last access was more than ExpireAfterAccessTime ago,
 * 2) last update was more than ExpireAfter*UpdateTime ago.
 *
 * To avoid client awaiting time on subsequent requests and keep the response
 * up to date, the cache updates entries in the background:
 * If request was successful, the cache performs the same request after RefreshTime
 * and updates the entry.
 * If request was unsuccessful, the entry (which contains error response) will be expired
 * after ExpireAfterFailedUpdateTime.
 */
class TAsyncExpiringCacheConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Time since last finished Get() after which an entry is removed.
    TDuration ExpireAfterAccessTime;

    //! Time since last update, if succeeded, after which an entry is removed.
    TDuration ExpireAfterSuccessfulUpdateTime;

    //! Time since last update, if it failed, after which an entry is removed.
    TDuration ExpireAfterFailedUpdateTime;

    //! Time before next (background) update.
    std::optional<TDuration> RefreshTime;

    //! If set to true, cache will invoke DoGetMany once instead of DoGet on every entry during an update.
    bool BatchUpdate;

    TAsyncExpiringCacheConfig()
    {
        RegisterParameter("expire_after_access_time", ExpireAfterAccessTime)
            .Default(TDuration::Seconds(300));
        RegisterParameter("expire_after_successful_update_time", ExpireAfterSuccessfulUpdateTime)
            .Alias("success_expiration_time")
            .Default(TDuration::Seconds(15));
        RegisterParameter("expire_after_failed_update_time", ExpireAfterFailedUpdateTime)
            .Alias("failure_expiration_time")
            .Default(TDuration::Seconds(15));
        RegisterParameter("refresh_time", RefreshTime)
            .Alias("success_probation_time")
            .Default(TDuration::Seconds(10));
        RegisterParameter("batch_update", BatchUpdate)
            .Default(false);

        RegisterPostprocessor([&] () {
            if (RefreshTime && *RefreshTime > ExpireAfterSuccessfulUpdateTime) {
                THROW_ERROR_EXCEPTION("\"refresh_time\" must be less than \"expire_after_successful_update_time\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TAsyncExpiringCacheConfig)

////////////////////////////////////////////////////////////////////////////////

class TLogDigestConfig
    : public NYTree::TYsonSerializable
{
public:
    // We will round each sample x to the range from [(1 - RelativePrecision)*x, (1 + RelativePrecision)*x].
    // This parameter affects the memory usage of the digest, it is proportional to
    // log(UpperBound / LowerBound) / log(1 + RelativePrecision).
    double RelativePrecision;

    // The bounds of the range operated by the class.
    double LowerBound;
    double UpperBound;

    // The value that is returned when there are no samples in the digest.
    std::optional<double> DefaultValue;

    TLogDigestConfig(double lowerBound, double upperBound, double defaultValue)
        : TLogDigestConfig()
    {
        LowerBound = lowerBound;
        UpperBound = upperBound;
        DefaultValue = defaultValue;
    }

    TLogDigestConfig()
    {
        RegisterParameter("relative_precision", RelativePrecision)
            .Default(0.01)
            .GreaterThan(0);

        RegisterParameter("lower_bound", LowerBound)
            .GreaterThan(0);

        RegisterParameter("upper_bound", UpperBound)
            .GreaterThan(0);

        RegisterParameter("default_value", DefaultValue);

        RegisterPostprocessor([&] () {
            // If there are more than 1000 buckets, the implementation of TLogDigest
            // becomes inefficient since it stores information about at least that many buckets.
            const int maxBucketCount = 1000;
            double bucketCount = log(UpperBound / LowerBound) / log(1 + RelativePrecision);
            if (bucketCount > maxBucketCount) {
                THROW_ERROR_EXCEPTION("Bucket count is too large")
                    << TErrorAttribute("bucket_count", bucketCount)
                    << TErrorAttribute("max_bucket_count", maxBucketCount);
            }
            if (DefaultValue && (*DefaultValue < LowerBound || *DefaultValue > UpperBound)) {
                THROW_ERROR_EXCEPTION("Default value should be between lower bound and uppper bound")
                    << TErrorAttribute("default_value", *DefaultValue)
                    << TErrorAttribute("lower_bound", LowerBound)
                    << TErrorAttribute("upper_bound", UpperBound);
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TLogDigestConfig)

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EHistoricUsageAggregationMode,
    ((None)                     (0))
    ((ExponentialMovingAverage) (1))
);

class THistoricUsageConfig
    : public NYTree::TYsonSerializable
{
public:
    EHistoricUsageAggregationMode AggregationMode;

    //! Parameter of exponential moving average (EMA) of the aggregated usage.
    //! Roughly speaking, it means that current usage ratio is twice as relevant for the
    //! historic usage as the usage ratio alpha seconds ago.
    //! EMA for unevenly spaced time series was adapted from here: https://clck.ru/HaGZs
    double EmaAlpha;

    THistoricUsageConfig()
    {
        RegisterParameter("aggregation_mode", AggregationMode)
            .Default(EHistoricUsageAggregationMode::None);

        RegisterParameter("ema_alpha", EmaAlpha)
            // TODO(eshcherbin): Adjust.
            .Default(1.0 / (24.0 * 60.0 * 60.0))
            .GreaterThanOrEqual(0.0);
    }
};

DEFINE_REFCOUNTED_TYPE(THistoricUsageConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
