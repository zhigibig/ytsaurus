#pragma once

#include "public.h"

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TLockManager
    : public TRefCounted
{
public:
    TLockManager();

    void Lock(TTimestamp timestamp, TTransactionId transactionId);
    std::vector<TTransactionId> RemoveUnconfirmedTransactions();
    void Unlock(TTransactionId transactionId);
    void Wait(TTimestamp timestamp);

    bool IsLocked();

    void Persist(const TStreamPersistenceContext& context);

private:
    class TImpl;
    TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TLockManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
