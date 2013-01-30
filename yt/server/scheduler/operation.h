#pragma once

#include "public.h"

#include <ytlib/misc/property.h>
#include <ytlib/misc/error.h>

#include <ytlib/ytree/node.h>

#include <ytlib/transaction_client/transaction.h>

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
    DEFINE_BYVAL_RO_PROPERTY(NTransactionClient::ITransactionPtr, UserTransaction);

    //! Transaction used for maintaining operation inputs and outputs.
    /*!
     *  SyncScheduler transaction is nested inside UserTransaction, if any.
     *  Input and output transactions are nested inside SyncSchedulerTransaction.
     */
    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::ITransactionPtr, SyncSchedulerTransaction);

    //! Transaction used for internal housekeeping, e.g. generating stderrs.
    /*!
     *  Not nested inside any other transaction.
     */
    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::ITransactionPtr, AsyncSchedulerTransaction);

    DEFINE_BYVAL_RO_PROPERTY(NYTree::IMapNodePtr, Spec);

    DEFINE_BYVAL_RO_PROPERTY(TInstant, StartTime);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, EndTime);

    //! Number of stderrs generated so far.
    DEFINE_BYVAL_RW_PROPERTY(int, StdErrCount);

    //! Currently existing jobs in the operation.
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TJobPtr>, Jobs);

    //! Controller that owns the operation.
    DEFINE_BYVAL_RW_PROPERTY(IOperationControllerPtr, Controller);

    //! Operation result, becomes set when the operation finishes.
    DEFINE_BYREF_RW_PROPERTY(NProto::TOperationResult, Result);

    //! Gets set when the operation is finished.
    TFuture<void> GetFinished();

    //! Marks the operation as finished.
    void SetFinished();

    //! Delegates to #NYT::NScheduler::IsOperationFinished.
    bool IsFinishedState() const;

    //! Delegates to #NYT::NScheduler::IsOperationFinishing.
    bool IsFinishingState() const;

public:
    TOperation(
        const TOperationId& operationId,
        EOperationType type,
        NTransactionClient::ITransactionPtr userTransaction,
        NYTree::IMapNodePtr spec,
        TInstant startTime,
        EOperationState state = EOperationState::Initializing);

    TPromise<void> FinishedPromise;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
