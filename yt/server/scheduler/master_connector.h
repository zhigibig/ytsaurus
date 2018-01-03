#pragma once

#include "private.h"

#include <yt/server/controller_agent/public.h>

#include <yt/server/cell_scheduler/public.h>

#include <yt/server/chunk_server/public.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/core/actions/signal.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

//! Information retrieved during scheduler-master handshake.
struct TMasterHandshakeResult
{
    std::vector<TOperationPtr> Operations;
};

using TWatcherRequester = TCallback<void(NObjectClient::TObjectServiceProxy::TReqExecuteBatchPtr)>;
using TWatcherHandler = TCallback<void(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr)>;

DEFINE_ENUM(EMasterConnectorState,
    (Disconnected)
    (Connecting)
    (Connected)
);

//! Mediates communication between scheduler and master.
/*!
 *  \note Thread affinity: control unless noted otherwise
 */
class TMasterConnector
{
public:
    TMasterConnector(
        TSchedulerConfigPtr config,
        NCellScheduler::TBootstrap* bootstrap);
    ~TMasterConnector();

    /*!
     *  \note Thread affinity: any
     */
    void Start();

    /*!
     *  \note Thread affinity: any
     */
    EMasterConnectorState GetState() const;

    /*!
     *  \note Thread affinity: any
     */
    TInstant GetConnectionTime() const;

    IInvokerPtr GetCancelableControlInvoker() const;

    void Disconnect();

    TFuture<void> CreateOperationNode(const TOperationPtr& operation);
    TFuture<void> ResetRevivingOperationNode(const TOperationPtr& operation);
    TFuture<void> FlushOperationNode(const TOperationPtr& operation);

    void SetSchedulerAlert(ESchedulerAlertType alertType, const TError& alert);

    void AddGlobalWatcherRequester(TWatcherRequester requester);
    void AddGlobalWatcherHandler(TWatcherHandler handler);
    void AddGlobalWatcher(TWatcherRequester requester, TWatcherHandler handler, TDuration period);

    void AddOperationWatcherRequester(const TOperationPtr& operation, TWatcherRequester requester);
    void AddOperationWatcherHandler(const TOperationPtr& operation, TWatcherHandler handler);

    void UpdateConfig(const TSchedulerConfigPtr& config);

    //! Raised during connection process.
    //! Handshake result contains operations created from Cypress data; all of these have valid revival descriptors.
    //! Subscribers may throw and yield.
    DECLARE_SIGNAL(void(const TMasterHandshakeResult& result), MasterConnecting);

    //! Raised when connection is complete.
    //! Subscribers may throw but cannot yield.
    DECLARE_SIGNAL(void(), MasterConnected);

    //! Raised when disconnect happens.
    //! Subscribers may yield but cannot throw.
    DECLARE_SIGNAL(void(), MasterDisconnected);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
