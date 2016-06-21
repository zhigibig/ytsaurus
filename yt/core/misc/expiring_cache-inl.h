#ifndef EXPIRING_CACHE_INL_H_
#error "Direct inclusion of this file is not allowed, include expiring_cache.h"
#endif

#include "config.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class TKey, class TValue>
TExpiringCache<TKey, TValue>::TExpiringCache(TExpiringCacheConfigPtr config)
    : Config_(std::move(config))
{ }

template <class TKey, class TValue>
TFuture<TValue> TExpiringCache<TKey, TValue>::Get(const TKey& key)
{
    auto now = TInstant::Now();

    // Fast path.
    {
        NConcurrency::TReaderGuard guard(SpinLock_);
        auto it = Map_.find(key);
        if (it != Map_.end()) {
            const auto& entry = it->second;
            if (now < entry->Deadline) {
                return entry->Promise;
            }
        }
    }

    // Slow path.
    {
        NConcurrency::TWriterGuard guard(SpinLock_);
        auto it = Map_.find(key);
        if (it == Map_.end()) {
            auto entry = New<TEntry>();
            entry->Deadline = TInstant::Max();
            auto promise = entry->Promise = NewPromise<TValue>();
            // NB: we don't want to hold a strong reference to entry after releasing the guard, so we make a weak reference here.
            auto weakEntry = MakeWeak(entry);
            YCHECK(Map_.insert(std::make_pair(key, std::move(entry))).second);
            guard.Release();
            InvokeGet(weakEntry, key);
            return promise;
        }

        auto& entry = it->second;
        const auto& promise = entry->Promise;
        if (!promise.IsSet()) {
            return promise;
        }

        if (now > entry->Deadline) {
            // Evict and retry.
            NConcurrency::TDelayedExecutor::CancelAndClear(entry->ProbationCookie);
            Map_.erase(it);
            guard.Release();
            return Get(key);
        }

        return promise;
    }
}

template <class TKey, class TValue>
bool TExpiringCache<TKey, TValue>::TryRemove(const TKey& key)
{
    NConcurrency::TWriterGuard guard(SpinLock_);
    return Map_.erase(key) != 0;
}

template <class TKey, class TValue>
void TExpiringCache<TKey, TValue>::Clear()
{
    NConcurrency::TWriterGuard guard(SpinLock_);
    Map_.clear();
}

template <class TKey, class TValue>
void TExpiringCache<TKey, TValue>::InvokeGet(const TWeakPtr<TEntry>& weakEntry, const TKey& key)
{
    NConcurrency::TWriterGuard guard(SpinLock_);

    if (weakEntry.IsExpired()) {
        return;
    }

    auto it = Map_.find(key);
    Y_ASSERT(it != Map_.end() && it->second == weakEntry.Lock());

    auto future = DoGet(key);

    guard.Release();

    future.Subscribe(BIND([=, this_ = MakeStrong(this)] (const TErrorOr<TValue>& valueOrError) {
        NConcurrency::TWriterGuard guard(SpinLock_);

        auto entry = weakEntry.Lock();
        if (!entry) {
            return;
        }

        auto it = Map_.find(key);
        Y_ASSERT(it != Map_.end() && it->second == entry);

        auto expirationTime = valueOrError.IsOK() ? Config_->SuccessExpirationTime : Config_->FailureExpirationTime;
        entry->Deadline = TInstant::Now() + expirationTime;
        if (entry->Promise.IsSet()) {
            entry->Promise = MakePromise(valueOrError);
        } else {
            entry->Promise.Set(valueOrError);
        }

        if (valueOrError.IsOK()) {
            NTracing::TNullTraceContextGuard guard;
            entry->ProbationCookie = NConcurrency::TDelayedExecutor::Submit(
                BIND(&TExpiringCache::InvokeGet, MakeWeak(this), MakeWeak(entry), key),
                Config_->SuccessProbationTime);
        }
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
