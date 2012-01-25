#pragma once

#include "command.h"

#include <ytlib/ytree/ytree.h>
#include <ytlib/object_server/id.h>

namespace NYT {
namespace NDriver {
    
////////////////////////////////////////////////////////////////////////////////

struct TGetRequest
    : public TRequestBase
{
    NYTree::TYPath Path;
    NYTree::INode::TPtr Stream;

    TGetRequest()
    {
        Register("path", Path);
        Register("stream", Stream).Default(NULL).CheckThat(~StreamSpecIsValid);
    }
};

class TGetCommand
    : public TCommandBase<TGetRequest>
{
public:
    TGetCommand(IDriverImpl* driverImpl)
        : TCommandBase(driverImpl)
    { }

private:
    virtual void DoExecute(TGetRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

struct TSetRequest
    : public TRequestBase
{
    NYTree::TYPath Path;
    NYTree::INode::TPtr Value;
    NYTree::INode::TPtr Stream;

    TSetRequest()
    {
        Register("path", Path);
        Register("value", Value).Default(NULL);
        Register("stream", Stream).Default(NULL).CheckThat(~StreamSpecIsValid);
    }

    virtual void DoValidate() const
    {
        if (!Value && !Stream) {
            ythrow yexception() << Sprintf("Neither \"value\" nor \"stream\" is given");
        }
        if (Value && Stream) {
            ythrow yexception() << Sprintf("Both \"value\" and \"stream\" are given");
        }
    }
};

class TSetCommand
    : public TCommandBase<TSetRequest>
{
public:
    TSetCommand(IDriverImpl* driverImpl)
        : TCommandBase(driverImpl)
    { }

private:
    virtual void DoExecute(TSetRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

struct TRemoveRequest
    : public TRequestBase
{
    NYTree::TYPath Path;

    TRemoveRequest()
    {
        Register("path", Path);
    }
};

class TRemoveCommand
    : public TCommandBase<TRemoveRequest>
{
public:
    TRemoveCommand(IDriverImpl* driverImpl)
        : TCommandBase(driverImpl)
    { }

private:
    virtual void DoExecute(TRemoveRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

struct TListRequest
    : public TRequestBase
{
    NYTree::TYPath Path;
    NYTree::INode::TPtr Stream;

    TListRequest()
    {
        Register("path", Path);
        Register("stream", Stream).Default(NULL).CheckThat(~StreamSpecIsValid);
    }
};

class TListCommand
    : public TCommandBase<TListRequest>
{
public:
    TListCommand(IDriverImpl* driverImpl)
        : TCommandBase(driverImpl)
    { }

private:
    virtual void DoExecute(TListRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

struct TCreateRequest
    : public TRequestBase
{
    NYTree::TYPath Path;
    NYTree::INode::TPtr Stream;
    NObjectServer::EObjectType Type;
    NYTree::INode::TPtr Manifest;

    TCreateRequest()
    {
        Register("path", Path);
        Register("stream", Stream)
            .Default(NULL)
            .CheckThat(~StreamSpecIsValid);
        Register("type", Type);
        Register("manifest", Manifest)
            .Default(NULL);
    }
};

class TCreateCommand
    : public TCommandBase<TCreateRequest>
{
public:
    TCreateCommand(IDriverImpl* driverImpl)
        : TCommandBase(driverImpl)
    { }

private:
    virtual void DoExecute(TCreateRequest* request);
};

////////////////////////////////////////////////////////////////////////////////

struct TLockRequest
    : public TRequestBase
{
    NYTree::TYPath Path;

    TLockRequest()
    {
        Register("path", Path);
    }
};

class TLockCommand
    : public TCommandBase<TLockRequest>
{
public:
    TLockCommand(IDriverImpl* driverImpl)
        : TCommandBase(driverImpl)
    { }

private:
    virtual void DoExecute(TLockRequest* request);
};

////////////////////////////////////////////////////////////////////////////////


} // namespace NDriver
} // namespace NYT

