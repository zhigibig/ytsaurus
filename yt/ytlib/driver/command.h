#pragma once

#include "private.h"
#include "public.h"
#include "driver.h"

#include <ytlib/misc/error.h>
#include <ytlib/misc/mpl.h>

#include <ytlib/ytree/public.h>
#include <ytlib/ytree/yson_serializable.h>
#include <ytlib/ytree/convert.h>

#include <ytlib/yson/consumer.h>
#include <ytlib/yson/parser.h>
#include <ytlib/yson/writer.h>

#include <ytlib/rpc/public.h>
#include <ytlib/rpc/channel.h>

#include <ytlib/chunk_client/public.h>

#include <ytlib/meta_state/public.h>
#include <ytlib/meta_state/rpc_helpers.h>

#include <ytlib/transaction_client/transaction.h>
#include <ytlib/transaction_client/transaction_manager.h>

#include <ytlib/security_client/public.h>

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/scheduler/scheduler_service_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

struct TRequest
    : public virtual TYsonSerializable
{
    TRequest()
    {
        SetKeepOptions(true);
    }
};

typedef TIntrusivePtr<TRequest> TRequestPtr;

////////////////////////////////////////////////////////////////////////////////

struct TTransactionalRequest
    : public virtual TRequest
{
    NObjectClient::TTransactionId TransactionId;
    bool PingAncestors;

    TTransactionalRequest()
    {
        RegisterParameter("transaction_id", TransactionId)
            .Default(NObjectClient::NullTransactionId);
        RegisterParameter("ping_ancestor_transactions", PingAncestors)
            .Default(false);
    }
};

typedef TIntrusivePtr<TTransactionalRequest> TTransactionalRequestPtr;

////////////////////////////////////////////////////////////////////////////////

struct TMutatingRequest
    : public virtual TRequest
{
    NMetaState::TMutationId MutationId;

    TMutatingRequest()
    {
        RegisterParameter("mutation_id", MutationId)
            .Default(NMetaState::NullMutationId);
    }
};

typedef TIntrusivePtr<TMutatingRequest> TMutatingRequestPtr;

////////////////////////////////////////////////////////////////////////////////

struct ICommandContext
    : public TRefCounted
{
    virtual ~ICommandContext()
    { }

    virtual TDriverConfigPtr GetConfig() = 0;
    virtual NRpc::IChannelPtr GetMasterChannel() = 0;
    virtual NRpc::IChannelPtr GetSchedulerChannel() = 0;
    virtual NChunkClient::IBlockCachePtr GetBlockCache() = 0;
    virtual NTransactionClient::TTransactionManagerPtr GetTransactionManager() = 0;

    virtual const TDriverRequest* GetRequest() = 0;
    virtual void SetResponse(const TDriverResponse& response) = 0;

    virtual const NFormats::TFormat& GetInputFormat() = 0;
    virtual const NFormats::TFormat& GetOutputFormat() = 0;

    virtual NYTree::TYsonProducer CreateInputProducer() = 0;
    virtual std::unique_ptr<NYson::IYsonConsumer> CreateOutputConsumer() = 0;
};

typedef TIntrusivePtr<ICommandContext> ICommandContextPtr;

////////////////////////////////////////////////////////////////////////////////

struct ICommand
    : public TRefCounted
{
    virtual void Execute(ICommandContextPtr context) = 0;
};

typedef TIntrusivePtr<ICommand> ICommandPtr;

////////////////////////////////////////////////////////////////////////////////

