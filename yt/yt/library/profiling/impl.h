#pragma once

#include "public.h"
#include "sensor.h"
#include "summary.h"

#include <yt/core/misc/weak_ptr.h>
#include <yt/core/misc/ref_counted.h>

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

struct IRegistryImpl
    : public TRefCounted
{
public:
    virtual ICounterImplPtr RegisterCounter(
        const TString& name,
        const TTagSet& tags,
        TSensorOptions options) = 0;

    virtual ITimeCounterImplPtr RegisterTimeCounter(
        const TString& name,
        const TTagSet& tags,
        TSensorOptions options) = 0;

    virtual IGaugeImplPtr RegisterGauge(
        const TString& name,
        const TTagSet& tags,
        TSensorOptions options) = 0;

    virtual ITimeGaugeImplPtr RegisterTimeGauge(
        const TString& name,
        const TTagSet& tags,
        TSensorOptions options) = 0;

    virtual ISummaryImplPtr RegisterSummary(
        const TString& name,
        const TTagSet& tags,
        TSensorOptions options) = 0;

    virtual ITimerImplPtr RegisterTimerSummary(
        const TString& name,
        const TTagSet& tags,
        TSensorOptions options) = 0;

    virtual ITimerImplPtr RegisterExponentialTimerHistogram(
        const TString& name,
        const TTagSet& tags,
        TSensorOptions options) = 0;

    virtual void RegisterFuncCounter(
        const TString& name,
        const TTagSet& tags,
        TSensorOptions options,
        const TRefCountedPtr& owner,
        std::function<i64()> reader) = 0;

    virtual void RegisterFuncGauge(
        const TString& name,
        const TTagSet& tags,
        TSensorOptions options,
        const TRefCountedPtr& owner,
        std::function<double()> reader) = 0;

    virtual void RegisterProducer(
        const TString& prefix,
        const TTagSet& tags,
        TSensorOptions options,
        const ISensorProducerPtr& owner) = 0;
};

DEFINE_REFCOUNTED_TYPE(IRegistryImpl)

IRegistryImplPtr GetGlobalRegistry();

////////////////////////////////////////////////////////////////////////////////

struct ICounterImpl
    : public TRefCounted
{
    virtual void Increment(i64 delta) = 0;
    virtual i64 GetValue() = 0;
};

DEFINE_REFCOUNTED_TYPE(ICounterImpl)

////////////////////////////////////////////////////////////////////////////////

struct ITimeCounterImpl
    : public TRefCounted
{
    virtual void Add(TDuration delta) = 0;

    virtual TDuration GetValue() = 0;
};

DEFINE_REFCOUNTED_TYPE(ITimeCounterImpl)

////////////////////////////////////////////////////////////////////////////////

struct IGaugeImpl
    : public TRefCounted
{
    virtual void Update(double value) = 0;
    virtual double GetValue() = 0;
};

DEFINE_REFCOUNTED_TYPE(IGaugeImpl)

////////////////////////////////////////////////////////////////////////////////

struct ITimeGaugeImpl
    : public TRefCounted
{
    virtual void Update(TDuration value) = 0;
    virtual TDuration GetValue() = 0;
};

DEFINE_REFCOUNTED_TYPE(ITimeGaugeImpl)

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct ISummaryImplBase
    : public TRefCounted
{
    virtual void Record(T value) = 0;

    virtual TSummarySnapshot<T> GetValue() = 0;
    virtual TSummarySnapshot<T> GetValueAndReset() = 0;
};

DEFINE_REFCOUNTED_TYPE(ISummaryImpl)
DEFINE_REFCOUNTED_TYPE(ITimerImpl)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
