#pragma once

#include "public.h"

#include <core/misc/property.h>
#include <core/misc/error.h>
#include <core/misc/ref.h>

#include <core/actions/future.h>

#include <core/ytree/node.h>

#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/scheduler/scheduler_service.pb.h>
#include <ytlib/scheduler/statistics.h>

#include <ytlib/hydra/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TOperation
    : public TRefCounted
{
public:
    DEFINE_BYVAL_RO_PROPERTY(TOperationId, Id);

    DEFINE_BYVAL_RO_PROPERTY(EOperationType, Type);

    DEFINE_BYVAL_RO_PROPERTY(NRpc::TMutationId, MutationId);

    DEFINE_BYVAL_RW_PROPERTY(EOperationState, State);
    DEFINE_BYVAL_RW_PROPERTY(bool, Suspended);
    DEFINE_BYVAL_RW_PROPERTY(bool, Queued);

    //! User-supplied transaction where the operation resides.
    DEFINE_BYVAL_RO_PROPERTY(NTransactionClient::TTransactionPtr, UserTransaction);

    //! Transaction used for maintaining operation inputs and outputs.
    /*!
     *  SyncSchedulerTransaction is nested inside UserTransaction, if any.
     *  Input and output transactions are nested inside SyncSchedulerTransaction.
     */
    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::TTransactionPtr, SyncSchedulerTransaction);

    //! Transaction used for internal housekeeping, e.g. generating stderrs.
    /*!
     *  Not nested inside any other transaction.
     */
    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::TTransactionPtr, AsyncSchedulerTransaction);

    //! Transaction used for taking snapshot of operation input.
    /*!
     *  InputTransaction is nested inside SyncSchedulerTransaction.
     */
    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::TTransactionPtr, InputTransaction);

    //! Transaction used for locking and writing operation output.
    /*!
     *  OutputTransaction is nested inside SyncSchedulerTransaction.
     */
    DEFINE_BYVAL_RW_PROPERTY(NTransactionClient::TTransactionPtr, OutputTransaction);

    DEFINE_BYVAL_RO_PROPERTY(NYTree::IMapNodePtr, Spec);

    DEFINE_BYVAL_RO_PROPERTY(Stroka, AuthenticatedUser);

    DEFINE_BYVAL_RO_PROPERTY(TInstant, StartTime);
    DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, FinishTime);

    //! Number of stderrs generated so far.
    DEFINE_BYVAL_RW_PROPERTY(int, StderrCount);

    //! Maximum number of stderrs to capture.
    DEFINE_BYVAL_RW_PROPERTY(int, MaxStderrCount);

    //! Scheduling tag.
    DEFINE_BYVAL_RW_PROPERTY(TNullable<Stroka>, SchedulingTag);

    //! Currently existing jobs in the operation.
    DEFINE_BYREF_RW_PROPERTY(yhash_set<TJobPtr>, Jobs);

    //! Controller that owns the operation.
    DEFINE_BYVAL_RW_PROPERTY(IOperationControllerPtr, Controller);

    //! Operation result, becomes set when the operation finishes.
    DEFINE_BYREF_RW_PROPERTY(NProto::TOperationResult, Result);

    //! If |true| then either the operation has been started during this very
    //! incarnation of the scheduler or the operation was revived but its previous
    //! progress was lost.
    DEFINE_BYVAL_RW_PROPERTY(bool, CleanStart);

    //! Holds a snapshot (generated by calling IOperationController::SaveSnapshot) during operation revival stage.
    DEFINE_BYREF_RW_PROPERTY(TSharedRef, Snapshot);

    //! Gets set when the operation is started.
    TFuture<TOperationPtr> GetStarted();

    //! Set operation start result.
    void SetStarted(const TError& error);

    //! Gets set when the operation is finished.
    TFuture<void> GetFinished();

    //! Marks the operation as finished.
    void SetFinished();

    //! Delegates to #NYT::NScheduler::IsOperationFinished.
    bool IsFinishedState() const;

    //! Delegates to #NYT::NScheduler::IsOperationFinishing.
    bool IsFinishingState() const;

    void UpdateJobStatistics(const TJobPtr& job);

    void BuildJobStatistics(NYson::IYsonConsumer* consumer) const;

    TOperation(
        const TOperationId& operationId,
        EOperationType type,
        const NRpc::TMutationId& mutationId,
        NTransactionClient::TTransactionPtr userTransaction,
        NYTree::IMapNodePtr spec,
        const Stroka& authenticatedUser,
        TInstant startTime,
        EOperationState state = EOperationState::Initializing,
        bool suspended = false);

private:
    TAggregatedStatistics JobStatistics;
    TPromise<void> StartedPromise;
    TPromise<void> FinishedPromise;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