class TCommandBase
    : public ICommand
{
protected:
    ICommandContextPtr Context;
    bool Replied;

    std::unique_ptr<NObjectClient::TObjectServiceProxy> ObjectProxy;
    std::unique_ptr<NScheduler::TSchedulerServiceProxy> SchedulerProxy;

    TCommandBase();

    void Prepare();

    void ReplyError(const TError& error);
    void ReplySuccess(const NYTree::TYsonString& yson);
    void ReplySuccess();

    template <class TResponse>
    void CheckAndReply(
        TFuture<TResponse> future,
        TCallback<NYTree::TYsonString(TResponse)> toYsonString
            = TCallback<NYTree::TYsonString(TResponse)>())
    {
        future.Apply(BIND(&TCommandBase::OnProxyResponse<TResponse>, MakeStrong(this), toYsonString));
    }
    
    void CheckAndReply(TAsyncError future)
    {
        auto this_ = MakeStrong(this);
        future.Apply(BIND([this, this_] (TError error) {
            if (!error.IsOK()) {
                ReplyError(error);
            } else {
                ReplySuccess();
            }
        }));
    }

    template <class TResponse>
    void OnProxyResponse(
        TCallback<NYTree::TYsonString(TResponse)> extractResult,
        TResponse response)
    {
        if (!response->IsOK()) {
            ReplyError(*response);
        } else if (!extractResult.IsNull()) {
            ReplySuccess(extractResult.Run(response));
        } else {
            ReplySuccess();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class TRequest>
class TTypedCommandBase
    : public virtual TCommandBase
{
public:
    virtual void Execute(ICommandContextPtr context)
    {
        Context = context;
        try {
            ParseRequest();
            Prepare();
            DoExecute();
        } catch (const std::exception& ex) {
            ReplyError(ex);
        }
    }

protected:
    TIntrusivePtr<TRequest> Request;

    virtual void DoExecute() = 0;

private:
    void ParseRequest()
    {
        Request = New<TRequest>();
        try {
            auto arguments = Context->GetRequest()->Arguments;;
            Request = NYTree::ConvertTo<TIntrusivePtr<TRequest>>(arguments);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error parsing command arguments") << ex;
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class TRequest, class = void>
class TTransactionalCommandBase
{ };

DECLARE_ENUM(EAllowNullTransaction,
    (Yes)
    (No)
);

DECLARE_ENUM(EPingTransaction,
    (Yes)
    (No)
);

template <class TRequest>
class TTransactionalCommandBase<
    TRequest,
    typename NMpl::TEnableIf<NMpl::TIsConvertible<TRequest&, TTransactionalRequest&> >::TType
>
    : public virtual TTypedCommandBase<TRequest>
{
protected:
    NTransactionClient::TTransactionId GetTransactionId(EAllowNullTransaction allowNullTransaction)
    {
        auto transaction = this->GetTransaction(allowNullTransaction, EPingTransaction::Yes);
        return transaction ? transaction->GetId() : NTransactionClient::NullTransactionId;
    }

    NTransactionClient::ITransactionPtr GetTransaction(EAllowNullTransaction allowNullTransaction, EPingTransaction pingTransaction)
    {
        if (allowNullTransaction == EAllowNullTransaction::No &&
            this->Request->TransactionId == NTransactionClient::NullTransactionId)
        {
            THROW_ERROR_EXCEPTION("Transaction is required");
        }

        auto transactionId = this->Request->TransactionId;
        if (transactionId == NTransactionClient::NullTransactionId) {
            return nullptr;
        }

        NTransactionClient::TTransactionAttachOptions options(transactionId);
        options.AutoAbort = false;
        options.Ping = (pingTransaction == EPingTransaction::Yes);
        options.PingAncestors = this->Request->PingAncestors;

        auto transactionManager = this->Context->GetTransactionManager();
        return transactionManager->Attach(options);
    }

    void SetTransactionId(NRpc::IClientRequestPtr request, EAllowNullTransaction allowNullTransaction)
    {
        NCypressClient::SetTransactionId(request, this->GetTransactionId(allowNullTransaction));
    }

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequest, class = void>
class TMutatingCommandBase
{ };

template <class TRequest>
class TMutatingCommandBase <
    TRequest,
    typename NMpl::TEnableIf<NMpl::TIsConvertible<TRequest&, TMutatingRequest&> >::TType
>
    : public virtual TTypedCommandBase<TRequest>
{
protected:
    NMetaState::TMutationId GenerateMutationId()
    {
        if (!this->CurrentMutationId) {
            this->CurrentMutationId = this->Request->MutationId;
        }

        auto result = *this->CurrentMutationId;
        ++(*this->CurrentMutationId).Parts[0];
        return result;
    }

    void GenerateMutationId(NRpc::IClientRequestPtr request)
    {
        NMetaState::SetMutationId(request, this->GenerateMutationId());
    }

private:
    TNullable<NMetaState::TMutationId> CurrentMutationId;

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequest>
class TTypedCommand
    : public virtual TTypedCommandBase<TRequest>
    , public TTransactionalCommandBase<TRequest>
    , public TMutatingCommandBase<TRequest>
{ };

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

