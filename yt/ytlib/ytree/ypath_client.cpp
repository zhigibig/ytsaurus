#include "stdafx.h"
#include "ypath_client.h"
#include "ypath_rpc.h"
#include "rpc.pb.h"

#include "../misc/serialize.h"
#include "../rpc/message.h"

namespace NYT {
namespace NYTree {

using namespace NBus;
using namespace NRpc;
using namespace NRpc::NProto;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = YTreeLogger;

////////////////////////////////////////////////////////////////////////////////

TYPathRequest::TYPathRequest(const Stroka& verb)
    : Verb_(verb)
{ }

IMessage::TPtr TYPathRequest::Serialize()
{
    // Serialize body.
    TBlob bodyData;
    if (!SerializeBody(&bodyData)) {
        LOG_FATAL("Error serializing request body");
    }

    // Compose message.
    return CreateRequestMessage(
        TRequestId(),
        Path_,
        Verb_,
        MoveRV(bodyData),
        Attachments_);
}

////////////////////////////////////////////////////////////////////////////////

void TYPathResponse::Deserialize(NBus::IMessage* message)
{
    YASSERT(message != NULL);

    const auto& parts = message->GetParts();
    YASSERT(parts.ysize() >= 1);

    // Deserialize RPC header.
    TResponseHeader header;
    if (!DeserializeProtobuf(&header, parts[0])) {
        LOG_FATAL("Error deserializing response header");
    }

    Error_ = TError(header.GetErrorCode(), header.GetErrorMessage());

    if (Error_.IsOK()) {
        // Deserialize body.
        DeserializeBody(parts[1]);

        // Load attachments.
        Attachments_.clear();
        std::copy(
            parts.begin() + 2,
            parts.end(),
            std::back_inserter(Attachments_));
    }
}

int TYPathResponse::GetErrorCode() const
{
    return Error_.GetCode();
}

bool TYPathResponse::IsOK() const
{
    return Error_.IsOK();
}

void TYPathResponse::ThrowIfError() const
{
    if (!IsOK()) {
        ythrow yexception() << Error_.ToString();
    }
}

////////////////////////////////////////////////////////////////////////////////

TYson SyncExecuteYPathGet(IYPathService* rootService, TYPath path)
{
    auto request = TYPathProxy::Get();
    auto response = ExecuteVerb(rootService, path, ~request)->Get();
    response->ThrowIfError();
    return response->GetValue();
}

void SyncExecuteYPathSet(IYPathService* rootService, TYPath path, const TYson& value)
{
    auto request = TYPathProxy::Set();
    request->SetValue(value);
    auto response = ExecuteVerb(rootService, path, ~request)->Get();
    response->ThrowIfError();
}

void SyncExecuteYPathRemove(IYPathService* rootService, TYPath path)
{
    auto request = TYPathProxy::Remove();
    auto response = ExecuteVerb(rootService, path, ~request)->Get();
    response->ThrowIfError();
}

yvector<Stroka> SyncExecuteYPathList(IYPathService* rootService, TYPath path)
{
    auto request = TYPathProxy::List();
    auto response = ExecuteVerb(rootService, path, ~request)->Get();
    response->ThrowIfError();
    return FromProto<Stroka>(response->GetKeys());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
