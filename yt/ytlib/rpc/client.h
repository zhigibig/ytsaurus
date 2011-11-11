#pragma once

#include "channel.h"

#include "../bus/bus_client.h"
#include "../actions/future.h"
#include "../misc/delayed_invoker.h"
#include "../misc/metric.h"

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

class TClientRequest;

template<
    class TRequestMessage,
    class TResponseMessage
>
class TTypedClientRequest;

class TClientResponse;

template<
    class TRequestMessage,
    class TResponseMessage
>
class TTypedClientResponse;

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

    TProxyBase(IChannel::TPtr channel, const Stroka& serviceName);

    IChannel::TPtr Channel;
    Stroka ServiceName;
};          

////////////////////////////////////////////////////////////////////////////////

struct IClientRequest
    : virtual public TRefCountedBase
{
    typedef TIntrusivePtr<IClientRequest> TPtr;

    virtual NBus::IMessage::TPtr Serialize() const = 0;

    virtual TRequestId GetRequestId() const = 0;
    virtual Stroka GetServiceName() const = 0;
    virtual Stroka GetMethodName() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TClientRequest
    : public IClientRequest
{
public:
    typedef TIntrusivePtr<TClientRequest> TPtr;

    TRequestId GetRequestId() const;

    yvector<TSharedRef>& Attachments();

protected:
    IChannel::TPtr Channel;

    TClientRequest(
        IChannel::TPtr channel,
        const Stroka& serviceName,
        const Stroka& methodName);

    virtual bool SerializeBody(TBlob* data) const = 0;
    TFuture<TError>::TPtr DoInvoke(TIntrusivePtr<TClientResponse> response, TDuration timeout);

    Stroka GetServiceName() const;
    Stroka GetMethodName() const;

private:
    Stroka ServiceName;
    Stroka MethodName;
    TRequestId RequestId;

    yvector<TSharedRef> Attachments_;

    NBus::IMessage::TPtr Serialize() const;
};

////////////////////////////////////////////////////////////////////////////////

template<
    class TRequestMessage,
    class TResponseMessage
>
class TTypedClientRequest
    : public TClientRequest
    , public TRequestMessage
{
private:
    typedef TTypedClientResponse<TRequestMessage, TResponseMessage> TTypedResponse;

public:
    typedef TIntrusivePtr<TTypedClientRequest> TPtr;
    typedef TFuture<typename TTypedResponse::TPtr> TInvokeResult;

    TTypedClientRequest(
        IChannel::TPtr channel,
        const Stroka& serviceName,
        const Stroka& methodName)
        : TClientRequest(channel, serviceName, methodName)
    {
        YASSERT(~channel != NULL);
    }

    typename TInvokeResult::TPtr Invoke(TDuration timeout = TDuration::Zero())
    {
        typename TInvokeResult::TPtr asyncResult = NYT::New<TInvokeResult>();
        typename TTypedResponse::TPtr response = NYT::New<TTypedResponse>(
            GetRequestId(),
            Channel);
        DoInvoke(~response, timeout)->Subscribe(FromMethod(
            &TTypedClientRequest::OnReady,
            asyncResult,
            response));
        return asyncResult;
    }

private:
    virtual bool SerializeBody(TBlob* data) const
    {
        return SerializeMessage(this, data);
    }

    static void OnReady(
        TError error,
        typename TInvokeResult::TPtr asyncResult,
        typename TTypedResponse::TPtr response)
    {
        YASSERT(~asyncResult != NULL);
        YASSERT(~response != NULL);

        UNUSED(error);
        asyncResult->Set(response);
    }
};

////////////////////////////////////////////////////////////////////////////////

struct IClientResponseHandler
    : virtual TRefCountedBase
{
    typedef TIntrusivePtr<IClientResponseHandler> TPtr;

    virtual void OnAcknowledgement(
        NBus::IBus::ESendResult sendResult) = 0;

    virtual void OnResponse(
        const TError& error,
        NBus::IMessage::TPtr message) = 0;

    virtual void OnTimeout() = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TClientResponse
    : public IClientResponseHandler
{
public:
    typedef TIntrusivePtr<TClientResponse> TPtr;

    yvector<TSharedRef>& Attachments();

    TRequestId GetRequestId() const;
    TError GetError() const;
    EErrorCode GetErrorCode() const;

    bool IsOK() const;

    TInstant GetStartTime() const;

protected:
    TClientResponse(
        const TRequestId& requestId,
        IChannel::TPtr channel);

    virtual bool DeserializeBody(TRef data) = 0;

private:
    friend class TClientRequest;

    DECLARE_ENUM(EState,
        (Sent)
        (Ack)
        (Done)
    );

    // Protects state.
    TSpinLock SpinLock;
    TRequestId RequestId;
    IChannel::TPtr Channel;
    EState State;
    TError Error;
    yvector<TSharedRef> MyAttachments;
    TInstant StartTime;

    // IClientResponseHandler implementation.
    virtual void OnAcknowledgement(NBus::IBus::ESendResult sendResult);
    virtual void OnResponse(const TError& error, NBus::IMessage::TPtr message);
    virtual void OnTimeout();

    void Deserialize(NBus::IMessage::TPtr message);
    void Complete(const TError& error);
};

////////////////////////////////////////////////////////////////////////////////

template<
    class TRequestMessage,
    class TResponseMessage
>
class TTypedClientResponse
    : public TClientResponse
    , public TResponseMessage
{
public:
    typedef TIntrusivePtr<TTypedClientResponse> TPtr;

    TTypedClientResponse(
        const TRequestId& requestId,
        IChannel::TPtr channel)
        : TClientResponse(
            requestId,
            channel)
    {
        YASSERT(~channel != NULL);
    }

private:
    typename TFuture<TPtr>::TPtr AsyncResult;

    virtual bool DeserializeBody(TRef data)
    {
        return DeserializeMessage(this, data);
    }
};

////////////////////////////////////////////////////////////////////////////////

#define RPC_DECLARE_PROXY(serviceName, errorCodes) \
    static Stroka GetServiceName() \
    { \
        return PP_STRINGIZE(serviceName); \
    } \
    \
    DECLARE_POLY_ENUM2(E##serviceName##Error, NRpc::EErrorCode, \
        errorCodes \
    ); \
    \
    typedef E##serviceName##Error EErrorCode;


////////////////////////////////////////////////////////////////////////////////

#define RPC_PROXY_METHOD(ns, method) \
    typedef ::NYT::NRpc::TTypedClientRequest<ns::TReq##method, ns::TRsp##method> TReq##method; \
    typedef ::NYT::NRpc::TTypedClientResponse<ns::TReq##method, ns::TRsp##method> TRsp##method; \
    typedef ::NYT::TFuture<TRsp##method::TPtr> TInv##method; \
    \
    TReq##method::TPtr method() \
    { \
        return New<TReq##method>(Channel, ServiceName, #method); \
    }

////////////////////////////////////////////////////////////////////////////////

#define USE_RPC_PROXY_METHOD(TProxy, MethodName) \
    typedef TProxy::TReq##MethodName TReq##MethodName; \
    typedef TProxy::TRsp##MethodName TRsp##MethodName; \
    typedef TProxy::TInv##MethodName TInv##MethodName;

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
