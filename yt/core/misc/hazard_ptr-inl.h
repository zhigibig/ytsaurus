#pragma once
#ifndef HAZARD_PTR_INL_H_
#error "Direct inclusion of this file is not allowed, include hazard_ptr.h"
// For the sake of sane code completion.
#include "hazard_ptr.h"
#endif
#undef HAZARD_PTR_INL_H_

#include <yt/core/logging/log.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

extern const NLogging::TLogger LockFreePtrLogger;

////////////////////////////////////////////////////////////////////////////////

template <class TPtrLoader>
void* AcquireHazardPointer(const TPtrLoader& ptrLoader, void* localPtr)
{
    YT_ASSERT(!HazardPointer.load(std::memory_order_relaxed));

    if (!localPtr) {
        return nullptr;
    }

    void* checkPtr;
    do {
        HazardPointer.store(localPtr, std::memory_order_relaxed);
        checkPtr = localPtr;
        localPtr = ptrLoader();
    } while (localPtr != checkPtr);

    return localPtr;
}

inline void ReleaseHazardPointer()
{
    HazardPointer.store(nullptr, std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
THazardPtr<T>::THazardPtr(THazardPtr&& other)
    : Ptr_(other.Ptr_)
{
    other.Ptr_ = nullptr;
}

template <class T>
THazardPtr<T>& THazardPtr<T>::operator=(THazardPtr&& other)
{
    YT_VERIFY(!Ptr_);
    Ptr_ = other.Ptr_;
    other.Ptr_ = nullptr;
    return *this;
}

template <class T>
template <class TPtrLoader>
THazardPtr<T> THazardPtr<T>::Acquire(const TPtrLoader& ptrLoader, T* localPtr)
{
    localPtr = static_cast<T*>(AcquireHazardPointer(ptrLoader, localPtr));
    return THazardPtr(localPtr);
}

template <class T>
template <class TPtrLoader>
THazardPtr<T> THazardPtr<T>::Acquire(const TPtrLoader& ptrLoader)
{
    auto localPtr = static_cast<T*>(AcquireHazardPointer(ptrLoader, ptrLoader()));
    return THazardPtr(localPtr);
}

template <class T>
void THazardPtr<T>::Reset()
{
    if (Ptr_) {
        ReleaseHazardPointer();
    }
}

template <class T>
THazardPtr<T>::~THazardPtr()
{
    Reset();
}

template <class T>
T* THazardPtr<T>::Get() const
{
    return Ptr_;
}

template <class T>
T& THazardPtr<T>::operator*() const
{
    YT_ASSERT(Ptr_);
    return *Ptr_;
}

template <class T>
T* THazardPtr<T>::operator->() const
{
    YT_ASSERT(Ptr_);
    return Ptr_;
}

template <class T>
THazardPtr<T>::operator bool() const
{
    return Ptr_ != nullptr;
}

template <class T>
THazardPtr<T>::THazardPtr(std::nullptr_t)
{ }

template <class T>
THazardPtr<T>::THazardPtr(T* ptr)
    : Ptr_(ptr)
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
