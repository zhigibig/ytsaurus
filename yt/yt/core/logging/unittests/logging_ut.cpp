
#include "yt/yt/core/misc/string_builder.h"
#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/core/logging/log.h>
#include <yt/yt/core/logging/log_manager.h>
#include <yt/yt/core/logging/log_writer.h>
#include <yt/yt/core/logging/log_writer_factory.h>
#include <yt/yt/core/logging/file_log_writer.h>
#include <yt/yt/core/logging/stream_log_writer.h>
#include <yt/yt/core/logging/random_access_gzip.h>
#include <yt/yt/core/logging/compression.h>
#include <yt/yt/core/logging/zstd_compression.h>
#include <yt/yt/core/logging/config.h>
#include <yt/yt/core/logging/formatter.h>

#include <yt/yt/core/json/json_parser.h>

#include <yt/yt/core/tracing/trace_context.h>

#include <yt/yt/core/ytree/fluent.h>
#include <yt/yt/core/ytree/convert.h>

#include <yt/yt/core/misc/range_formatters.h>

#include <library/cpp/streams/zstd/zstd.h>

#include <util/system/fs.h>
#include <util/system/tempfile.h>

#include <util/stream/zlib.h>

#include <cmath>
#include <thread>

#ifdef _unix_
#include <unistd.h>
#endif

