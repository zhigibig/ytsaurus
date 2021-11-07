#include "producer.h"

#include <util/system/compiler.h>

#include <library/cpp/yt/memory/new.h>

namespace NYT::NProfiling {

////////////////////////////////////////////////////////////////////////////////

TWithTagGuard::TWithTagGuard(ISensorWriter* writer)
    : Writer_(writer)
{
    YT_VERIFY(Writer_);
}

TWithTagGuard::TWithTagGuard(ISensorWriter* writer, const TTag& tag)
    : Writer_(writer)
{
    YT_VERIFY(Writer_);
    AddTag(tag);
}

void TWithTagGuard::AddTag(const TTag& tag)
{
    Writer_->PushTag(tag);
    ++AddedTagCount_;
}

TWithTagGuard::~TWithTagGuard()
{
    for (int i = 0; i < AddedTagCount_; ++i) {
        Writer_->PopTag();
    }
}

////////////////////////////////////////////////////////////////////////////////

void TSensorBuffer::PushTag(const TTag& tag)
{
    Tags_.push_back(tag);
}

void TSensorBuffer::PopTag()
{
    Tags_.pop_back();
}

void TSensorBuffer::AddGauge(const TString& name, double value)
{
    Gauges_.emplace_back(name, Tags_, value);
}

void TSensorBuffer::AddCounter(const TString& name, i64 value)
{
    Counters_.emplace_back(name, Tags_, value);
}

const std::vector<std::tuple<TString, TTagList, i64>>& TSensorBuffer::GetCounters() const
{
    return Counters_;
}

const std::vector<std::tuple<TString, TTagList, double>>& TSensorBuffer::GetGauges() const
{
    return Gauges_;
}

void TSensorBuffer::WriteTo(ISensorWriter* writer)
{
    for (const auto& [name, tags, value] : Counters_) {
        for (const auto& tag : tags) {
            writer->PushTag(tag);
        }

        writer->AddCounter(name, value);

        for (size_t i = 0; i < tags.size(); i++) {
            writer->PopTag();
        }
    }

    for (const auto& [name, tags, value] : Gauges_) {
        for (const auto& tag : tags) {
            writer->PushTag(tag);
        }

        writer->AddGauge(name, value);

        for (size_t i = 0; i < tags.size(); i++) {
            writer->PopTag();
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

TIntrusivePtr<TSensorBuffer> ISensorProducer::GetBuffer()
{
    auto buffer = New<TSensorBuffer>();
    CollectSensors(buffer.Get());
    return buffer;
}

////////////////////////////////////////////////////////////////////////////////

void TBufferedProducer::CollectSensors(ISensorWriter* )
{
    YT_ABORT();
}

TIntrusivePtr<TSensorBuffer> TBufferedProducer::GetBuffer()
{
    auto guard = Guard(Lock_);
    if (Enabled_) {
        if (Buffer_) {
            return Buffer_;
        } else {
            return New<TSensorBuffer>();
        }
    }

    return nullptr;
}

void TBufferedProducer::SetEnabled(bool enabled)
{
    auto guard = Guard(Lock_);
    Enabled_ = enabled;
}

void TBufferedProducer::Update(TSensorBuffer buffer)
{
    auto ptr = New<TSensorBuffer>(std::move(buffer));
    auto guard = Guard(Lock_);
    Buffer_ = ptr;
}

void TBufferedProducer::Update(std::function<void(ISensorWriter*)> cb)
{
    TSensorBuffer buffer;
    cb(&buffer);
    Update(std::move(buffer));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NProfiling
