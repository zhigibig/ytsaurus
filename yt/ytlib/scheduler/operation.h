#pragma once

#include "public.h"

#include <ytlib/misc/property.h>
#include <ytlib/misc/error.h>
#include <ytlib/ytree/ytree.h>
#include <ytlib/scheduler/scheduler_service.pb.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TOperation
    : public TRefCounted
{
    DEFINE_BYVAL_RO_PROPERTY(TOperationId, OperationId);
    
    DEFINE_BYVAL_RO_PROPERTY(EOperationType, Type);
    
    DEFINE_BYVAL_RW_PROPERTY(EOperationState, State);

    //! User-supplied transaction where the operation resides.
    DEFINE_BYVAL_RO_PROPERTY(TTransactionId, TransactionId);

    DEFINE_BYVAL_RO_PROPERTY(NYTree::IMapNodePtr, Spec);

    DEFINE_BYVAL_RO_PROPERTY(TInstant, StartTime);

    //! Currently existing jobs in the operation.
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TJobPtr>, Jobs);

    DEFINE_BYVAL_RW_PROPERTY(IOperationControllerPtr, Controller);

    //! Operation result, becomes set when the operation finishes.
    DEFINE_BYREF_RW_PROPERTY(NProto::TOperationResult, Result);

    //! Gets set when the operation is finished.
    TFuture<void> GetFinished();

    //! Marks the operation as finished.
    void SetFinished();

public:
    TOperation(
        const TOperationId& operationId,
        EOperationType type,
        const TTransactionId& transactionId,
        NYTree::IMapNodePtr spec,
        TInstant startTime,
        EOperationState state = EOperationState::Initializing);

    TPromise<void> FinishedPromise;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
