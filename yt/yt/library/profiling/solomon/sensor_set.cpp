#include "sensor_set.h"
#include "private.h"

#include <library/cpp/monlib/metrics/summary_snapshot.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/misc/assert.h>

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = SolomonLogger;

////////////////////////////////////////////////////////////////////////////////

TSensorSet::TSensorSet(TSensorOptions options, i64 iteration, int windowSize)
    : Options_(options)
    , CountersCube_{windowSize, iteration}
    , TimeCountersCube_{windowSize, iteration}
    , GaugesCube_{windowSize, iteration}
    , SummariesCube_{windowSize, iteration}
    , TimersCube_{windowSize, iteration}
{ }

bool TSensorSet::IsEmpty() const
{
    return Counters_.empty() &&
        Gauges_.empty() &&
        Summaries_.empty() &&
        Timers_.empty();
}

void TSensorSet::ValidateOptions(TSensorOptions options)
{
    if (Options_ != options) {
        OnError(TError("Conflicting sensor settings")
            << TErrorAttribute("current", ToString(Options_))
            << TErrorAttribute("provided", ToString(options)));
    }
}

void TSensorSet::AddCounter(TCounterStatePtr counter)
{
    InitializeType(0);
    CountersCube_.AddAll(counter->TagIds, counter->Projections);
    Counters_.emplace(std::move(counter));
}

void TSensorSet::AddGauge(TGaugeStatePtr gauge)
{
    InitializeType(1);
    GaugesCube_.AddAll(gauge->TagIds, gauge->Projections);
    Gauges_.emplace(std::move(gauge));
}

void TSensorSet::AddSummary(TSummaryStatePtr summary)
{
    InitializeType(2);
    SummariesCube_.AddAll(summary->TagIds, summary->Projections);
    Summaries_.emplace(std::move(summary));
}

void TSensorSet::AddTimerSummary(TTimerSummaryStatePtr timer)
{
    InitializeType(3);
    TimersCube_.AddAll(timer->TagIds, timer->Projections);
    Timers_.emplace(std::move(timer));
}

void TSensorSet::AddTimeCounter(TTimeCounterStatePtr counter)
{
    InitializeType(4);
    TimeCountersCube_.AddAll(counter->TagIds, counter->Projections);
    TimeCounters_.emplace(std::move(counter));
}

int TSensorSet::Collect()
{
    int count = 0;

    auto collect = [&] (auto& set, auto& cube, auto doRead) {
        using TElement = typename std::remove_reference_t<decltype(set)>::key_type;

        std::deque<TElement> toRemove;

        cube.StartIteration();
        for (const auto& counter : set) {
            auto [value, ok] = doRead(counter);
            if (!ok) {
                toRemove.push_back(counter);
                continue;
            }

            counter->Projections.Range(counter->TagIds, [&, value=value] (auto tags) {
                cube.Update(tags, value);
            });
        }
        cube.FinishIteration();

        for (const auto& removed : toRemove) {
            cube.RemoveAll(removed->TagIds, removed->Projections);
            set.erase(removed);
        }

        count += cube.GetProjections().size();
    };

    collect(Counters_, CountersCube_, [] (auto counter) -> std::pair<i64, bool> {
        auto owner = counter->Owner.Lock();
        if (!owner) {
            return {0, false};
        }

        try {
            auto value = counter->Reader();

            auto delta = value - counter->LastValue;
            counter->LastValue = value;
            return {delta, true};
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Counter read failed");
            return {0, false};
        }
    });

    collect(TimeCounters_, TimeCountersCube_, [] (auto counter) -> std::pair<i64, bool> {
        auto owner = counter->Owner.Lock();
        if (!owner) {
            return {0, false};
        }

        auto value = owner->GetValue();

        auto delta = value - counter->LastValue;
        counter->LastValue = value;
        return {delta.MicroSeconds(), true};
    });

    collect(Gauges_, GaugesCube_, [] (auto counter) -> std::pair<double, bool> {
        auto owner = counter->Owner.Lock();
        if (!owner) {
            return {0, false};
        }

        try {
            auto value = counter->Reader();

            return {value, true};
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Gauge read failed");
            return {0, false};
        }
    });

    collect(Summaries_, SummariesCube_, [] (auto counter) -> std::pair<TSummarySnapshot<double>, bool> {
        auto owner = counter->Owner.Lock();
        if (!owner) {
            return {{}, false};
        }

        auto value = owner->GetValueAndReset();
        return {value, true};
    });

    collect(Timers_, TimersCube_, [] (auto counter) -> std::pair<TSummarySnapshot<TDuration>, bool> {
        auto owner = counter->Owner.Lock();
        if (!owner) {
            return {{}, false};
        }

        auto value = owner->GetValueAndReset();
        return {value, true};
    });

    return count;
}

