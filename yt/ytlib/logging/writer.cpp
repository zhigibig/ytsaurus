#include "stdafx.h"
#include "writer.h"
#include "log.h"

#include <ytlib/misc/fs.h>

#include <yt/build.h>

namespace NYT {
namespace NLog {

////////////////////////////////////////////////////////////////////////////////

const char* const SystemLoggingCategory = "Logging";
static TLogger Logger(SystemLoggingCategory);

////////////////////////////////////////////////////////////////////////////////

namespace {

TLogEvent GetBannerEvent()
{
    return TLogEvent(
        SystemLoggingCategory,
        ELogLevel::Info,
        Sprintf("Logging started (Version: %s, BuildHost: %s, BuildTime: %s)",
            YT_VERSION,
            YT_BUILD_HOST,
            YT_BUILD_TIME));
}

} // namespace

TStreamLogWriter::TStreamLogWriter(
    TOutputStream* stream,
    Stroka pattern)
    : Stream(stream)
    , Pattern(pattern)
{ }

void TStreamLogWriter::Write(const TLogEvent& event)
{
    *Stream << FormatEvent(event, Pattern) << Endl;
}

void TStreamLogWriter::Flush()
{
    Stream->Flush();
}

void TStreamLogWriter::Reload()
{ }

void TStreamLogWriter::CheckSpace(i64 minSpace)
{ }

////////////////////////////////////////////////////////////////////////////////

TStdErrLogWriter::TStdErrLogWriter(const Stroka& pattern)
    : TStreamLogWriter(&StdErrStream(), pattern)
{ }

////////////////////////////////////////////////////////////////////////////////

TStdOutLogWriter::TStdOutLogWriter(const Stroka& pattern)
    : TStreamLogWriter(&StdOutStream(), pattern)
{ }

////////////////////////////////////////////////////////////////////////////////

TFileLogWriterBase::TFileLogWriterBase(const Stroka& fileName)
    : FileName(fileName)
    , Initialized(false)
{
    AtomicSet(NotEnoughSpace, false);
}

void TFileLogWriterBase::ReopenFile()
{
    NFS::ForcePath(NFS::GetDirectoryName(FileName));
    File.reset(new TFile(FileName, OpenAlways|ForAppend|WrOnly|Seq|CloseOnExec));
    FileOutput.reset(new TBufferedFileOutput(*File, BufferSize));
    FileOutput->SetFinishPropagateMode(true);
    *FileOutput << Endl;
}

void TFileLogWriterBase::CheckSpace(i64 minSpace)
{
    try {
        auto statistics = NFS::GetDiskSpaceStatistics(FileName);
        if (statistics.AvailableSpace < minSpace) {
            AtomicSet(NotEnoughSpace, true);
            LOG_ERROR(
                "Disable log writer: not enough space (FileName: %s, AvailableSpace: %" PRId64 ", MinSpace: %" PRId64 ")",
                ~FileName,
                statistics.AvailableSpace,
                minSpace);
        }
    } catch (const std::exception& ex) {
        AtomicSet(NotEnoughSpace, true);
            LOG_ERROR(
                ex,
                "Disable log writer: space check failed (FileName: %s)",
                ~FileName);
    }
}

////////////////////////////////////////////////////////////////////////////////

TFileLogWriter::TFileLogWriter(
    Stroka fileName,
    Stroka pattern)
    : TFileLogWriterBase(fileName)
    , Pattern(pattern)
{
    EnsureInitialized();
}

void TFileLogWriter::EnsureInitialized()
{
    if (Initialized || AtomicGet(NotEnoughSpace) == true)
        return;

    // No matter what, let's pretend we're initialized to avoid subsequent attempts.
    Initialized = true;

    try {
        ReopenFile();
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Error opening log file: %s", ~FileName);
        return;
    }

    LogWriter = New<TStreamLogWriter>(~FileOutput, Pattern);
    Write(GetBannerEvent());
}

void TFileLogWriter::Write(const TLogEvent& event)
{
    if (LogWriter && AtomicGet(NotEnoughSpace) == false) {
        try {
            LogWriter->Write(event);
        } catch (...) {
            LOG_ERROR("Failed to write to log (FileName: %s)", ~FileName);
        }
    }
}

void TFileLogWriter::Flush()
{
    if (LogWriter && AtomicGet(NotEnoughSpace) == false) {
        try {
            LogWriter->Flush();
        } catch (...) {
            LOG_ERROR("Failed to flush log (FileName: %s)", ~FileName);
        }
    }
}

void TFileLogWriter::Reload()
{
    Flush();

    if (~File) {
        try {
            File->Close();
        } catch (...) {
            LOG_ERROR("Failed to close log (FileName: %s)", ~FileName);
        }
    }

    Initialized = false;
    EnsureInitialized();
}

////////////////////////////////////////////////////////////////////////////////

TRawFileLogWriter::TRawFileLogWriter(const Stroka& fileName)
    : TFileLogWriterBase(fileName)
    , Buffer(new TMessageBuffer())
{
    EnsureInitialized();
}

void TRawFileLogWriter::EnsureInitialized()
{
    if (Initialized || AtomicGet(NotEnoughSpace) == true)
        return;

    // No matter what, let's pretend we're initialized to avoid subsequent attempts.
    Initialized = true;

    try {
        ReopenFile();
        *FileOutput << Endl;
    } catch (const std::exception& ex) {
        LOG_ERROR(ex, "Error opening log file: %s", ~FileName);
    }

    Write(GetBannerEvent());
}

void TRawFileLogWriter::Write(const TLogEvent& event)
{
    if (!FileOutput || AtomicGet(NotEnoughSpace) == true) {
        return;
    }

    auto* buffer = ~Buffer;
    buffer->Reset();

    FormatDateTime(buffer, event.DateTime);
    buffer->AppendChar('\t');
    FormatLevel(~Buffer, event.Level);
    buffer->AppendChar('\t');
    buffer->AppendString(~event.Category);
    buffer->AppendChar('\t');
    FormatMessage(buffer, event.Message);
    buffer->AppendChar('\t');
    if (event.ThreadId != 0) {
        buffer->AppendNumber(event.ThreadId, 16);
    }
    buffer->AppendChar('\n');

    FileOutput->Write(buffer->GetData(), buffer->GetBytesWritten());
}

void TRawFileLogWriter::Flush()
{
    if (~FileOutput && AtomicGet(NotEnoughSpace) == false) {
        try {
            FileOutput->Flush();
        } catch (...) {
            LOG_ERROR("Failed to write to log (FileName: %s)", ~FileName);
        }
    }
}

void TRawFileLogWriter::Reload()
{
    Flush();

    if (~File) {
        try {
            File->Close();
        } catch (...) {
            LOG_ERROR("Failed to close log (FileName: %s)", ~FileName);
        }
    }

    Initialized = false;
    EnsureInitialized();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NLog
} // namespace NYT
