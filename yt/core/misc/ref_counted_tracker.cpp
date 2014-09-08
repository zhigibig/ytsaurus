#include "stdafx.h"
#include "ref_counted_tracker.h"
#include "demangle.h"

#include <core/ytree/fluent.h>

#include <util/system/tls.h>

#include <algorithm>

namespace NYT {

using namespace NYTree;
using namespace NYson;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TRefCountedTracker::TStatisticsHolder
{
public:
    bool IsInitialized() const
    {
        return Owner_ != nullptr;
    }

    void Initialize(TRefCountedTracker* owner)
    {
        Owner_ = owner;
    }

    TAnonymousStatistics* GetStatistics()
    {
        return &Statistics_;
    }

    ~TStatisticsHolder()
    {
        Owner_->FlushPerThreadStatistics(this);
    }

private:
    TRefCountedTracker* Owner_ = nullptr;
    TAnonymousStatistics Statistics_;

};

static PER_THREAD void* CurrentThreadStatisticsBegin = nullptr; // actually TRefCountedTracker::TAnonymousSlot*
static PER_THREAD int CurrentThreadStatisticsSize = 0;

////////////////////////////////////////////////////////////////////////////////

TRefCountedTracker::TNamedSlot::TNamedSlot(TRefCountedTypeKey key)
    : Key_(key)
{ }

TRefCountedTypeKey TRefCountedTracker::TNamedSlot::GetKey() const
{
    return Key_;
}

Stroka TRefCountedTracker::TNamedSlot::GetName() const
{
    return DemangleCxxName(static_cast<const std::type_info*>(Key_)->name());
}

TRefCountedTracker::TAnonymousSlot& TRefCountedTracker::TAnonymousSlot::operator+=(const TAnonymousSlot& other)
{
    ObjectsAllocated_ += other.ObjectsAllocated_;
    BytesAllocated_ += other.BytesAllocated_;
    ObjectsFreed_ += other.ObjectsFreed_;
    BytesFreed_ += other.BytesFreed_;
    return *this;
}

i64 TRefCountedTracker::TAnonymousSlot::GetObjectsAllocated() const
{
    return ObjectsAllocated_;
}

i64 TRefCountedTracker::TAnonymousSlot::GetObjectsAlive() const
{
    return ObjectsAllocated_ - ObjectsFreed_;
}

i64 TRefCountedTracker::TAnonymousSlot::GetBytesAllocated() const
{
    return BytesAllocated_;
}

i64 TRefCountedTracker::TAnonymousSlot::GetBytesAlive() const
{
    return BytesAllocated_ - BytesFreed_;
}

int TRefCountedTracker::GetTrackedThreadCount() const
{
    TGuard<TForkAwareSpinLock> guard(SpinLock_);
    return PerThreadHolders_.size();
}

////////////////////////////////////////////////////////////////////////////////

TRefCountedTracker* TRefCountedTracker::Get()
{
    return Singleton<TRefCountedTracker>();
}

TRefCountedTypeCookie TRefCountedTracker::GetCookie(TRefCountedTypeKey key)
{
    TGuard<TForkAwareSpinLock> guard(SpinLock_);
    auto it = KeyToCookie_.find(key);
    if (it != KeyToCookie_.end()) {
        return it->second;
    }
    auto cookie = ++LastUsedCookie;
    KeyToCookie_.insert(std::make_pair(key, cookie));
    return cookie;
}

TRefCountedTracker::TRefCountedTracker()
{
    Active_ = true;
}

TRefCountedTracker::~TRefCountedTracker()
{
    Active_ = false;
}

TRefCountedTracker::TNamedStatistics TRefCountedTracker::GetSnapshot() const
{
    TGuard<TForkAwareSpinLock> guard(SpinLock_);

    TNamedStatistics result;
    for (auto cookie : CookieToKey_) {
        result.emplace_back(cookie);
    }

    auto accumulateResult = [&] (const TAnonymousStatistics& statistics) {
        for (auto index = 0; index < result.size() && index < statistics.size(); ++index) {
            result[index] += statistics[index];
        }
    };

    accumulateResult(GlobalStatistics_);
    for (auto* holder : PerThreadHolders_) {
        accumulateResult(*holder->GetStatistics());
    }

    return result;
}

void TRefCountedTracker::SortSnapshot(TNamedStatistics* snapshot, int sortByColumn)
{
    std::function<bool(const TNamedSlot& lhs, const TNamedSlot& rhs)> predicate;
    switch (sortByColumn) {
        case 0:
        default:
            predicate = [] (const TNamedSlot& lhs, const TNamedSlot& rhs) {
                return lhs.GetObjectsAlive() > rhs.GetObjectsAlive();
            };
            break;

        case 1:
            predicate = [] (const TNamedSlot& lhs, const TNamedSlot& rhs) {
                return lhs.GetObjectsAllocated() > rhs.GetObjectsAllocated();
            };
            break;

        case 2:
            predicate = [] (const TNamedSlot& lhs, const TNamedSlot& rhs) {
                return lhs.GetBytesAlive() > rhs.GetBytesAlive();
            };
            break;

        case 3:
            predicate = [] (const TNamedSlot& lhs, const TNamedSlot& rhs) {
                return lhs.GetBytesAllocated() > rhs.GetBytesAllocated();
            };
            break;

        case 4:
            predicate = [] (const TNamedSlot& lhs, const TNamedSlot& rhs) {
                return lhs.GetName() < rhs.GetName();
            };
            break;
    }
    std::sort(snapshot->begin(), snapshot->end(), predicate);
}

Stroka TRefCountedTracker::GetDebugInfo(int sortByColumn) const
{
    auto snapshot = GetSnapshot();
    SortSnapshot(&snapshot, sortByColumn);

    TStringBuilder builder;

    size_t totalObjectsAlive = 0;
    size_t totalObjectsAllocated = 0;
    size_t totalBytesAlive = 0;
    size_t totalBytesAllocated = 0;

    builder.AppendFormat(
        "%10s %10s %15s %15s %s\n",
        "ObjAlive",
        "ObjAllocated",
        "BytesAlive",
        "BytesAllocated",
        "Name");

    builder.AppendString("-------------------------------------------------------------------------------------------------------------\n");

    for (const auto& slot : snapshot) {
        totalObjectsAlive += slot.GetObjectsAlive();
        totalObjectsAllocated += slot.GetObjectsAllocated();
        totalBytesAlive += slot.GetBytesAlive();
        totalBytesAllocated += slot.GetBytesAllocated();

        builder.AppendFormat(
            "%10" PRISZT " %10" PRISZT " %15" PRISZT " %15" PRISZT " %s\n",
            slot.GetObjectsAlive(),
            slot.GetObjectsAllocated(),
            slot.GetBytesAlive(),
            slot.GetBytesAllocated(),
            ~slot.GetName());
    }

    builder.AppendString("-------------------------------------------------------------------------------------------------------------\n");
    builder.AppendFormat(
        "%10" PRISZT " %10" PRISZT " %15" PRISZT " %15" PRISZT " %s\n",
        totalObjectsAlive,
        totalObjectsAllocated,
        totalBytesAlive,
        totalBytesAllocated,
        "Total");

    return builder.Flush();
}

TYsonProducer TRefCountedTracker::GetMonitoringProducer() const
{
    return BIND([=] (IYsonConsumer* consumer) {
        auto slots = GetSnapshot();
        SortSnapshot(&slots, -1);

        i64 totalObjectsAlive = 0;
        i64 totalObjectsAllocated = 0;
        i64 totalBytesAlive = 0;
        i64 totalBytesAllocated = 0;

        for (const auto& slot : slots) {
            totalObjectsAlive += slot.GetObjectsAlive();
            totalObjectsAllocated += slot.GetObjectsAllocated();
            totalBytesAlive += slot.GetBytesAlive();
            totalBytesAllocated += slot.GetBytesAllocated();
        }

        BuildYsonFluently(consumer)
            .BeginMap()
                .Item("statistics").DoListFor(slots, [] (TFluentList fluent, const TNamedSlot& slot) {
                    fluent
                        .Item().BeginMap()
                            .Item("name").Value(slot.GetName())
                            .Item("objects_alive").Value(slot.GetObjectsAlive())
                            .Item("objects_allocated").Value(slot.GetObjectsAllocated())
                            .Item("bytes_alive").Value(slot.GetBytesAlive())
                            .Item("bytes_allocated").Value(slot.GetBytesAllocated())
                        .EndMap();
                })
                .Item("total").BeginMap()
                    .Item("objects_alive").Value(totalObjectsAlive)
                    .Item("objects_allocated").Value(totalObjectsAllocated)
                    .Item("bytes_alive").Value(totalBytesAlive)
                    .Item("bytes_allocated").Value(totalBytesAllocated)
                .EndMap()
            .EndMap();
    });
}

i64 TRefCountedTracker::GetObjectsAllocated(TRefCountedTypeKey key)
{
    return GetSlot(key).GetObjectsAllocated();
}

i64 TRefCountedTracker::GetObjectsAlive(TRefCountedTypeKey key)
{
    return GetSlot(key).GetObjectsAlive();
}

i64 TRefCountedTracker::GetAllocatedBytes(TRefCountedTypeKey key)
{
    return GetSlot(key).GetBytesAllocated();
}

i64 TRefCountedTracker::GetAliveBytes(TRefCountedTypeKey key)
{
    return GetSlot(key).GetBytesAlive();
}

TRefCountedTracker::TNamedSlot TRefCountedTracker::GetSlot(TRefCountedTypeKey key)
{
    auto cookie = GetCookie(key);

    TNamedSlot result(key);
    auto accumulateResult = [&] (const TAnonymousStatistics& statistics) {
        if (cookie < statistics.size()) {
            result += statistics[cookie];
        }
    };

    TGuard<TForkAwareSpinLock> guard(SpinLock_);
    accumulateResult(GlobalStatistics_);
    for (auto* holder : PerThreadHolders_) {
        accumulateResult(*holder->GetStatistics());
    }

    return result;
}

TRefCountedTracker::TAnonymousSlot* TRefCountedTracker::GetPerThreadSlot(TRefCountedTypeCookie cookie)
{
    if (cookie >= CurrentThreadStatisticsSize) {
        STATIC_THREAD(TStatisticsHolder) Holder;
        auto* holder = Holder.GetPtr();

        if (!holder->IsInitialized()) {
            holder->Initialize(this);
            TGuard<TForkAwareSpinLock> guard(SpinLock_);
            YCHECK(PerThreadHolders_.insert(holder).second);
        }

        auto* statistics = holder->GetStatistics();
        if (statistics->size() <= cookie) {
            statistics->resize(std::max(
                static_cast<size_t>(cookie + 1),
                statistics->size() * 2));
        }

        CurrentThreadStatisticsBegin = statistics->data();
        CurrentThreadStatisticsSize = statistics->size();
    }
    return reinterpret_cast<TAnonymousSlot*>(CurrentThreadStatisticsBegin) + cookie;
}

void TRefCountedTracker::FlushPerThreadStatistics(TStatisticsHolder* holder)
{
    if (!Active_)
        return;

    TGuard<TForkAwareSpinLock> guard(SpinLock_);
    const auto& perThreadStatistics = *holder->GetStatistics();
    if (GlobalStatistics_.size() < perThreadStatistics.size()) {
        GlobalStatistics_.resize(std::max(
            perThreadStatistics.size(),
            GlobalStatistics_.size() * 2));
    }
    for (auto index = 0; index < perThreadStatistics.size(); ++index) {
        GlobalStatistics_[index] += perThreadStatistics[index];
    }
    YCHECK(PerThreadHolders_.erase(holder) == 1);
}

////////////////////////////////////////////////////////////////////////////////

void DumpRefCountedTracker(int sortByColumn)
{
    fprintf(stderr, "%s", ~TRefCountedTracker::Get()->GetDebugInfo(sortByColumn));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