namespace NYT::NLogging {
namespace {

using namespace NYTree;
using namespace NConcurrency;
using namespace NYson;
using namespace NJson;

////////////////////////////////////////////////////////////////////////////////

static const TLogger Logger("Test");

namespace {

TString GenerateLogFileName()
{
    return GenerateRandomFileName("log");
}

} // namespace

class TLoggingTest
    : public ::testing::Test
    , public ILogWriterHost
{
protected:
    const TLoggingCategory Category = {
        .Name = "category"
    };
    const int DateLength = ToString("2014-04-24 23:41:09,804000").length();

    IInvokerPtr GetCompressionInvoker() override
    {
        return GetCurrentInvoker();
    }

    IMapNodePtr DeserializeStructured(const TString& source, ELogFormat format)
    {
        switch (format) {
            case ELogFormat::Json: {
                auto builder = CreateBuilderFromFactory(GetEphemeralNodeFactory());
                builder->BeginTree();
                TStringStream stream(source);
                ParseJson(&stream, builder.get());
                return builder->EndTree()->AsMap();
            }
            case ELogFormat::Yson: {
                // Each line ends with a semicolon, so it must be treated as a list fragment.
                auto listFragment = ConvertTo<std::vector<IMapNodePtr>>(TYsonStringBuf(source, EYsonType::ListFragment));
                YT_VERIFY(listFragment.size() == 1);
                return listFragment.front();
            }
            default:
                YT_ABORT();
        }
    }

    void WritePlainTextEvent(const ILogWriterPtr& writer)
    {
        TLogEvent event;
        event.Family = ELogFamily::PlainText;
        event.Category = &Category;
        event.Level = ELogLevel::Debug;
        event.Message = TSharedRef::FromString("message");
        event.ThreadId = 0xba;
        WriteEvent(writer, event);
    }

    void WriteEvent(const ILogWriterPtr& writer, const TLogEvent& event)
    {
        writer->Write(event);
        writer->Flush();
    }

    std::vector<TString> ReadFile(
        const TString& fileName,
        std::optional<ECompressionMethod> compressionMethod = {})
    {
        auto splitLines = [&] (IInputStream *input) {
            TString line;
            std::vector<TString> lines;
            while (input->ReadLine(line)) {
                lines.push_back(line + "\n");
            }
            return lines;
        };

        TUnbufferedFileInput rawInput(fileName);
        if (!compressionMethod) {
            return splitLines(&rawInput);
        } else if (compressionMethod == ECompressionMethod::Gzip) {
            TZLibDecompress input(&rawInput);
            return splitLines(&input);
        } else if (compressionMethod == ECompressionMethod::Zstd) {
            TZstdDecompress input(&rawInput);
            return splitLines(&input);
        } else {
            EXPECT_TRUE(false);
            return {};
        }
    }

    bool CheckLogFileContains(const TString& fileName, const TString& message)
    {
        if (!NFs::Exists(fileName)) {
            return false;
        }

        auto lines = ReadFile(fileName);
        for (const auto& line : lines) {
            if (line.Contains(message)) {
                return true;
            }
        }
        return false;
    }

    void Configure(const TString& configYson)
    {
        auto configNode = ConvertToNode(TYsonString(configYson));
        auto config = ConvertTo<TLogManagerConfigPtr>(configNode);
        TLogManager::Get()->Configure(config, /*sync*/ true);
    }

    void DoTestCompression(ECompressionMethod method, int compressionLevel)
    {
        TTempFile logFile(GenerateLogFileName() + ".gz");

        auto writerConfig = New<TFileLogWriterConfig>();
        writerConfig->FileName = logFile.Name();
        writerConfig->EnableCompression = true;
        writerConfig->CompressionMethod = method;
        writerConfig->CompressionLevel = compressionLevel;

        auto writer = CreateFileLogWriter(
            std::make_unique<TPlainTextLogFormatter>(),
            "test_writer",
            writerConfig,
            this);

        WritePlainTextEvent(writer);

        writer->Reload();
        WritePlainTextEvent(writer);

        {
            auto lines = ReadFile(logFile.Name(), method);
            EXPECT_EQ(5, std::ssize(lines));
            EXPECT_TRUE(lines[0].find("Logging started") != TString::npos);
            EXPECT_EQ("\tD\tcategory\tmessage\tba\t\t\n", lines[1].substr(DateLength, lines[1].size()));
        }
    }
};

#ifdef _unix_

TEST_F(TLoggingTest, ReloadOnSighup)
{
    TTempFile logFile(GenerateLogFileName());
    TTempFile rotatedLogFile(logFile.Name() + ".1");

    Cerr << "Configuring logging" << Endl;

    Configure(Format(R"({
        rules = [
            {
                "min_level" = "info";
                "writers" = [ "info" ];
            };
        ];
        "writers" = {
            "info" = {
                "file_name" = "%v";
                "type" = "file";
            };
        };
    })", logFile.Name()));

    WaitForPredicate([&] {
        TString message("Message1");
        YT_LOG_INFO(message);
        return CheckLogFileContains(logFile.Name(), message);
    });

    Cerr << "Renaming logfile" << Endl;

    NFs::Rename(logFile.Name(), rotatedLogFile.Name());

    Cerr << "Sending SIGHUP" << Endl;

    ::kill(::getpid(), SIGHUP);

    Cerr << "Waiting for message 2" << Endl;

    WaitForPredicate([&] {
        TString message("Message2");
        YT_LOG_INFO(message);
        return CheckLogFileContains(logFile.Name(), message);
    });

    Cerr << "Success" << Endl;
}

TEST_F(TLoggingTest, ReloadOnRename)
{
    TTempFile logFile(GenerateLogFileName());
    TTempFile rotatedLogFile(logFile.Name() + ".1");

    Cerr << "Configuring logging" << Endl;

    Configure(Format(R"({
        watch_period = 1000;
        rules = [
            {
                "min_level" = "info";
                "writers" = [ "info" ];
            };
        ];
        "writers" = {
            "info" = {
                "file_name" = "%v";
                "type" = "file";
            };
        };
    })", logFile.Name()));

    Cerr << "Waiting for message 1" << Endl;

    WaitForPredicate([&] {
        TString message("Message1");
        YT_LOG_INFO(message);
        return CheckLogFileContains(logFile.Name(), message);
    });

    Cerr << "Renaming logfile" << Endl;

    NFs::Rename(logFile.Name(), rotatedLogFile.Name());

    Cerr << "Waiting for message 2" << Endl;

    WaitForPredicate([&] {
        TString message("Message2");
        YT_LOG_INFO(message);
        return CheckLogFileContains(logFile.Name(), message);
    });

    Cerr << "Success" << Endl;
}

#endif

TEST_F(TLoggingTest, FileWriter)
{
    TTempFile logFile(GenerateLogFileName());

    auto writerConfig = New<TFileLogWriterConfig>();
    writerConfig->FileName = logFile.Name();

    auto writer = CreateFileLogWriter(
        std::make_unique<TPlainTextLogFormatter>(),
        "test_writer",
        writerConfig,
        this);

    WritePlainTextEvent(writer);

    {
        auto lines = ReadFile(logFile.Name());
        EXPECT_EQ(2, std::ssize(lines));
        EXPECT_TRUE(lines[0].find("Logging started") != TString::npos);
        EXPECT_EQ("\tD\tcategory\tmessage\tba\t\t\n", lines[1].substr(DateLength, lines[1].size()));
    }

    writer->Reload();
    WritePlainTextEvent(writer);

    {
        auto lines = ReadFile(logFile.Name());
        EXPECT_EQ(5, std::ssize(lines));
        EXPECT_TRUE(lines[0].find("Logging started") != TString::npos);
        EXPECT_EQ("\tD\tcategory\tmessage\tba\t\t\n", lines[1].substr(DateLength));
        EXPECT_EQ("\n", lines[2]);
        EXPECT_TRUE(lines[3].find("Logging started") != TString::npos);
        EXPECT_EQ("\tD\tcategory\tmessage\tba\t\t\n", lines[4].substr(DateLength));
    }
}

TEST_F(TLoggingTest, GzipCompression)
{
    // No compression.
    DoTestCompression(ECompressionMethod::Gzip, /*compressionLevel*/ 0);

    // Default compression.
    DoTestCompression(ECompressionMethod::Gzip, /*compressionLevel*/ 6);

    // Maximum compression.
    DoTestCompression(ECompressionMethod::Gzip, /*compressionLevel*/ 9);
}

TEST_F(TLoggingTest, ZstdCompression)
{
    // Default compression.
    DoTestCompression(ECompressionMethod::Zstd, /*compressionLevel*/ 0);

    // Fast compression (--fast=<...>).
    DoTestCompression(ECompressionMethod::Zstd, /*compressionLevel*/ -2);

    // Fast compression.
    DoTestCompression(ECompressionMethod::Zstd, /*compressionLevel*/ 1);

    // Maximum compression.
    DoTestCompression(ECompressionMethod::Zstd, /*compressionLevel*/ 22);
}

TEST_F(TLoggingTest, StreamWriter)
{
    TStringStream stringOutput;
    auto writer = CreateStreamLogWriter(
        std::make_unique<TPlainTextLogFormatter>(),
        "test_writer",
        &stringOutput);

    WritePlainTextEvent(writer);

    EXPECT_EQ(
       "\tD\tcategory\tmessage\tba\t\t\n",
       stringOutput.Str().substr(DateLength));
}

TEST_F(TLoggingTest, Rule)
{
    auto rule = New<TRuleConfig>();
    rule->Load(ConvertToNode(TYsonString(TStringBuf(
        R"({
            exclude_categories = [ bus ];
            min_level = info;
            writers = [ some_writer ];
        })"))));

    EXPECT_TRUE(rule->IsApplicable("some_service", ELogFamily::PlainText));
    EXPECT_FALSE(rule->IsApplicable("bus", ELogFamily::PlainText));
    EXPECT_FALSE(rule->IsApplicable("bus", ELogLevel::Debug, ELogFamily::PlainText));
    EXPECT_FALSE(rule->IsApplicable("some_service", ELogLevel::Debug, ELogFamily::PlainText));
    EXPECT_TRUE(rule->IsApplicable("some_service", ELogLevel::Warning, ELogFamily::PlainText));
    EXPECT_TRUE(rule->IsApplicable("some_service", ELogLevel::Info, ELogFamily::PlainText));
}

TEST_F(TLoggingTest, LogManager)
{
    TTempFile infoFile(GenerateLogFileName());
    TTempFile errorFile(GenerateLogFileName());

    Configure(Format(R"({
        rules = [
            {
                "min_level" = "info";
                "writers" = [ "info" ];
            };
            {
                "min_level" = "error";
                "writers" = [ "error" ];
            };
        ];
        "writers" = {
            "error" = {
                "file_name" = "%v";
                "type" = "file";
            };
            "info" = {
                "file_name" = "%v";
                "type" = "file";
            };
        };
    })", errorFile.Name(), infoFile.Name()));

    YT_LOG_DEBUG("Debug message");
    YT_LOG_INFO("Info message");
    YT_LOG_ERROR("Error message");

    TLogManager::Get()->Synchronize();

    auto infoLog = ReadFile(infoFile.Name());
    auto errorLog = ReadFile(errorFile.Name());

    EXPECT_EQ(3, std::ssize(infoLog));
    EXPECT_EQ(2, std::ssize(errorLog));
}

TEST_F(TLoggingTest, StructuredLogging)
{
    TLogEvent event;
    event.Family = ELogFamily::Structured;
    event.Category = &Category;
    event.Level = ELogLevel::Debug;
    event.StructuredMessage = BuildYsonStringFluently<EYsonType::MapFragment>()
        .Item("message")
        .Value("test_message")
        .Finish();

    for (auto format : {ELogFormat::Yson, ELogFormat::Json}) {
        TTempFile logFile(GenerateLogFileName());

        auto writerConfig = New<TFileLogWriterConfig>();
        writerConfig->FileName = logFile.Name();

        auto writer = CreateFileLogWriter(
            std::make_unique<TStructuredLogFormatter>(format, THashMap<TString, INodePtr>{}),
            "test_writer",
            writerConfig,
            this);

        WriteEvent(writer, event);
        TLogManager::Get()->Synchronize();

        auto log = ReadFile(logFile.Name());

        auto loggingStarted = DeserializeStructured(log[0], format);
        EXPECT_EQ(loggingStarted->GetChildOrThrow("message")->AsString()->GetValue(), "Logging started");
        EXPECT_EQ(loggingStarted->GetChildOrThrow("level")->AsString()->GetValue(), "info");
        EXPECT_EQ(loggingStarted->GetChildOrThrow("category")->AsString()->GetValue(), "Logging");

        auto message = DeserializeStructured(log[1], format);
        EXPECT_EQ(message->GetChildOrThrow("message")->AsString()->GetValue(), "test_message");
        EXPECT_EQ(message->GetChildOrThrow("level")->AsString()->GetValue(), "debug");
        EXPECT_EQ(message->GetChildOrThrow("category")->AsString()->GetValue(), "category");
    }
}

TEST_F(TLoggingTest, StructuredLoggingJsonFormat)
{
    TString longString(1000, 'a');
    TString longStringPrefix(100, 'a');

    TLogEvent event;
    event.Family = ELogFamily::Structured;
    event.Category = &Category;
    event.Level = ELogLevel::Debug;
    event.StructuredMessage = BuildYsonStringFluently<EYsonType::MapFragment>()
        .Item("message").Value("test_message")
        .Item("nan_value").Value(std::nan("1"))
        .Item("long_string_value").Value(longString)
        .Finish();

    auto jsonFormat = New<TJsonFormatConfig>();
    jsonFormat->StringifyNanAndInfinity = true;
    jsonFormat->StringLengthLimit = 100;

    TTempFile logFile(GenerateLogFileName());

    auto writerConfig = New<TFileLogWriterConfig>();
    writerConfig->FileName = logFile.Name();

    auto formatter = std::make_unique<TStructuredLogFormatter>(
        ELogFormat::Json,
        /*commonFields*/ THashMap<TString, INodePtr>{},
        /*enableControlMessages*/ true,
        jsonFormat);

    auto writer = CreateFileLogWriter(
        std::move(formatter),
        "test_writer",
        writerConfig,
        this);

    WriteEvent(writer, event);
    TLogManager::Get()->Synchronize();

    auto log = ReadFile(logFile.Name());

    auto loggingStarted = DeserializeStructured(log[0], ELogFormat::Json);
    EXPECT_EQ(loggingStarted->GetChildOrThrow("message")->AsString()->GetValue(), "Logging started");
    EXPECT_EQ(loggingStarted->GetChildOrThrow("level")->AsString()->GetValue(), "info");
    EXPECT_EQ(loggingStarted->GetChildOrThrow("category")->AsString()->GetValue(), "Logging");

    auto message = DeserializeStructured(log[1], ELogFormat::Json);
    EXPECT_EQ(message->GetChildOrThrow("message")->AsString()->GetValue(), "test_message");
    EXPECT_EQ(message->GetChildOrThrow("nan_value")->AsString()->GetValue(), "nan");
    EXPECT_EQ(message->GetChildOrThrow("long_string_value")->AsString()->GetValue(), longStringPrefix);
    EXPECT_EQ(message->GetChildOrThrow("level")->AsString()->GetValue(), "debug");
    EXPECT_EQ(message->GetChildOrThrow("category")->AsString()->GetValue(), "category");
}

////////////////////////////////////////////////////////////////////////////////

class TAppendableZstdFileTest
    : public ::testing::Test
{
protected:
    TTempFile GetLogFile()
    {
        return {GenerateLogFileName() + ".zst"};
    }

    TAppendableCompressedFilePtr CreateAppendableZstdFile(TFile rawFile, bool writeTruncateMessage)
    {
        return New<TAppendableCompressedFile>(
            std::move(rawFile),
            CreateZstdCompressionCodec(),
            GetCurrentInvoker(),
            writeTruncateMessage);
    }

    void WriteTestFile(const TString& filename, i64 addBytes, bool writeTruncateMessage)
    {
        {
            TFile rawFile(filename, OpenAlways|RdWr|CloseOnExec);
            auto file = CreateAppendableZstdFile(rawFile, writeTruncateMessage);
            *file << "foo\n";
            file->Flush();
            *file << "bar\n";
            file->Finish();

            rawFile.Resize(rawFile.GetLength() + addBytes);
        }
        {
            TFile rawFile(filename, OpenAlways|RdWr|CloseOnExec);
            auto file = CreateAppendableZstdFile(rawFile, writeTruncateMessage);
            *file << "zog\n";
            file->Flush();
        }
    }

    std::vector<char> GenerateIncompressibleData(i64 size) {
        std::vector<char> data(size);
        for (int index = 0; index < size; index++) {
            data[index] = rand();
        }
        return std::move(data);
    }
};

TEST_F(TAppendableZstdFileTest, Write)
{
    auto logFile = GetLogFile();
    WriteTestFile(logFile.Name(), 0, false);

    TUnbufferedFileInput file(logFile.Name());
    TZstdDecompress decompress(&file);
    EXPECT_EQ("foo\nbar\nzog\n", decompress.ReadAll());
}

TEST_F(TAppendableZstdFileTest, WriteMultipleFramesPerFlush)
{
    auto logFile = GetLogFile();
    auto data = GenerateIncompressibleData(5 * MaxZstdFrameUncompressedLength);

    {
        TFile rawFile(logFile.Name(), OpenAlways|RdWr|CloseOnExec);
        auto file = CreateAppendableZstdFile(rawFile, true);
        file->Write(data.data(), data.size());
        file->Finish();
    }

    TUnbufferedFileInput file(logFile.Name());
    TZstdDecompress decompress(&file);
    auto decompressed = decompress.ReadAll();

    EXPECT_EQ(data.size(), decompressed.size());
    EXPECT_TRUE(std::equal(data.begin(), data.end(), decompressed.begin()));
}

TEST_F(TAppendableZstdFileTest, RepairSmall)
{
    auto logFile = GetLogFile();
    WriteTestFile(logFile.Name(), -1, false);

    TUnbufferedFileInput file(logFile.Name());
    TZstdDecompress decompress(&file);
    EXPECT_EQ("foo\nzog\n", decompress.ReadAll());
}

TEST_F(TAppendableZstdFileTest, RepairLarge)
{
    auto logFile = GetLogFile();
    WriteTestFile(logFile.Name(), 10_MB, true);

    TUnbufferedFileInput file(logFile.Name());
    TZstdDecompress decompress(&file);

    TStringBuilder expected;
    expected.AppendFormat("foo\nbar\nTruncated %v bytes due to zstd repair.\nzog\n", 10_MB);
    EXPECT_EQ(expected.Flush(), decompress.ReadAll());
}

TEST(TRandomAccessGZipTest, Write)
{
    TTempFile logFile(GenerateLogFileName() + ".gz");

    {
        TFile rawFile(logFile.Name(), OpenAlways|RdWr|CloseOnExec);
        auto file = New<TRandomAccessGZipFile>(rawFile);
        *file << "foo\n";
        file->Flush();
        *file << "bar\n";
        file->Finish();
    }
    {
        TFile rawFile(logFile.Name(), OpenAlways|RdWr|CloseOnExec);
        auto file = New<TRandomAccessGZipFile>(rawFile);
        *file << "zog\n";
        file->Finish();
    }

    auto input = TUnbufferedFileInput(logFile.Name());
    TZLibDecompress decompress(&input);
    EXPECT_EQ("foo\nbar\nzog\n", decompress.ReadAll());
}

TEST(TRandomAccessGZipTest, RepairIncompleteBlocks)
{
    TTempFile logFile(GenerateLogFileName() + ".gz");

    {
        TFile rawFile(logFile.Name(), OpenAlways|RdWr|CloseOnExec);
        auto file = New<TRandomAccessGZipFile>(rawFile);
        *file << "foo\n";
        file->Flush();
        *file << "bar\n";
        file->Finish();
    }

    i64 fullSize;
    {
        TFile file(logFile.Name(), OpenAlways|RdWr);
        fullSize = file.GetLength();
        file.Resize(fullSize - 1);
    }

    {
        TFile rawFile(logFile.Name(), OpenAlways | RdWr | CloseOnExec);
        auto file = New<TRandomAccessGZipFile>(rawFile);
    }

    {
        TFile file(logFile.Name(), OpenAlways|RdWr);
        EXPECT_LE(file.GetLength(), fullSize - 1);
    }
}

// This test is for manual check of YT_LOG_FATAL
TEST_F(TLoggingTest, DISABLED_LogFatal)
{
    TTempFile logFile(GenerateLogFileName());

    Configure(Format(R"({
        rules = [
            {
                "min_level" = "info";
                "writers" = [ "info" ];
            };
        ];
        "writers" = {
            "info" = {
                "file_name" = "%v";
                "type" = "file";
            };
        };
    })", logFile.Name()));

    YT_LOG_INFO("Info message");

    Sleep(TDuration::MilliSeconds(100));

    YT_LOG_INFO("Info message");
    YT_LOG_FATAL("FATAL");
}

TEST_F(TLoggingTest, RequestSuppression)
{
    TTempFile logFile(GenerateLogFileName());

    Configure(Format(R"({
        rules = [
            {
                "min_level" = "info";
                "writers" = [ "info" ];
            };
        ];
        "writers" = {
            "info" = {
                "file_name" = "%v";
                "type" = "file";
            };
        };
        "request_suppression_timeout" = 100;
    })", logFile.Name()));

    {
        auto requestId = NTracing::TRequestId::Create();
        auto traceContext = NTracing::TTraceContext::NewRoot("Test");
        traceContext->SetRequestId(requestId);
        NTracing::TTraceContextGuard guard(traceContext);

        YT_LOG_INFO("Traced message");

        TLogManager::Get()->SuppressRequest(requestId);
    }

    YT_LOG_INFO("Info message");

    TLogManager::Get()->Synchronize();

    auto lines = ReadFile(logFile.Name());

    EXPECT_EQ(2, std::ssize(lines));
    EXPECT_TRUE(lines[0].find("Logging started") != TString::npos);
    EXPECT_TRUE(lines[1].find("Info message") != TString::npos);
}

////////////////////////////////////////////////////////////////////////////////

class TLoggingTagsTest
    : public ::testing::TestWithParam<std::tuple<bool, bool, bool, TString>>
{ };

TEST_P(TLoggingTagsTest, All)
{
    auto hasMessageTag = std::get<0>(GetParam());
    auto hasLoggerTag = std::get<1>(GetParam());
    auto hasTraceContext = std::get<2>(GetParam());
    auto expected = std::get<3>(GetParam());

    auto loggingContext = NLogging::GetLoggingContext();
    if (hasTraceContext) {
        loggingContext.TraceLoggingTag = TStringBuf("TraceContextTag");
    }

    auto logger = TLogger("Test");
    if (hasLoggerTag) {
        logger = logger.WithTag("LoggerTag");
    }

    if (hasMessageTag) {
        EXPECT_EQ(
            expected,
            ToString(NLogging::NDetail::BuildLogMessage(
                loggingContext,
                logger,
                "Log message (Value: %v)",
                123).Message));
    } else {
        EXPECT_EQ(
            expected,
            ToString(NLogging::NDetail::BuildLogMessage(
                loggingContext,
                logger,
                "Log message").Message));
    }
}

INSTANTIATE_TEST_SUITE_P(ValueParametrized, TLoggingTagsTest,
    ::testing::Values(
        std::make_tuple(false, false, false, "Log message"),
        std::make_tuple(false, false,  true, "Log message (TraceContextTag)"),
        std::make_tuple(false,  true, false, "Log message (LoggerTag)"),
        std::make_tuple(false,  true,  true, "Log message (LoggerTag, TraceContextTag)"),
        std::make_tuple( true, false, false, "Log message (Value: 123)"),
        std::make_tuple( true, false,  true, "Log message (Value: 123, TraceContextTag)"),
        std::make_tuple( true,  true, false, "Log message (Value: 123, LoggerTag)"),
        std::make_tuple( true,  true,  true, "Log message (Value: 123, LoggerTag, TraceContextTag)")));

////////////////////////////////////////////////////////////////////////////////

class TLongMessagesTest
    : public TLoggingTest
{
protected:
    static constexpr int N = 500;
    std::vector<TString> Chunks_;

    TLongMessagesTest()
    {
        for (int i = 0; i < N; ++i) {
            Chunks_.push_back(Format("PayloadPayloadPayloadPayloadPayload%v", i));
        }
    }

    void ConfigureForLongMessages(const TString& fileName)
    {
        Configure(Format(R"({
            rules = [
                {
                    "min_level" = "info";
                    "max_level" = "info";
                    "writers" = [ "info" ];
                };
            ];
            "writers" = {
                "info" = {
                    "file_name" = "%v";
                    "type" = "file";
                };
            };
        })", fileName));
    }

    void LogLongMessages()
    {
        for (int i = 0; i < N; ++i) {
            YT_LOG_INFO("%v", MakeRange(Chunks_.data(), Chunks_.data() + i));
        }
    }

    void CheckLongMessages(const TString& fileName)
    {
        TLogManager::Get()->Synchronize();

        auto infoLog = ReadFile(fileName);
        EXPECT_EQ(N + 1, std::ssize(infoLog));
        for (int i = 0; i < N; ++i) {
            auto expected = Format("%v", MakeRange(Chunks_.data(), Chunks_.data() + i));
            auto actual = infoLog[i + 1];
            EXPECT_NE(TString::npos, actual.find(expected));
        }
    }
};

TEST_F(TLongMessagesTest, WithPerThreadCache)
{
    TTempFile logFile(GenerateLogFileName());
    ConfigureForLongMessages(logFile.Name());
    LogLongMessages();
    CheckLongMessages(logFile.Name());
}

TEST_F(TLongMessagesTest, WithoutPerThreadCache)
{
    TTempFile logFile(GenerateLogFileName());
    ConfigureForLongMessages(logFile.Name());
    std::thread thread([&] {
        NLogging::NDetail::TMessageStringBuilder::DisablePerThreadCache();
        LogLongMessages();
    });
    thread.join();
    CheckLongMessages(logFile.Name());
}

TEST_F(TLoggingTest, Anchors)
{
    NLogging::TLogger logger;
    NLogging::TLoggingContext context{};
    EXPECT_EQ(NLogging::NDetail::BuildLogMessage(context, logger, "Simple message").Anchor, "Simple message");
    EXPECT_EQ(NLogging::NDetail::BuildLogMessage(context, logger, "Simple message (Param: %v)", 1).Anchor, "Simple message (Param: %v)");
}

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TTestWriterConfig)

class TTestWriterConfig
    : public TYsonSerializable
{
public:
    int Padding;

    TTestWriterConfig()
    {
        RegisterParameter("padding", Padding)
            .GreaterThanOrEqual(0)
            .Default(0);
    }
};

DEFINE_REFCOUNTED_TYPE(TTestWriterConfig)

DECLARE_REFCOUNTED_CLASS(TTestWriter)

class TTestWriter
    : public ILogWriter
{
public:
    explicit TTestWriter(TTestWriterConfigPtr config)
        : Config_(std::move(config))
    { }

    void Write(const TLogEvent& event) override
    {
        Messages_.push_back(TString(Config_->Padding, ' ') + ToString(event.Message));
    }

    void Flush() override
    { }

    void Reload() override
    { }

    void SetRateLimit(std::optional<i64> /*limit*/) override
    { }

    void SetCategoryRateLimits(const THashMap<TString, i64>& /*categoryRateLimits*/) override
    { }

    const std::vector<TString>& GetMessages() const
    {
        return Messages_;
    }

private:
    const TTestWriterConfigPtr Config_;

    std::vector<TString> Messages_;
};

DEFINE_REFCOUNTED_TYPE(TTestWriter)

DECLARE_REFCOUNTED_CLASS(TTestWriterFactory)

class TTestWriterFactory
    : public ILogWriterFactory
{
public:
    void ValidateConfig(const IMapNodePtr& configNode) override
    {
        ParseConfig(configNode);
    }

    ILogWriterPtr CreateWriter(
        std::unique_ptr<ILogFormatter> /*formater*/,
        TString /*name*/,
        const IMapNodePtr& configNode,
        ILogWriterHost* /*host*/) noexcept override
    {
        EXPECT_FALSE(Writer_.operator bool());
        Writer_ = New<TTestWriter>(ParseConfig(configNode));
        return Writer_;
    }

    TTestWriterPtr GetWriter()
    {
        EXPECT_TRUE(Writer_.operator bool());
        return Writer_;
    }

private:
    TTestWriterPtr Writer_;

    static TTestWriterConfigPtr ParseConfig(const IMapNodePtr& configNode)
    {
        return ConvertTo<TTestWriterConfigPtr>(configNode);
    }
};

DEFINE_REFCOUNTED_TYPE(TTestWriterFactory)

class TCustomWriterTest
    : public TLoggingTest
{
protected:
    static inline const TString CustomWriterType = "custom";
    const TTestWriterFactoryPtr WriterFactory_ = New<TTestWriterFactory>();

    void SetUp() override
    {
        TLogManager::Get()->RegisterWriterFactory(CustomWriterType, WriterFactory_);
    }

    void TearDown() override
    {
        TLogManager::Get()->UnregisterWriterFactory(CustomWriterType);
    }
};

TEST_F(TCustomWriterTest, UnknownWriterType)
{
    EXPECT_THROW_WITH_SUBSTRING(
        {
            Configure(R"({
                "rules" = [];
                "writers" = {
                    "custom" = {
                        "type" = "unknown";
                    };
                };
            })");
        },
        "Unknown log writer type");
}

TEST_F(TCustomWriterTest, WriterConfigValidation)
{
    EXPECT_THROW_WITH_SUBSTRING(
        {
            Configure(Format(R"({
                "rules" = [];
                "writers" = {
                    "custom" = {
                        "type" = "%v";
                        "padding" = -10;
                    };
                };
            })", CustomWriterType));
        },
        "Expected >= 0, found -10");
}

TEST_F(TCustomWriterTest, Write)
{
    Configure(Format(R"({
        "rules" = [
            {
                "min_level" = "info";
                "writers" = [ "custom" ];
            }
        ];
        "writers" = {
            "custom" = {
                "type" = "%v";
                "padding" = 2;
            };
        };
    })", CustomWriterType));

    YT_LOG_INFO("first");
    YT_LOG_INFO("second");
    YT_LOG_INFO("third");

    TLogManager::Get()->Synchronize();

    auto writer = WriterFactory_->GetWriter();
    const auto& messages = writer->GetMessages();
    EXPECT_EQ(3, std::ssize(messages));
    EXPECT_EQ("  first", messages[0]);
    EXPECT_EQ("  second", messages[1]);
    EXPECT_EQ("  third", messages[2]);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NLogging
