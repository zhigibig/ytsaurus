#ifndef CONNECTION_POOL_INL_H
#error "Direct inclusion of this file is not allowed, include connection_pool.h"
// For the sake of sane code completion.
#include "connection_pool.h"
#endif

namespace NYT::NHttp::NDetail {

////////////////////////////////////////////////////////////////////////////////

template <class T>
TConnectionReuseWrapper<T>::~TConnectionReuseWrapper()
{
    if (T::IsSafeToReuse()) {
        T::Reset();
    } else if (ReusableState_) {
        ReusableState_->Reusable = false;
    }
}

template <class T>
void TConnectionReuseWrapper<T>::SetReusableState(TReusableConnectionStatePtr reusableState)
{
    ReusableState_ = std::move(reusableState);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHttp::NDetail
