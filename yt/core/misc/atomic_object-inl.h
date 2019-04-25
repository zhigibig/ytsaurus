#pragma once
#ifndef ATOMIC_OBJECT_INL_H_
#error "Direct inclusion of this file is not allowed, include atomic_object.h"
// For the sake of sane code completion.
#include "atomic_object.h"
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

template <class T>
template <class U>
TAtomicObject<T>::TAtomicObject(U&& u)
    : Object_(std::forward<U>(u))
{ }

template <class T>
template <class U>
void TAtomicObject<T>::Store(U&& u)
{
    T newObject = std::forward<U>(u);
    {
        NConcurrency::TWriterGuard guard(Spinlock_);
        // NB: Using swap to avoid destructing the old object while holding the lock.
        std::swap(Object_, newObject);
    }
}

template <class T>
T TAtomicObject<T>::Load() const
{
    NConcurrency::TReaderGuard guard(Spinlock_);
    return Object_;
}

////////////////////////////////////////////////////////////////////////////////

template <class TOriginal, class TSerialized>
void ToProto(TSerialized* serialized, const TAtomicObject<TOriginal>& original)
{
    ToProto(serialized, original.Load());
}

template <class TOriginal, class TSerialized>
void FromProto(TAtomicObject<TOriginal>* original, const TSerialized& serialized)
{
    TOriginal data;
    FromProto(&data, serialized);
    original->Store(std::move(data));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
