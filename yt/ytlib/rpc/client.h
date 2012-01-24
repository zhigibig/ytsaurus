#pragma once

#include "channel.h"

#include <ytlib/misc/property.h>
#include <ytlib/misc/delayed_invoker.h>
#include <ytlib/misc/metric.h>
#include <ytlib/misc/serialize.h>
#include <ytlib/bus/client.h>
#include <ytlib/actions/future.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

class TClientRequest;

template <class TRequestMessage, class TResponse>
class TTypedClientRequest;

class TClientResponse;

template <class TResponseMessage>
class TTypedClientResponse;

class TOneWayClientResponse;

////////////////////////////////////////////////////////////////////////////////

class TProxyBase
    : public TNonCopyable
{
protected:
    //! Service error type.
    /*!
     * Defines a basic type of error code for all proxies.
     * A derived proxy type may hide this definition by introducing
     * an appropriate descendant of NRpc::EErrorCode.
     */
    typedef NRpc::EErrorCode EErrorCode;

    TProxyBase(IChannel* channel, const Stroka& serviceName);

    DEFINE_BYVAL_RW_PROPERTY(TDuration, Timeout);

    IChannel::TPtr Channel;
    Stroka ServiceName;
};          

////////////////////////////////////////////////////////////////////////////////

struct IClientRequest
    : virtual public TRefCounted
{
    typedef TIntrusivePtr<IClientRequest> TPtr;

    virtual NBus::IMessage::TPtr Serialize() const = 0;

    virtual const TRequestId& GetRequestId() const = 0;
    virtual const Stroka& GetPath() const = 0;
    virtual const Stroka& GetVerb() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TClientRequest
    : public IClientRequest
{
    DEFINE_BYREF_RW_PROPERTY(yvector<TSharedRef>, Attachments);
    DEFINE_BYVAL_RO_PROPERTY(bool, OneWay);
    DEFINE_BYVAL_RW_PROPERTY(TDuration, Timeout);

public:
    typedef TIntrusivePtr<TClientRequest> TPtr;

    NBus::IMessage::TPtr Serialize() const;

    const Stroka& GetPath() const;
    const Stroka& GetVerb() const;

    const TRequestId& GetRequestId() const;

protected:
    IChannel::TPtr Channel;
    Stroka Path;
    Stroka Verb;
    TRequestId RequestId;

    TClientRequest(
        IChannel* channel,
        const Stroka& path,
        const Stroka& verb,
        bool oneWay);

    virtual TBlob SerializeBody() const = 0;

    void DoInvoke(IClientResponseHandler* responseHandler, TDuration timeout);

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequestMessage, class TResponse>
class TTypedClientRequest
    : public TClientRequest
    , public TRequestMessage
{
public:
    typedef TIntrusivePtr<TTypedClientRequest> TPtr;

    TTypedClientRequest(
        IChannel* channel,
        const Stroka& path,
        const Stroka& verb,
        bool oneWay)
        : TClientRequest(channel, path, verb, oneWay)
    { }

    typename TFuture< TIntrusivePtr<TResponse> >::TPtr Invoke()
    {
        auto response = NYT::New<TResponse>(GetRequestId());
        auto asyncResult = response->GetAsyncResult();
        DoInvoke(~response, Timeout_);
        return asyncResult;
    }

    // Override base method for fluent use.
    TIntrusivePtr<TTypedClientRequest> SetTimeout(TDuration timeout)
    {
        TClientRequest::SetTimeout(timeout);
        return this;
    }

private:
    virtual TBlob SerializeBody() const
    {
        NLog::TLogger& Logger = RpcLogger;
        TBlob blob;
        if (!SerializeProtobuf(this, &blob)) {
            LOG_FATAL("Error serializing request body");
        }
        return blob;
    }

};

////////////////////////////////////////////////////////////////////////////////

//! Handles response for an RPC request.
struct IClientResponseHandler
    : virtual TRefCounted
{
    typedef TIntrusivePtr<IClientResponseHandler> TPtr;

    //! Request delivery has been acknowledged.
    virtual void OnAcknowledgement() = 0;
    //! The request has been replied with #EErrorCode::OK.
    /*!
     *  \param message A message containing the response.
     */
    virtual void OnResponse(NBus::IMessage* message) = 0;
    //! The request has failed.
    /*!
     *  \param error An error that has occurred.
     */
    virtual void OnError(const TError& error) = 0;
};

////////////////////////////////////////////////////////////////////////////////

//! Provides a common base for both one-way and two-way responses.
class TClientResponseBase
    : public IClientResponseHandler
{
    DEFINE_BYVAL_RO_PROPERTY(TRequestId, RequestId);
    DEFINE_BYVAL_RO_PROPERTY(TError, Error);
    DEFINE_BYVAL_RO_PROPERTY(TInstant, StartTime);

public:
    typedef TIntrusivePtr<TClientResponseBase> TPtr;

    int GetErrorCode() const;
    bool IsOK() const;

protected:
    TClientResponseBase(const TRequestId& requestId);

    virtual void FireCompleted() = 0;

    DECLARE_ENUM(EState,
        (Sent)
        (Ack)
        (Done)
    );

    // Protects state.
    TSpinLock SpinLock;
    EState State;

    // IClientResponseHandler implementation.
    virtual void OnError(const TError& error);

};

////////////////////////////////////////////////////////////////////////////////

//! Describes a two-way response.
class TClientResponse
    : public TClientResponseBase
{
    DEFINE_BYREF_RW_PROPERTY(yvector<TSharedRef>, Attachments);

public:
    typedef TIntrusivePtr<TClientResponse> TPtr;

    NBus::IMessage::TPtr GetResponseMessage() const;

protected:
    TClientResponse(const TRequestId& requestId);

    virtual void DeserializeBody(const TRef& data) = 0;

private:
    // Protected by #SpinLock.
    NBus::IMessage::TPtr ResponseMessage;

    // IClientResponseHandler implementation.
    virtual void OnAcknowledgement();
    virtual void OnResponse(NBus::IMessage* message);

    void Deserialize(NBus::IMessage* responseMessage);

};

////////////////////////////////////////////////////////////////////////////////

template <class TResponseMessage>
class TTypedClientResponse
    : public TClientResponse
    , public TResponseMessage
{
public:
    typedef TIntrusivePtr<TTypedClientResponse> TPtr;

    TTypedClientResponse(const TRequestId& requestId)
        : TClientResponse(requestId)
        , AsyncResult(NYT::New< TFuture<TPtr> >())
    { }

    typename TFuture<TPtr>::TPtr GetAsyncResult()
    {
        return AsyncResult;
    }

private:
    typename TFuture<TPtr>::TPtr AsyncResult;

    virtual void FireCompleted()
    {
        AsyncResult->Set(this);
        AsyncResult.Reset();
    }

    virtual void DeserializeBody(const TRef& data)
    {
        NLog::TLogger& Logger = RpcLogger;
        if (!DeserializeProtobuf(this, data)) {
            LOG_FATAL("Error deserializing response body");
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

//! Describes a one-way response.
class TOneWayClientResponse
    : public TClientResponseBase
{
public:
    typedef TIntrusivePtr<TOneWayClientResponse> TPtr;

    TOneWayClientResponse(const TRequestId& requestId);

    TFuture<TPtr>::TPtr GetAsyncResult();

private:
    TFuture<TPtr>::TPtr AsyncResult;

    // IClientResponseHandler implementation.
    virtual void OnAcknowledgement();
    virtual void OnResponse(NBus::IMessage* message);

    virtual void FireCompleted();

};

////////////////////////////////////////////////////////////////////////////////

#define DEFINE_RPC_PROXY_METHOD(ns, method) \
    typedef ::NYT::NRpc::TTypedClientResponse<ns::TRsp##method> TRsp##method; \
    typedef ::NYT::NRpc::TTypedClientRequest<ns::TReq##method, TRsp##method> TReq##method; \
    typedef ::NYT::TFuture<TRsp##method::TPtr> TInv##method; \
    \
    TReq##method::TPtr method() \
    { \
        return \
            New<TReq##method>(~Channel, ServiceName, #method, false) \
            ->SetTimeout(Timeout_); \
    }

////////////////////////////////////////////////////////////////////////////////

#define DEFINE_ONE_WAY_RPC_PROXY_METHOD(ns, method) \
    typedef ::NYT::NRpc::TOneWayClientResponse TRsp##method; \
    typedef ::NYT::NRpc::TTypedClientRequest<ns::TReq##method, TRsp##method> TReq##method; \
    typedef ::NYT::TFuture<TRsp##method::TPtr> TInv##method; \
    \
    TReq##method::TPtr method() \
    { \
        return \
            New<TReq##method>(~Channel, ServiceName, #method, true) \
            ->SetTimeout(Timeout_); \
    }

////////////////////////////////////////////////////////////////////////////////
} // namespace NRpc
} // namespace NYT
