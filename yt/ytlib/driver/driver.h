#pragma once

#include "public.h"

#include <yt/ytlib/api/public.h>

#include <yt/ytlib/formats/format.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/core/concurrency/async_stream.h>

#include <yt/core/misc/error.h>

#include <yt/core/rpc/public.h>

#include <yt/core/yson/consumer.h>
#include <yt/core/yson/writer.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NDriver {

////////////////////////////////////////////////////////////////////////////////

//! An instance of driver request.
struct TDriverRequest
{
    TDriverRequest();

    //! Request identifier to be logged.
    ui64 Id = 0;

    //! Command name to execute.
    Stroka CommandName;

    //! Stream used for reading command input.
    //! The stream must stay alive for the duration of #IDriver::Execute.
    NConcurrency::IAsyncInputStreamPtr InputStream;

    //! Stream where the command output is written.
    //! The stream must stay alive for the duration of #IDriver::Execute.
    NConcurrency::IAsyncOutputStreamPtr OutputStream;

    //! A map containing command parameters.
    NYTree::IMapNodePtr Parameters;

    //! Name of the user issuing the request.
    Stroka AuthenticatedUser = NSecurityClient::RootUserName;

    //! Provides means to return arbitrary structured data from any command.
    NYson::IYsonConsumer* ResponseParametersConsumer;
};

////////////////////////////////////////////////////////////////////////////////

//! Command meta-descriptor.
/*!
 *  Contains various meta-information describing a given command type.
 */
struct TCommandDescriptor
{
    //! Name of the command.
    Stroka CommandName;

    //! Type of data expected by the command at #TDriverRequest::InputStream.
    NFormats::EDataType InputType;

    //! Type of data written by the command to #TDriverRequest::OutputStream.
    NFormats::EDataType OutputType;

    //! Whether the command changes the state of the cell.
    bool IsVolatile;

    //! Whether the execution of a command is lengthly and/or causes a heavy load.
    bool IsHeavy;
};

////////////////////////////////////////////////////////////////////////////////

//! An instance of command execution engine.
/*!
 *  Each driver instance maintains a collection of cached connections to
 *  various YT subsystems (e.g. masters, scheduler).
 *
 *  IDriver instances are thread-safe and reentrant.
 */
struct IDriver
    : public virtual TRefCounted
{
    //! Asynchronously executes a given request.
    virtual TFuture<void> Execute(const TDriverRequest& request) = 0;

    //! Returns a descriptor for the command with a given name or
    //! |Null| if no command with this name is registered.
    virtual const TNullable<TCommandDescriptor> FindCommandDescriptor(const Stroka& commandName) const = 0;

    //! Returns a descriptor for then command with a given name.
    //! Fails if no command with this name is registered.
    const TCommandDescriptor GetCommandDescriptor(const Stroka& commandName) const;

    //! Returns the list of descriptors for all supported commands.
    virtual const std::vector<TCommandDescriptor> GetCommandDescriptors() const = 0;

    //! Returns the underlying connection.
    virtual NApi::IConnectionPtr GetConnection() = 0;

};

DEFINE_REFCOUNTED_TYPE(IDriver)

////////////////////////////////////////////////////////////////////////////////

//! Creates an implementation of IDriver with a given configuration.
IDriverPtr CreateDriver(TDriverConfigPtr config);

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT

