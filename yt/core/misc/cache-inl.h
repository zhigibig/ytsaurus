#ifndef CACHE_INL_H_
#error "Direct inclusion of this file is not allowed, include cache.h"
#endif
#undef CACHE_INL_H_

#include "config.h"

#include <util/system/yield.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash>
const TKey& TCacheValueBase<TKey, TValue, THash>::GetKey() const
{
    return Key_;
}

template <class TKey, class TValue, class THash>
TCacheValueBase<TKey, TValue, THash>::TCacheValueBase(const TKey& key)
    : Key_(key)
{ }

template <class TKey, class TValue, class THash>
NYT::TCacheValueBase<TKey, TValue, THash>::~TCacheValueBase()
{
    if (Cache_) {
        Cache_->Unregister(Key_);
    }
}

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::Clear()
{
    NConcurrency::TWriterGuard guard(SpinLock_);

    ItemMap_.clear();
    ItemMapSize_ = 0;

    YoungerLruList_.Clear();
    YoungerWeight_ = 0;

    OlderLruList_.Clear();
    OlderWeight_ = 0;
}

template <class TKey, class TValue, class THash>
TSlruCacheBase<TKey, TValue, THash>::TSlruCacheBase(TSlruCacheConfigPtr config)
    : Config_(std::move(config))
{ }

template <class TKey, class TValue, class THash>
typename TSlruCacheBase<TKey, TValue, THash>::TValuePtr
TSlruCacheBase<TKey, TValue, THash>::Find(const TKey& key)
{
    NConcurrency::TReaderGuard readerGuard(SpinLock_);

    auto itemIt = ItemMap_.find(key);
    if (itemIt == ItemMap_.end()) {
        return nullptr;
    }

    auto* item = itemIt->second;
    if (CanTouch(item)) {
        auto writerGuard = readerGuard.Upgrade();
        Touch(item);
    }

    return item->Value;
}

template <class TKey, class TValue, class THash>
std::vector<typename TSlruCacheBase<TKey, TValue, THash>::TValuePtr>
TSlruCacheBase<TKey, TValue, THash>::GetAll()
{
    NConcurrency::TReaderGuard guard(SpinLock_);

    std::vector<TValuePtr> result;
    result.reserve(ValueMap_.size());
    for (const auto& pair : ValueMap_) {
        auto value = TRefCounted::DangerousGetPtr<TValue>(pair.second);
        if (value) {
            result.push_back(value);
        }
    }
    return result;
}

template <class TKey, class TValue, class THash>
typename TSlruCacheBase<TKey, TValue, THash>::TValuePtrOrErrorFuture
TSlruCacheBase<TKey, TValue, THash>::Lookup(const TKey& key)
{
    while (true) {
        {
            NConcurrency::TReaderGuard readerGuard(SpinLock_);

            auto itemIt = ItemMap_.find(key);
            if (itemIt != ItemMap_.end()) {
                auto* item = itemIt->second;
                if (CanTouch(item)) {
                    auto writerGuard = readerGuard.Upgrade();
                    Touch(item);
                }
                return item->ValueOrErrorPromise;
            }

            auto valueIt = ValueMap_.find(key);
            if (valueIt == ValueMap_.end()) {
                return TValuePtrOrErrorFuture();
            }

            auto value = TRefCounted::DangerousGetPtr(valueIt->second);
            if (value) {
                auto writerGuard = readerGuard.Upgrade();

                auto* item = new TItem(value);
                // This holds an extra reference to the promise state...
                auto valueOrError = item->ValueOrErrorPromise;

                ItemMap_.insert(std::make_pair(key, item));
                ++ItemMapSize_;

                PushToYounger(item, value.Get());

                writerGuard.Release();

                // ...since the item can be dead at this moment.
                TrimIfNeeded();

                return valueOrError;
            }
        }

        // Back off.
        ThreadYield();
    }
}

