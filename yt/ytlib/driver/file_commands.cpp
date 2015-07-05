#include "stdafx.h"
#include "file_commands.h"
#include "config.h"
#include "driver.h"

#include <ytlib/api/file_reader.h>
#include <ytlib/api/file_writer.h>

#include <ytlib/chunk_client/chunk_spec.h>

#include <core/concurrency/scheduler.h>

namespace NYT {
namespace NDriver {

using namespace NApi;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

void TReadFileCommand::DoExecute()
{
    // COMPAT(babenko): remove Request_->FileReader
    auto config = UpdateYsonSerializable(
        Context_->GetConfig()->FileReader,
        Request_->FileReader);
    config = UpdateYsonSerializable(
        config,
        Request_->GetOptions());

    TFileReaderOptions options;
    options.Offset = Request_->Offset;
    options.Length = Request_->Length;
    options.Config = std::move(config);
    SetTransactionalOptions(&options);
    SetSuppressableAccessTrackingOptions(&options);

    auto reader = Context_->GetClient()->CreateFileReader(
        Request_->Path.GetPath(),
        options);

    {
        auto result = WaitFor(reader->Open());
        THROW_ERROR_EXCEPTION_IF_FAILED(result);
    }

    auto output = Context_->Request().OutputStream;

    while (true) {
        auto blockOrError = WaitFor(reader->Read());

        THROW_ERROR_EXCEPTION_IF_FAILED(blockOrError);
        auto block = blockOrError.Value();

        if (!block)
            break;

        auto result = WaitFor(output->Write(block));
        THROW_ERROR_EXCEPTION_IF_FAILED(result);
    }
}

//////////////////////////////////////////////////////////////////////////////////

void TWriteFileCommand::DoExecute()
{
    // COMPAT(sandello): remove Request_->FileReader ??
    auto config = UpdateYsonSerializable(
        Context_->GetConfig()->FileWriter,
        Request_->FileWriter);
    config = UpdateYsonSerializable(
        config,
        Request_->GetOptions());

    TFileWriterOptions options;
    options.Append = Request_->Path.GetAppend();
    options.Config = std::move(config);
    SetTransactionalOptions(&options);

    auto writer = Context_->GetClient()->CreateFileWriter(
        Request_->Path.GetPath(),
        options);

    {
        auto result = WaitFor(writer->Open());
        THROW_ERROR_EXCEPTION_IF_FAILED(result);
    }

    struct TWriteBufferTag { };

    auto buffer = TSharedMutableRef::Allocate<TWriteBufferTag>(Context_->GetConfig()->WriteBufferSize, false);

    auto input = Context_->Request().InputStream;

    while (true) {
        auto bytesRead = WaitFor(input->Read(buffer))
            .ValueOrThrow();

        if (bytesRead == 0)
            break;

        WaitFor(writer->Write(buffer.Slice(0, bytesRead)))
            .ThrowOnError();
    }

    WaitFor(writer->Close())
        .ThrowOnError();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NDriver
} // namespace NYT
