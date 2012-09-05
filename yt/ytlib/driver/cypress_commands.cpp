#include "stdafx.h"
#include "cypress_commands.h"

#include <ytlib/object_client/object_service_proxy.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/ytree/ypath_proxy.h>

#include <ytlib/meta_state/rpc_helpers.h>

namespace NYT {
namespace NDriver {

using namespace NYTree;
using namespace NCypressClient;
using namespace NObjectClient;

////////////////////////////////////////////////////////////////////////////////

void TGetCommand::DoExecute()
{
    TObjectServiceProxy proxy(Context->GetMasterChannel());
    auto req = TYPathProxy::Get(WithTransaction(
        Request->Path.GetPath(),
        GetTransactionId(false)));

    ToProto(req->mutable_attributes(), Request->Attributes);

    req->Attributes().MergeFrom(Request->GetOptions());
    auto rsp = proxy.Execute(req).Get();

    if (!rsp->IsOK()) {
        ReplyError(rsp->GetError());
        return;
    }

    ReplySuccess(TYsonString(rsp->value()));
}

////////////////////////////////////////////////////////////////////////////////

void TSetCommand::DoExecute()
{
    TObjectServiceProxy proxy(Context->GetMasterChannel());
    auto req = TYPathProxy::Set(WithTransaction(
        Request->Path.GetPath(),
        GetTransactionId(false)));
    NMetaState::GenerateRpcMutationId(req);
    
    auto producer = Context->CreateInputProducer();
    TYsonString value = ConvertToYsonString(producer);
    req->set_value(value.Data());

    req->Attributes().MergeFrom(Request->GetOptions());
    auto rsp = proxy.Execute(req).Get();

    if (!rsp->IsOK()) {
        ReplyError(rsp->GetError());
    }
}

////////////////////////////////////////////////////////////////////////////////

void TRemoveCommand::DoExecute()
{
    TObjectServiceProxy proxy(Context->GetMasterChannel());
    auto req = TYPathProxy::Remove(WithTransaction(
        Request->Path.GetPath(),
        GetTransactionId(false)));
    NMetaState::GenerateRpcMutationId(req);

    req->Attributes().MergeFrom(Request->GetOptions());
    auto rsp = proxy.Execute(req).Get();

    if (!rsp->IsOK()) {
        ReplyError(rsp->GetError());
    }
}

//////////////////////////////////////////////////////////////////////////////////

void TListCommand::DoExecute()
{
    TObjectServiceProxy proxy(Context->GetMasterChannel());
    auto req = TYPathProxy::List(WithTransaction(
        Request->Path.GetPath(),
        GetTransactionId(false)));

    ToProto(req->mutable_attributes(), Request->Attributes);

    req->Attributes().MergeFrom(Request->GetOptions());
    auto rsp = proxy.Execute(req).Get();

    if (!rsp->IsOK()) {
        ReplyError(rsp->GetError());
        return;
    }

    ReplySuccess(TYsonString(rsp->keys()));
}

//////////////////////////////////////////////////////////////////////////////////

void TCreateCommand::DoExecute()
{
    TObjectServiceProxy proxy(Context->GetMasterChannel());
    auto req = TCypressYPathProxy::Create(WithTransaction(
        Request->Path.GetPath(),
        GetTransactionId(false)));
    NMetaState::GenerateRpcMutationId(req);

    req->set_type(Request->Type);

    req->Attributes().MergeFrom(Request->GetOptions());
    auto rsp = proxy.Execute(req).Get();

    if (!rsp->IsOK()) {
        ReplyError(rsp->GetError());
        return;
    }

    auto consumer = Context->CreateOutputConsumer();
    auto nodeId = TNodeId::FromProto(rsp->object_id());
    BuildYsonFluently(~consumer)
        .Scalar(nodeId.ToString());
}

//////////////////////////////////////////////////////////////////////////////////

void TLockCommand::DoExecute()
{
    TObjectServiceProxy proxy(Context->GetMasterChannel());
    auto req = TCypressYPathProxy::Lock(WithTransaction(
        Request->Path.GetPath(),
        GetTransactionId(true)));
    NMetaState::GenerateRpcMutationId(req);

    req->set_mode(Request->Mode);

    req->Attributes().MergeFrom(Request->GetOptions());
    auto rsp = proxy.Execute(req).Get();

    if (!rsp->IsOK()) {
        ReplyError(rsp->GetError());
    }
}

////////////////////////////////////////////////////////////////////////////////

void TCopyCommand::DoExecute()
{
    TObjectServiceProxy proxy(Context->GetMasterChannel());
    auto req = TCypressYPathProxy::Copy(WithTransaction(
        Request->DestinationPath.GetPath(),
        GetTransactionId(false)));
    NMetaState::GenerateRpcMutationId(req);
    req->set_source_path(Request->SourcePath.GetPath());

    auto rsp = proxy.Execute(req).Get();

    if (!rsp->IsOK()) {
        ReplyError(rsp->GetError());
        return;
    }

    auto consumer = Context->CreateOutputConsumer();
    auto nodeId = TNodeId::FromProto(rsp->object_id());
    BuildYsonFluently(~consumer)
        .Scalar(nodeId.ToString());
}

////////////////////////////////////////////////////////////////////////////////

void TMoveCommand::DoExecute()
{
    TObjectServiceProxy proxy(Context->GetMasterChannel());
    {
        auto req = TCypressYPathProxy::Copy(WithTransaction(
            Request->DestinationPath.GetPath(),
            GetTransactionId(false)));
        NMetaState::GenerateRpcMutationId(req);
        req->set_source_path(Request->SourcePath.GetPath());
        auto rsp = proxy.Execute(req).Get();

        if (!rsp->IsOK()) {
            ReplyError(rsp->GetError());
            return;
        }
    }

    {
        auto req = TYPathProxy::Remove(WithTransaction(
            Request->SourcePath.GetPath(),
            GetTransactionId(false)));
        NMetaState::GenerateRpcMutationId(req);

        auto rsp = proxy.Execute(req).Get();
        if (!rsp->IsOK()) {
            ReplyError(rsp->GetError());
            return;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
