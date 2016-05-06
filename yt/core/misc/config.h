#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

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
    }
};

DEFINE_REFCOUNTED_TYPE(TSlruCacheConfig)

////////////////////////////////////////////////////////////////////////////////

class TExpiringCacheConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    TDuration SuccessExpirationTime;
    TDuration SuccessProbationTime;
    TDuration FailureExpirationTime;

    TExpiringCacheConfig()
    {
        RegisterParameter("success_expiration_time", SuccessExpirationTime)
            .Default(TDuration::Seconds(15));
        RegisterParameter("success_probation_time", SuccessProbationTime)
            .Default(TDuration::Seconds(10));
        RegisterParameter("failure_expiration_time", FailureExpirationTime)
            .Default(TDuration::Seconds(15));

        RegisterValidator([&] () {
            if (SuccessProbationTime > SuccessExpirationTime) {
                THROW_ERROR_EXCEPTION("\"success_probation_time\" must be less than \"success_expiration_time\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TExpiringCacheConfig)

////////////////////////////////////////////////////////////////////////////////

class TLogDigestConfig
    : virtual public NYTree::TYsonSerializable
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
    TNullable<double> DefaultValue;

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

        RegisterValidator([&] () {
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

} // namespace NYT
