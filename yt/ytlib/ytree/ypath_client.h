#pragma once

#include "common.h"
#include "ypath_service.h"

#include "../misc/ref.h"
#include "../misc/property.h"
#include "../bus/message.h"
#include "../rpc/client.h"
#include "../actions/action_util.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

class TYPathRequest;

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathRequest;

class TYPathResponse;

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathResponse;

////////////////////////////////////////////////////////////////////////////////

class TYPathRequest
    : public TRefCountedBase
{
    DEFINE_BYVAL_RO_PROPERTY(Stroka, Verb);
    DEFINE_BYVAL_RW_PROPERTY(TYPath, Path);
    DEFINE_BYREF_RW_PROPERTY(yvector<TSharedRef>, Attachments);

public:
    typedef TIntrusivePtr<TYPathRequest> TPtr;
    
    TYPathRequest(const Stroka& verb);

    NBus::IMessage::TPtr Serialize();

protected:
    virtual bool SerializeBody(TBlob* data) const = 0;

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathRequest
    : public TYPathRequest
    , public TRequestMessage
{
public:
    typedef TTypedYPathResponse<TRequestMessage, TResponseMessage> TTypedResponse;
    typedef TIntrusivePtr< TTypedYPathRequest<TRequestMessage, TResponseMessage> > TPtr;

    TTypedYPathRequest(const Stroka& verb)
        : TYPathRequest(verb)
    { }

protected:
    virtual bool SerializeBody(TBlob* data) const
    {
        return SerializeProtobuf(this, data);
    }
};

////////////////////////////////////////////////////////////////////////////////

class TYPathResponse
    : public TRefCountedBase
{
    DEFINE_BYREF_RW_PROPERTY(yvector<TSharedRef>, Attachments);
    DEFINE_BYVAL_RW_PROPERTY(NRpc::TError, Error);

public:
    typedef TIntrusivePtr<TYPathResponse> TPtr;

    void Deserialize(NBus::IMessage* message);

    NRpc::EErrorCode GetErrorCode() const;
    bool IsOK() const;

    void ThrowIfError() const;

protected:
    virtual bool DeserializeBody(TRef data) = 0;

};

////////////////////////////////////////////////////////////////////////////////

template <class TRequestMessage, class TResponseMessage>
class TTypedYPathResponse
    : public TYPathResponse
    , public TResponseMessage
{
public:
    typedef TIntrusivePtr< TTypedYPathResponse<TRequestMessage, TResponseMessage> > TPtr;

protected:
    virtual bool DeserializeBody(TRef data)
    {
        return DeserializeProtobuf(this, data);
    }

};

////////////////////////////////////////////////////////////////////////////////

#define YPATH_PROXY_METHOD(ns, method) \
    typedef ::NYT::NYTree::TTypedYPathRequest<ns::TReq##method, ns::TRsp##method> TReq##method; \
    typedef ::NYT::NYTree::TTypedYPathResponse<ns::TReq##method, ns::TRsp##method> TRsp##method; \
    \
    static TReq##method::TPtr method() \
    { \
        return New<TReq##method>(#method); \
    }

////////////////////////////////////////////////////////////////////////////////

//! Executes a YPath verb against a local service.
template <class TTypedRequest>
TIntrusivePtr< TFuture< TIntrusivePtr<typename TTypedRequest::TTypedResponse> > >
ExecuteVerb(
    IYPathService* rootService,
    TYPath path,
    TTypedRequest* request,
    IYPathExecutor* executor = ~GetDefaultExecutor());

//! Executes "Get" verb synchronously. Throws if an error has occurred.
TYson SyncExecuteYPathGet(IYPathService* rootService, TYPath path);

//! Executes "Set" verb synchronously. Throws if an error has occurred.
void SyncExecuteYPathSet(IYPathService* rootService, TYPath path, const TYson& value);

//! Executes "Remove" verb synchronously. Throws if an error has occurred.
void SyncExecuteYPathRemove(IYPathService* rootService, TYPath path);

//! Executes "List" verb synchronously. Throws if an error has occurred.
yvector<Stroka> SyncExecuteYPathList(IYPathService* rootService, TYPath path);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT


// TODO: move to ypath_client-inl.h

#include "ypath_detail.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

template <class TTypedRequest, class TTypedResponse>
void OnYPathResponse(
    const TYPathResponseHandlerParam& param,
    TIntrusivePtr< TFuture< TIntrusivePtr<TTypedResponse> > > asyncResponse,
    const Stroka& verb,
    TYPath resolvedPath)
{
    auto response = New<TTypedResponse>();
    response->Deserialize(~param.Message);
    if (!response->IsOK()) {
        auto error = response->GetError();
        Stroka message = Sprintf("Error executing YPath operation (Verb: %s, ResolvedPath: %s)\n%s",
            ~verb,
            ~resolvedPath,
            ~error.GetMessage());
        response->SetError(NRpc::TError(error.GetCode(), message));
    }
    asyncResponse->Set(response);
}

template <class TTypedRequest>
TIntrusivePtr< TFuture< TIntrusivePtr<typename TTypedRequest::TTypedResponse> > >
ExecuteVerb(
    IYPathService* rootService,
    TYPath path,
    TTypedRequest* request,
    IYPathExecutor* executor)
{
    Stroka verb = request->GetVerb();

    IYPathService::TPtr suffixService;
    TYPath suffixPath;
    ResolveYPath(rootService, path, verb, &suffixService, &suffixPath);

    request->SetPath(suffixPath);

    auto requestMessage = request->Serialize();
    auto asyncResponse = New< TFuture< TIntrusivePtr<typename TTypedRequest::TTypedResponse> > >();

    auto context = CreateYPathContext(
        ~requestMessage,
        suffixPath,
        verb,
        YTreeLogger.GetCategory(),
        ~FromMethod(
            &OnYPathResponse<TTypedRequest, typename TTypedRequest::TTypedResponse>,
            asyncResponse,
            verb,
            ComputeResolvedYPath(path, suffixPath)));

    try {
        executor->ExecuteVerb(~suffixService, ~context);
    } catch (const NRpc::TServiceException& ex) {
        context->Reply(NRpc::TError(
            EYPathErrorCode(EYPathErrorCode::GenericError),
            ex.what()));
    }

    return asyncResponse;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