template <class TKey, class TValue, class THash>
bool TSlruCacheBase<TKey, TValue, THash>::BeginInsert(TInsertCookie* cookie)
{
    YCHECK(!cookie->Active_);
    auto key = cookie->GetKey();

    while (true) {
        {
            NConcurrency::TWriterGuard guard(SpinLock_);

            auto itemIt = ItemMap_.find(key);
            if (itemIt != ItemMap_.end()) {
                auto* item = itemIt->second;
                cookie->ValueOrErrorPromise_ = item->ValueOrErrorPromise;
                return false;
            }

            auto valueIt = ValueMap_.find(key);
            if (valueIt == ValueMap_.end()) {
                auto* item = new TItem();

                YCHECK(ItemMap_.insert(std::make_pair(key, item)).second);
                ++ItemMapSize_;

                cookie->ValueOrErrorPromise_ = item->ValueOrErrorPromise;
                cookie->Active_ = true;
                cookie->Cache_ = this;

                return true;
            }

            auto value = TRefCounted::DangerousGetPtr(valueIt->second);
            if (value) {
                auto* item = new TItem(value);

                YCHECK(ItemMap_.insert(std::make_pair(key, item)).second);
                ++ItemMapSize_;

                PushToYounger(item, value.Get());

                cookie->ValueOrErrorPromise_ = item->ValueOrErrorPromise;

                guard.Release();

                TrimIfNeeded();

                return false;
            }
        }

        // Back off.
        // Hopefully the object we had just extracted will be destroyed soon
        // and thus vanish from ValueMap.
        ThreadYield();
    }
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::EndInsert(TValuePtr value, TInsertCookie* cookie)
{
    YCHECK(cookie->Active_);

    auto key = value->GetKey();

    TValuePtrOrErrorPromise valueOrErrorPromise;
    {
        NConcurrency::TWriterGuard guard(SpinLock_);

        YCHECK(!value->Cache_);
        value->Cache_ = this;

        auto it = ItemMap_.find(key);
        YCHECK(it != ItemMap_.end());

        auto* item = it->second;
        item->Value = value;
        valueOrErrorPromise = item->ValueOrErrorPromise;

        YCHECK(ValueMap_.insert(std::make_pair(key, value.Get())).second);
    }

    valueOrErrorPromise.Set(value);

    {
        NConcurrency::TWriterGuard guard(SpinLock_);

        auto it = ItemMap_.find(key);
        if (it != ItemMap_.end()) {
            auto* item = it->second;
            PushToYounger(item, value.Get());
        }
    }

    OnAdded(value.Get());
    TrimIfNeeded();
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::CancelInsert(const TKey& key, const TError& error)
{
    TValuePtrOrErrorPromise valueOrErrorPromise;
    {
        NConcurrency::TWriterGuard guard(SpinLock_);

        auto it = ItemMap_.find(key);
        YCHECK(it != ItemMap_.end());

        auto* item = it->second;
        valueOrErrorPromise = item->ValueOrErrorPromise;

        ItemMap_.erase(it);
        --ItemMapSize_;

        delete item;
    }

    valueOrErrorPromise.Set(error);
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::Unregister(const TKey& key)
{
    NConcurrency::TWriterGuard guard(SpinLock_);

    YCHECK(ItemMap_.find(key) == ItemMap_.end());
    YCHECK(ValueMap_.erase(key) == 1);
}

template <class TKey, class TValue, class THash>
bool TSlruCacheBase<TKey, TValue, THash>::Remove(const TKey& key)
{
    NConcurrency::TWriterGuard guard(SpinLock_);

    auto it = ItemMap_.find(key);
    if (it == ItemMap_.end()) {
        return false;
    }

    auto* item = it->second;
    auto value = item->Value;

    ItemMap_.erase(it);
    --ItemMapSize_;

    Pop(item, value.Get());

    // Release the guard right away to prevent recursive spinlock acquisition.
    // Indeed, the item's dtor may drop the last reference
    // to the value and thus cause an invocation of TCacheValueBase::~TCacheValueBase.
    // The latter will try to acquire the spinlock.
    guard.Release();

    OnRemoved(value.Get());

    delete item;

    return true;
}

template <class TKey, class TValue, class THash>
bool TSlruCacheBase<TKey, TValue, THash>::Remove(TValuePtr value)
{
    NConcurrency::TWriterGuard guard(SpinLock_);

    auto valueIt = ValueMap_.find(value->GetKey());
    if (valueIt == ValueMap_.end() || valueIt->second != value) {
        return false;
    }
    ValueMap_.erase(valueIt);

    auto itemIt = ItemMap_.find(value->GetKey());
    if (itemIt != ItemMap_.end()) {
        auto* item = itemIt->second;
        ItemMap_.erase(itemIt);
        --ItemMapSize_;

        Pop(item, value.Get());

        delete item;
    }

    value->Cache_.Reset();

    guard.Release();

    OnRemoved(value.Get());

    return true;
}

template <class TKey, class TValue, class THash>
bool TSlruCacheBase<TKey, TValue, THash>::CanTouch(TItem* item)
{
    return NProfiling::GetCpuInstant() >= item->NextTouchInstant;
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::Touch(TItem* item)
{
    static auto MinTouchPeriod = TDuration::MilliSeconds(100);
    if (!item->Empty()) {
        auto value = item->Value;
        MoveToOlder(item, value.Get());
        item->Unlink();
        item->Younger = false;
        item->NextTouchInstant = NProfiling::GetCpuInstant() + NProfiling::DurationToCpuDuration(MinTouchPeriod);
        OlderLruList_.PushFront(item);
    }
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::OnAdded(TValue* /*value*/)
{ }

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::OnRemoved(TValue* /*value*/)
{ }

template <class TKey, class TValue, class THash>
int TSlruCacheBase<TKey, TValue, THash>::GetSize() const
{
    return ItemMapSize_;
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::PushToYounger(TItem* item, TValue* value)
{
    YASSERT(item->Empty());
    YoungerLruList_.PushFront(item);
    YoungerWeight_ += GetWeight(value);
    item->Younger = true;
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::MoveToYounger(TItem* item, TValue* value)
{
    YASSERT(!item->Empty());
    item->Unlink();
    YoungerLruList_.PushFront(item);
    if (!item->Younger) {
        i64 weight = GetWeight(value);
        OlderWeight_ -= weight;
        YoungerWeight_ += weight;
        item->Younger = true;
    }
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::MoveToOlder(TItem* item, TValue* value)
{
    YASSERT(!item->Empty());
    item->Unlink();
    OlderLruList_.PushFront(item);
    if (item->Younger) {
        i64 weight = GetWeight(value);
        YoungerWeight_ -= weight;
        OlderWeight_ += weight;
        item->Younger = false;
    }
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::Pop(TItem* item, TValue* value)
{
    if (item->Empty())
        return;
    i64 weight = GetWeight(value);
    if (item->Younger) {
        YoungerWeight_ -= weight;
    } else {
        OlderWeight_ -= weight;
    }
    item->Unlink();
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::TrimIfNeeded()
{
    // Move from older to younger.
    while (true) {
        NConcurrency::TWriterGuard guard(SpinLock_);

        if (OlderLruList_.Empty() || OlderWeight_ <= Config_->Capacity * (1 - Config_->YoungerSizeFraction))
            break;

        auto* item = &*(--OlderLruList_.End());
        auto value = item->Value;

        MoveToYounger(item, value.Get());

        guard.Release();
    }

    // Evict from younger.
    while (true) {
        NConcurrency::TWriterGuard guard(SpinLock_);

        if (YoungerLruList_.Empty() || YoungerWeight_ + OlderWeight_ <= Config_->Capacity)
            break;

        auto* item = &*(--YoungerLruList_.End());
        auto value = item->Value;

        Pop(item, value.Get());

        YCHECK(ItemMap_.erase(value->GetKey()) == 1);
        --ItemMapSize_;

        guard.Release();

        OnRemoved(value.Get());

        delete item;
    }
}

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue, class THash>
TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::TInsertCookie()
    : Active_(false)
{ }

template <class TKey, class TValue, class THash>
TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::TInsertCookie(const TKey& key)
    : Key_(key)
      , Active_(false)
{ }

template <class TKey, class TValue, class THash>
TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::TInsertCookie(TInsertCookie&& other)
    : Key_(std::move(other.Key_))
      , Cache_(std::move(other.Cache_))
      , ValueOrErrorPromise_(std::move(other.ValueOrErrorPromise_))
      , Active_(other.Active_)
{
    other.Active_ = false;
}

template <class TKey, class TValue, class THash>
TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::~TInsertCookie()
{
    Abort();
}

template <class TKey, class TValue, class THash>
typename TSlruCacheBase<TKey, TValue, THash>::TInsertCookie& TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::operator =(TInsertCookie&& other)
{
    if (this != &other) {
        Abort();
        Key_ = std::move(other.Key_);
        Cache_ = std::move(other.Cache_);
        ValueOrErrorPromise_ = std::move(other.ValueOrErrorPromise_);
        Active_ = other.Active_;
        other.Active_ = false;
    }
    return *this;
}

template <class TKey, class TValue, class THash>
TKey TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::GetKey() const
{
    return Key_;
}

template <class TKey, class TValue, class THash>
typename TSlruCacheBase<TKey, TValue, THash>::TValuePtrOrErrorFuture
TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::GetValue() const
{
    YASSERT(ValueOrErrorPromise_);
    return ValueOrErrorPromise_;
}

template <class TKey, class TValue, class THash>
bool TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::IsActive() const
{
    return Active_;
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::Cancel(const TError& error)
{
    if (Active_) {
        Cache_->CancelInsert(Key_, error);
        Active_ = false;
    }
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::EndInsert(TValuePtr value)
{
    YCHECK(Active_);
    Cache_->EndInsert(value, this);
    Active_ = false;
}

template <class TKey, class TValue, class THash>
void TSlruCacheBase<TKey, TValue, THash>::TInsertCookie::Abort()
{
    Cancel(TError("Cache item insertion aborted"));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