void TSensorSet::ReadSensors(
    const TString& name,
    const TReadOptions& options,
    const TTagRegistry& tagsRegistry,
    NMonitoring::IMetricConsumer* consumer) const
{
    if (!Error_.IsOK()) {
        return;
    }

    auto readOptions = options;
    readOptions.Sparse = Options_.Sparse;
    readOptions.Global = Options_.Global;

    CountersCube_.ReadSensors(name, readOptions, tagsRegistry, consumer);
    TimeCountersCube_.ReadSensors(name, readOptions, tagsRegistry, consumer);
    GaugesCube_.ReadSensors(name, readOptions, tagsRegistry, consumer);
    SummariesCube_.ReadSensors(name, readOptions, tagsRegistry, consumer);
    TimersCube_.ReadSensors(name, readOptions, tagsRegistry, consumer);
}

void TSensorSet::LegacyReadSensors(const TString& name, TTagRegistry* tagRegistry)
{
    auto prefix = TStringBuf{name};
    prefix.SkipPrefix("yt");
    prefix.SkipPrefix("yp");
    auto fullName = TString{prefix};

    auto readLegacy = [&] (auto& set, auto doRead) {
        for (const auto& counter : set) {
            TQueuedSample sample;

            auto empty = doRead(counter, &sample);
            if (Options_.Sparse && empty) {
                continue;
            }

            sample.Time = GetCpuInstant();
            sample.Path = fullName;
            sample.TagIds = tagRegistry->EncodeLegacy(counter->TagIds);

            TProfileManager::Get()->Enqueue(sample, false);
        }
    };

    readLegacy(Counters_, [&] (auto state, auto* sample) -> bool {
        sample->MetricType = EMetricType::Counter;

        auto owner = state->Owner.Lock();
        if (!owner) {
            sample->Value = state->LastValue;
            return true;
        }

        sample->Value = state->Reader();
        return sample->Value == state->LastValue;
    });

    readLegacy(TimeCounters_, [&] (auto state, auto* sample) -> bool {
        sample->MetricType = EMetricType::Counter;

        auto owner = state->Owner.Lock();
        if (!owner) {
            sample->Value = state->LastValue.MicroSeconds();
            return true;
        }

        sample->Value = owner->GetValue().MicroSeconds();
        return sample->Value == static_cast<NProfiling::TValue>(state->LastValue.MicroSeconds());
    });

    readLegacy(Gauges_, [&] (auto state, auto* sample) -> bool {
        sample->MetricType = EMetricType::Gauge;

        auto owner = state->Owner.Lock();
        if (!owner) {
            sample->Value = 0.0;
            return true;
        }

        sample->Value = state->Reader();
        return sample->Value == 0.0;
    });

    readLegacy(Summaries_, [&] (auto state, auto* sample) -> bool {
        sample->MetricType = EMetricType::Gauge;

        auto owner = state->Owner.Lock();
        if (!owner) {
            sample->Value = 0.0;
            return true;
        }

        auto value = owner->GetValue();
        if (value.Count() == 0) {
            return true;
        }

        sample->Value = value.Max();
        return false;
    });

    readLegacy(Timers_, [&] (auto state, auto* sample) -> bool {
        sample->MetricType = EMetricType::Gauge;

        auto owner = state->Owner.Lock();
        if (!owner) {
            sample->Value = 0.0;
            return true;
        }

        auto value = owner->GetValue();
        if (value.Count() == 0) {
            return true;
        }

        sample->Value = value.Max().MicroSeconds();
        return false;
    });
}

int TSensorSet::GetObjectCount() const
{
    return Counters_.size() +
        TimeCounters_.size() +
        Gauges_.size() +
        Summaries_.size() +
        Timers_.size();
}

int TSensorSet::GetCubeSize() const
{
    return CountersCube_.GetSize() +
        TimeCountersCube_.GetSize() +
        GaugesCube_.GetSize() +
        SummariesCube_.GetSize() +
        TimersCube_.GetSize();
}

const TError& TSensorSet::GetError() const
{
    return Error_;
}

void TSensorSet::OnError(TError error)
{
    if (Error_.IsOK()) {
        Error_ = std::move(error);
    }
}

void TSensorSet::InitializeType(int type)
{
    if (Type_ && *Type_ != type) {
        OnError(TError("Conflicting sensor types")
            << TErrorAttribute("expected", *Type_)
            << TErrorAttribute("provided", type));
    }

    if (!Type_) {
        Type_ = type;
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
