#include <yt/core/misc/common.h>

#include "user_job.h"

#include "config.h"
#include "job_detail.h"
#include "private.h"
#include "table_output.h"
#include "user_job_io.h"
#include "stracer.h"

#include <yt/server/exec_agent/public.h>

#include <yt/ytlib/chunk_client/public.h>

#include <yt/ytlib/table_client/helpers.h>
#include <yt/ytlib/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_writer.h>
#include <yt/ytlib/table_client/table_consumer.h>
#include <yt/ytlib/table_client/schemaless_chunk_reader.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>
#include <yt/ytlib/table_client/schemaful_reader_adapter.h>
#include <yt/ytlib/table_client/schemaful_writer_adapter.h>

#include <yt/ytlib/file_client/file_chunk_output.h>

#include <yt/ytlib/job_tracker_client/statistics.h>

#include <yt/ytlib/formats/parser.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/ytlib/cgroup/cgroup.h>

#include <yt/ytlib/query_client/public.h>
#include <yt/ytlib/query_client/evaluator.h>
#include <yt/ytlib/query_client/plan_fragment.h>
#include <yt/ytlib/query_client/query_statistics.h>

#include <yt/core/pipes/async_reader.h>
#include <yt/core/pipes/async_writer.h>

#include <yt/core/misc/fs.h>
#include <yt/core/misc/proc.h>
#include <yt/core/misc/process.h>
#include <yt/core/misc/subprocess.h>
#include <yt/core/misc/pattern_formatter.h>
#include <yt/core/misc/finally.h>

#include <yt/core/ypath/tokenizer.h>

#include <yt/core/tools/tools.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/misc/public.h>

#include <util/folder/dirut.h>

#include <util/system/execpath.h>

#include <util/stream/null.h>

namespace NYT {
namespace NJobProxy {

using namespace NYTree;
using namespace NYson;
using namespace NTableClient;
using namespace NFormats;
using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NTransactionClient;
using namespace NConcurrency;
using namespace NCGroup;
using namespace NJobAgent;
using namespace NChunkClient;
using namespace NFileClient;
using namespace NChunkClient::NProto;
using namespace NPipes;
using namespace NQueryClient;
using namespace NExecAgent;
using namespace NYPath;
using namespace NJobTrackerClient;

using NJobTrackerClient::NProto::TJobResult;
using NJobTrackerClient::NProto::TJobSpec;
using NScheduler::NProto::TUserJobSpec;

////////////////////////////////////////////////////////////////////////////////

#ifdef _unix_

static const int JobStatisticsFD = 5;
static const char* CGroupPrefix = "user_jobs/yt-job-";

static const int BufferSize = 1024 * 1024;

static const size_t MaxCustomStatisticsPathLength = 512;

static TNullOutput NullOutput;

////////////////////////////////////////////////////////////////////////////////

class TUserJob
    : public TJob
{
public:
    TUserJob(
        IJobHost* host,
        const TUserJobSpec& userJobSpec,
        const TJobId& jobId,
        std::unique_ptr<IUserJobIO> userJobIO)
        : TJob(host)
        , JobIO_(std::move(userJobIO))
        , UserJobSpec_(userJobSpec)
        , Config_(host->GetConfig())
        , JobErrorPromise_(NewPromise<void>())
        , MemoryUsage_(UserJobSpec_.memory_reserve())
        , CumulativeMemoryUsageMbSec_(0)
        , PipeIOQueue_(New<TActionQueue>("PipeIO"))
        , PeriodicQueue_(New<TActionQueue>("UserJobPeriodic"))
        , JobProberQueue_(New<TActionQueue>("JobProber"))
        , Process_(GetExecPath(), false)
        , CpuAccounting_(CGroupPrefix + ToString(jobId))
        , BlockIO_(CGroupPrefix + ToString(jobId))
        , Memory_(CGroupPrefix + ToString(jobId))
        , Freezer_(CGroupPrefix + ToString(jobId))
        , Logger(host->GetLogger())
    {
        MemoryWatchdogExecutor_ = New<TPeriodicExecutor>(
            PeriodicQueue_->GetInvoker(),
            BIND(&TUserJob::CheckMemoryUsage, MakeWeak(this)),
            Config_->MemoryWatchdogPeriod);

        BlockIOWatchdogExecutor_ = New<TPeriodicExecutor>(
            PeriodicQueue_->GetInvoker(),
            BIND(&TUserJob::CheckBlockIOUsage, MakeWeak(this)),
            Config_->BlockIOWatchdogPeriod);
    }

    virtual TJobResult Run() override
    {
        LOG_DEBUG("Starting job process");

        JobIO_->Init();

        Prepare();

        bool expected = false;
        if (Prepared_.compare_exchange_strong(expected, true)) {
            Process_.Spawn();
            LOG_INFO("Job process started");

            MemoryWatchdogExecutor_->Start();
            BlockIOWatchdogExecutor_->Start();

            DoJobIO();

            if (!JobErrorPromise_.IsSet())  {
                FinalizeJobIO();
            }

            CleanupUserProcesses();

            WaitFor(BlockIOWatchdogExecutor_->Stop());
            WaitFor(MemoryWatchdogExecutor_->Stop());
        } else {
            JobErrorPromise_.TrySet(TError("Job aborted"));
        }

        auto jobResultError = JobErrorPromise_.TryGet();

        TJobResult result;
        ToProto(result.mutable_error(), jobResultError
            ? TError("User job failed") << *jobResultError
            : TError());
        auto* schedulerResultExt = result.MutableExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

        SaveErrorChunkId(schedulerResultExt);

        if (jobResultError) {
            DumpFailContexts(schedulerResultExt);
        } else {
            JobIO_->PopulateResult(schedulerResultExt);
        }

        return result;
    }

    virtual void Abort() override
    {
        bool expected = false;
        if (!Prepared_.compare_exchange_strong(expected, true)) {
            // Job has been prepared.
            CleanupUserProcesses();
        }
    }

    virtual double GetProgress() const override
    {
        const auto& reader = JobIO_->GetReader();
        if (!reader) {
            return 0;
        }

        i64 total = reader->GetTotalRowCount();
        i64 current = reader->GetSessionRowIndex();

        if (total == 0) {
            return 0.0;
        }

        return std::min(current / static_cast<double>(total), 1.0);
    }

    virtual std::vector<TChunkId> GetFailedChunkIds() const override
    {
        std::vector<TChunkId> failedChunks;
        const auto& reader = JobIO_->GetReader();
        if (reader) {
            auto chunks = reader->GetFailedChunkIds();
            failedChunks.insert(failedChunks.end(), chunks.begin(), chunks.end());
        }
        return failedChunks;
    }

private:
    std::unique_ptr<IUserJobIO> JobIO_;

    const TUserJobSpec& UserJobSpec_;

    TJobProxyConfigPtr Config_;

    TPromise<void> JobErrorPromise_;

    std::atomic<bool> Prepared_ = { false };
    std::atomic<bool> IsWoodpecker_ = { false };

    std::atomic_flag Stracing_ = ATOMIC_FLAG_INIT;

    i64 MemoryUsage_;
    i64 CumulativeMemoryUsageMbSec_;

    TActionQueuePtr PipeIOQueue_;

    TActionQueuePtr PeriodicQueue_;
    TPeriodicExecutorPtr MemoryWatchdogExecutor_;
    TPeriodicExecutorPtr BlockIOWatchdogExecutor_;

    std::vector<std::unique_ptr<TOutputStream>> TableOutputs_;
    std::vector<TWritingValueConsumerPtr> WritingValueConsumers_;

    std::unique_ptr<TFileChunkOutput> ErrorOutput_;
    std::unique_ptr<TTableOutput> StatisticsOutput_;

    std::vector<TAsyncReaderPtr> TablePipeReaders_;
    std::vector<TAsyncWriterPtr> TablePipeWriters_;

    std::vector<ISchemalessFormatWriterPtr> FormatWriters_;

    std::vector<TCallback<void()>> InputActions_;
    std::vector<TCallback<void()>> OutputActions_;
    std::vector<TCallback<void()>> FinalizeActions_;

    TActionQueuePtr JobProberQueue_;

    TProcess Process_;

    TCpuAccounting CpuAccounting_;
    TBlockIO BlockIO_;
    TMemory Memory_;
    TFreezer Freezer_;

    TSpinLock FreezerLock_;

    std::vector<TBlockIO::TStatisticsItem> LastServicedIOs_;

    TSpinLock StatisticsLock_;
    TStatistics CustomStatistics_;

    NLogging::TLogger Logger;


    void Prepare()
    {
        PrepareCGroups();

        PreparePipes();

        Process_.AddArgument("--executor");
        Process_.AddArguments({"--command", UserJobSpec_.shell_command()});
        Process_.AddArguments({"--config", NFS::CombinePaths(GetCwd(), NExecAgent::ProxyConfigFileName)});
        Process_.AddArguments({"--working-dir", SandboxDirectoryNames[ESandboxKind::User]});

        if (UserJobSpec_.enable_core_dump()) {
            Process_.AddArgument("--enable-core-dump");
        }

        if (Config_->UserId) {
            Process_.AddArguments({"--uid", ::ToString(*Config_->UserId)});
        }

        // Init environment variables.
        TPatternFormatter formatter;
        formatter.AddProperty("SandboxPath", NFS::CombinePaths(GetCwd(), SandboxDirectoryNames[ESandboxKind::User]));

        for (int i = 0; i < UserJobSpec_.environment_size(); ++i) {
            Process_.AddArguments({"--env", formatter.Format(UserJobSpec_.environment(i))});
        }
    }

    void CleanupUserProcesses()
    {
        if (!Config_->EnableCGroups) {
            return;
        }

        try {
            // Kill everything for sanity reasons: main user process completed,
            // but its children may still be alive.
            Stroka freezerFullPath;
            {
                TGuard<TSpinLock> guard(FreezerLock_);
                freezerFullPath = Freezer_.GetFullPath();
            }

            RunKiller(freezerFullPath);
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "Failed to clean up user processes");
        }
    }

    TOutputStream* CreateStatisticsOutput()
    {
        auto consumer = std::make_unique<TStatisticsConsumer>(
            BIND(&TUserJob::AddCustomStatistics, Unretained(this)));
        auto parser = CreateParserForFormat(TFormat(EFormatType::Yson), EDataType::Tabular, consumer.get());
        StatisticsOutput_.reset(new TTableOutput(std::move(parser), std::move(consumer)));
        return StatisticsOutput_.get();
    }

    TMultiChunkWriterOptionsPtr CreateSystemFileOptions()
    {
        auto options = New<TMultiChunkWriterOptions>();
        options->Account = NSecurityClient::SysAccountName;
        options->ReplicationFactor = 1;
        options->ChunksVital = false;
        return options;
    }

    TOutputStream* CreateErrorOutput()
    {
        auto host = Host.Lock();
        YCHECK(host);

        ErrorOutput_.reset(new TFileChunkOutput(
            Config_->JobIO->ErrorFileWriter,
            CreateSystemFileOptions(),
            host->GetClient(),
            FromProto<TTransactionId>(UserJobSpec_.async_scheduler_transaction_id()),
            UserJobSpec_.max_stderr_size()));

        return ErrorOutput_.get();
    }

    void SaveErrorChunkId(TSchedulerJobResultExt* schedulerResultExt)
    {
        if (!ErrorOutput_) {
            return;
        }

        auto errorChunkId = ErrorOutput_->GetChunkId();
        if (errorChunkId) {
            ToProto(schedulerResultExt->mutable_stderr_chunk_id(), errorChunkId);
            LOG_INFO("Stderr chunk generated (ChunkId: %v)", errorChunkId);
        }
    }

    void DumpFailContexts(TSchedulerJobResultExt* schedulerResultExt)
    {
        auto contexts = DoGetInputContexts();
        auto contextChunkIds = DoDumpInputContexts(contexts);

        YCHECK(contextChunkIds.size() <= 1);
        if (!contextChunkIds.empty()) {
            ToProto(schedulerResultExt->mutable_fail_context_chunk_id(), contextChunkIds.front());
        }
    }

    virtual std::vector<TChunkId> DumpInputContext() override
    {
        if (!Prepared_) {
            THROW_ERROR_EXCEPTION("Cannot dump job context: job pipes haven't been prepared yet");
        }

        auto asyncContexts = BIND(&TUserJob::DoGetInputContexts, MakeStrong(this))
                .AsyncVia(PipeIOQueue_->GetInvoker())
                .Run();
        auto contexts = WaitFor(asyncContexts)
            .ValueOrThrow();

        return DoDumpInputContexts(contexts);
    }

    std::vector<TChunkId> DoDumpInputContexts(const std::vector<TBlob>& contexts)
    {
        auto host = Host.Lock();
        YCHECK(host);

        std::vector<TChunkId> result;

        auto transactionId = FromProto<TTransactionId>(UserJobSpec_.async_scheduler_transaction_id());
        for (int index = 0; index < contexts.size(); ++index) {
            TFileChunkOutput contextOutput(
                Config_->JobIO->ErrorFileWriter,
                CreateSystemFileOptions(),
                host->GetClient(),
                transactionId);

            const auto& context = contexts[index];
            contextOutput.Write(context.Begin(), context.Size());
            contextOutput.Finish();

            auto contextChunkId = contextOutput.GetChunkId();
            LOG_INFO("Input context chunk generated (ChunkId: %v, InputIndex: %v)",
                contextChunkId,
                index);

            result.push_back(contextChunkId);
        }

        return result;
    }

    std::vector<TBlob> DoGetInputContexts()
    {
        std::vector<TBlob> result;

        for (const auto& input : FormatWriters_) {
            result.push_back(input->GetContext());
        }

        return result;
    }

    virtual TYsonString Strace() override
    {
        if (!Prepared_) {
            THROW_ERROR_EXCEPTION("Job has not started yet");
        }

        std::vector<int> pids;

        {
            TGuard<TSpinLock> guard(FreezerLock_);
            if (!Freezer_.IsCreated()) {
                THROW_ERROR_EXCEPTION("Cannot determine user job processes: freezer cgoup is not created");
            }

            pids = Freezer_.GetTasks();
        }

        if (Stracing_.test_and_set()) {
            THROW_ERROR_EXCEPTION("Cannot strace while other stracing routing is active");
        }

        TFinallyGuard guard([this] () {
            Stracing_.clear();
        });

        auto asyncTraces = WaitFor(BIND([&] () {
            return RunTool<TStraceTool>(pids);
        })
            .AsyncVia(JobProberQueue_->GetInvoker())
            .Run());

        if (!asyncTraces.IsOK()) {
            THROW_ERROR_EXCEPTION("Failed to strace")
                << asyncTraces;
        }

        return ConvertToYsonString(asyncTraces.Value());
    }

    int GetMaxReservedDescriptor() const
    {
        int outputCount = JobIO_->GetWriters().size();
        int inputCount = 1;

        if (UserJobSpec_.use_yamr_descriptors()) {
            return 2 + outputCount;
        }

        int result = 3 * std::max(inputCount, outputCount);
        return std::max(result, JobStatisticsFD + 1);
    }

    std::vector<IValueConsumerPtr> CreateValueConsumers()
    {
        std::vector<IValueConsumerPtr> valueConsumers;
        for (const auto& writer : JobIO_->GetWriters()) {
            WritingValueConsumers_.push_back(New<TWritingValueConsumer>(writer));
            valueConsumers.push_back(WritingValueConsumers_.back());
        }
        return valueConsumers;
    }

    void PrepareOutputTablePipes(TPipeFactory* pipeFactory)
    {
        YCHECK(pipeFactory);
        auto format = ConvertTo<TFormat>(TYsonString(UserJobSpec_.output_format()));

        const auto& writers = JobIO_->GetWriters();

        TableOutputs_.resize(writers.size());
        for (int i = 0; i < writers.size(); ++i) {
            auto valueConsumers = CreateValueConsumers();
            std::unique_ptr<IYsonConsumer> consumer(new TTableConsumer(valueConsumers, i));
            auto parser = CreateParserForFormat(format, EDataType::Tabular, consumer.get());
            TableOutputs_[i].reset(new TTableOutput(
                std::move(parser),
                std::move(consumer)));

            int jobDescriptor = UserJobSpec_.use_yamr_descriptors()
                ? 3 + i
                : 3 * i + 1;

            auto reader = PrepareOutputPipe(pipeFactory->Create(), jobDescriptor, TableOutputs_[i].get());
            TablePipeReaders_.push_back(reader);
        }

        FinalizeActions_.push_back(BIND([=] () {
            for (auto valueConsumer : WritingValueConsumers_) {
                valueConsumer->Flush();
            }

            for (auto writer : JobIO_->GetWriters()) {
                auto error = WaitFor(writer->Close());
                THROW_ERROR_EXCEPTION_IF_FAILED(error, "Error closing table output");
            }
        }));
    }

    TAsyncReaderPtr PrepareOutputPipe(TPipe&& pipe, int jobDescriptor, TOutputStream* output)
    {
        Process_.AddDup2FileAction(pipe.GetWriteFD(), jobDescriptor);

        Process_.AddArguments({ "--prepare-pipe", ::ToString(jobDescriptor) });

        auto asyncInput = pipe.CreateAsyncReader();

        OutputActions_.push_back(BIND([=] (int writeFD) {
            SafeClose(writeFD, false);
            auto input = CreateSyncAdapter(asyncInput);
            PipeInputToOutput(input.get(), output, BufferSize);
        }, pipe.ReleaseWriteFD()));

        return asyncInput;
    }

    void PrepareInputActionsPassthrough(
        int jobDescriptor,
        const TFormat& format,
        TAsyncWriterPtr asyncOutput)
    {
        JobIO_->CreateReader();
        const auto& reader = JobIO_->GetReader();
        auto writer = CreateSchemalessWriterForFormat(
            format,
            reader->GetNameTable(),
            asyncOutput,
            true,
            Config_->JobIO->ControlAttributes->EnableKeySwitch,
            JobIO_->GetReduceKeyColumnCount());

        FormatWriters_.push_back(writer);

        auto bufferRowCount = Config_->JobIO->BufferRowCount;

        InputActions_.push_back(BIND([=] () {
            try {
                WaitFor(reader->Open())
                    .ThrowOnError();

                PipeReaderToWriter(
                    reader,
                    writer,
                    Config_->JobIO->ControlAttributes,
                    bufferRowCount);

                WaitFor(asyncOutput->Close())
                    .ThrowOnError();
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION("Table input pipe failed")
                        << TErrorAttribute("fd", jobDescriptor)
                        << ex;
            }
        }));
    }

    void PrepareInputActionsQuery(
        const TQuerySpec& spec,
        int jobDescriptor,
        const TFormat& format,
        TAsyncWriterPtr asyncOutput)
    {
        if (Config_->JobIO->ControlAttributes->EnableKeySwitch) {
            THROW_ERROR_EXCEPTION("enable_key_switch is not supported when query is set");
        }

        auto readerFactory = JobIO_->GetReaderFactory();

        InputActions_.push_back(BIND([=] () {
            try {
                auto query = FromProto(spec.query());

                auto resultSchema = query->GetTableSchema();
                auto resultNameTable = TNameTable::FromSchema(resultSchema);
                auto schemalessWriter = CreateSchemalessWriterForFormat(
                    format,
                    resultNameTable,
                    asyncOutput,
                    true,
                    false,
                    0);

                FormatWriters_.push_back(schemalessWriter);

                WaitFor(schemalessWriter->Open())
                    .ThrowOnError();

                auto writer = CreateSchemafulWriterAdapter(schemalessWriter);

                std::vector<TUdfDescriptorPtr> descriptors;
                for (const auto& descriptor : FromProto<Stroka>(spec.udf_descriptors())) {
                    descriptors.push_back(ConvertTo<TUdfDescriptorPtr>(TYsonString(descriptor)));
                }
                auto registry = CreateJobFunctionRegistry(descriptors, SandboxDirectoryNames[ESandboxKind::Udf]);
                auto evaluator = New<TEvaluator>(New<TExecutorConfig>());
                auto reader = WaitFor(CreateSchemafulReaderAdapter(readerFactory, query->TableSchema))
                    .ValueOrThrow();

                evaluator->Run(query, reader, writer, registry, true);
                WaitFor(asyncOutput->Close())
                    .ThrowOnError();
            } catch (const std::exception& ex) {
                THROW_ERROR_EXCEPTION("Query evaluation failed")
                    << TErrorAttribute("fd", jobDescriptor)
                    << ex;
            }
        }));
    }

    void PrepareInputTablePipe(TPipeFactory* pipeFactory)
    {
        YCHECK(pipeFactory);
        auto pipe = pipeFactory->Create();
        int jobDescriptor = 0;

        Process_.AddDup2FileAction(pipe.GetReadFD(), jobDescriptor);
        Process_.AddArguments({ "--prepare-pipe", ::ToString(jobDescriptor) });

        auto format = ConvertTo<TFormat>(TYsonString(UserJobSpec_.input_format()));
        auto asyncOutput = pipe.CreateAsyncWriter();
        TablePipeWriters_.push_back(asyncOutput);

        // NB: we do not bother to close it. Anyway, job proxy process would not live long.
        auto readFD = pipe.ReleaseReadFD();

        auto host = Host.Lock();
        YCHECK(host);

        auto jobSpec = host->GetJobSpec().GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        if (jobSpec.has_input_query_spec()) {
            PrepareInputActionsQuery(jobSpec.input_query_spec(), jobDescriptor, format, asyncOutput);
        } else {
            PrepareInputActionsPassthrough(jobDescriptor, format, asyncOutput);
        }

        if (!UserJobSpec_.check_input_fully_consumed()) {
            return;
        }

        FinalizeActions_.push_back(BIND([=] (int readFD) {
            char buffer;
            // Try to read some data from the pipe.
            ssize_t res = ::read(readFD, &buffer, 1);
            if (res > 0) {
                THROW_ERROR_EXCEPTION("Input stream was not fully consumed by user process")
                    << TErrorAttribute("fd", jobDescriptor);
            }
            YCHECK(TryClose(readFD, false));
        }, readFD));
    }

    void PreparePipes()
    {
        LOG_DEBUG("Initializing pipes");

        // We use the following convention for designating input and output file descriptors
        // in job processes:
        // fd == 3 * (N - 1) for the N-th input table (if exists)
        // fd == 3 * (N - 1) + 1 for the N-th output table (if exists)
        // fd == 2 for the error stream
        // e. g.
        // 0 - first input table
        // 1 - first output table
        // 2 - error stream
        // 3 - second input
        // 4 - second output
        // etc.
        //
        // A special option (ToDo(psushin): which one?) enables concatenating
        // all input streams into fd == 0.

        int maxReservedDescriptor = GetMaxReservedDescriptor();
        YASSERT(maxReservedDescriptor > 0);

        // To avoid descriptor collisions between pipes on this, proxy side,
        // and "standard" descriptor numbers in forked job (see comments above)
        // we ensure that enough lower descriptors are allocated before creating pipes.

        TPipeFactory pipeFactory(maxReservedDescriptor + 1);

        // Configure stderr pipe.
        PrepareOutputPipe(pipeFactory.Create(), STDERR_FILENO, CreateErrorOutput());

        PrepareOutputTablePipes(&pipeFactory);

        if (UserJobSpec_.use_yamr_descriptors()) {
            // This hack is to work around the fact that usual output pipe accepts a
            // single job descriptor, whilst yamr convention requires fds 1 and 3 to be the same.
            Process_.AddDup2FileAction(3, 1);
        } else {
            // Configure statistics output pipe.
            PrepareOutputPipe(pipeFactory.Create(), JobStatisticsFD, CreateStatisticsOutput());
        }

        PrepareInputTablePipe(&pipeFactory);

        // Close reserved descriptors.
        pipeFactory.Clear();

        LOG_DEBUG("Pipes initialized");
    }

    void PrepareCGroups()
    {
#ifdef _linux_
        if (!Config_->EnableCGroups) {
            return;
        }

        try {
            {
                TGuard<TSpinLock> guard(FreezerLock_);
                Freezer_.Create();
                Process_.AddArguments({ "--cgroup", Freezer_.GetFullPath() });
            }

            if (Config_->IsCGroupSupported(TCpuAccounting::Name)) {
                CpuAccounting_.Create();
                Process_.AddArguments({ "--cgroup", CpuAccounting_.GetFullPath() });
                Process_.AddArguments({ "--env", Format("YT_CGROUP_CPUACCT=%v", CpuAccounting_.GetFullPath()) });
            }

            if (Config_->IsCGroupSupported(TBlockIO::Name)) {
                BlockIO_.Create();
                Process_.AddArguments({ "--cgroup", BlockIO_.GetFullPath() });
                Process_.AddArguments({ "--env", Format("YT_CGROUP_BLKIO=%v", BlockIO_.GetFullPath()) });
            }

            if (Config_->IsCGroupSupported(TMemory::Name)) {
                Memory_.Create();
                Process_.AddArguments({ "--cgroup", Memory_.GetFullPath() });
                Process_.AddArguments({ "--env", Format("YT_CGROUP_MEMORY=%v", Memory_.GetFullPath()) });
            }
        } catch (const std::exception& ex) {
            LOG_FATAL(ex, "Failed to create required cgroups");
        }
#endif
    }

    void AddCustomStatistics(const INodePtr& sample)
    {
        TGuard<TSpinLock> guard(StatisticsLock_);
        CustomStatistics_.AddSample("/custom", sample);

        size_t customStatisticsCount = 0;
        for (const auto& pair : CustomStatistics_.Data()) {
            if (HasPrefix(pair.first, "/custom")) {
                if (pair.first.size() > MaxCustomStatisticsPathLength) {
                    THROW_ERROR_EXCEPTION(
                        "Custom statistics path is too long: %v > %v",
                        pair.first.size(),
                        MaxCustomStatisticsPathLength);
                }
                ++customStatisticsCount;
            }

            // ToDo(psushin): validate custom statistics path does not contain $.
        }

        if (customStatisticsCount > UserJobSpec_.custom_statistics_count_limit()) {
            THROW_ERROR_EXCEPTION(
                "Custom statistics count exceeded: %v > %v",
                customStatisticsCount,
                UserJobSpec_.custom_statistics_count_limit());
        }
    }

    virtual TStatistics GetStatistics() const override
    {
        TStatistics statistics;
        {
            TGuard<TSpinLock> guard(StatisticsLock_);
            statistics = CustomStatistics_;
        }
        const auto& reader = JobIO_->GetReader();
        if (reader) {
            statistics.AddSample("/data/input", reader->GetDataStatistics());
        }

        int i = 0;
        for (const auto& writer : JobIO_->GetWriters()) {
            statistics.AddSample(
                "/data/output/" + NYPath::ToYPathLiteral(i),
                writer->GetDataStatistics());
            ++i;
        }

        // Cgroups statistics.
        if (Config_->EnableCGroups && Prepared_) {
            if (Config_->IsCGroupSupported(TCpuAccounting::Name)) {
                statistics.AddSample("/user_job/cpu", CpuAccounting_.GetStatistics());
            }

            if (Config_->IsCGroupSupported(TBlockIO::Name)) {
                statistics.AddSample("/user_job/block_io", BlockIO_.GetStatistics());
            }

            if (Config_->IsCGroupSupported(TMemory::Name)) {
                statistics.AddSample("/user_job/max_memory", Memory_.GetMaxMemoryUsage());
                statistics.AddSample("/user_job/current_memory", Memory_.GetStatistics());
            }

            statistics.AddSample("/user_job/cumulative_memory_mb_sec", CumulativeMemoryUsageMbSec_);
            statistics.AddSample("/user_job/woodpecker", IsWoodpecker_ ? 1 : 0);
        }

        return statistics;
    }

    void DoJobIO()
    {
        auto onIOError = BIND([=] (const TError& error) {
            if (error.IsOK() || error.FindMatching(NPipes::EErrorCode::Aborted)) {
                return;
            }

            if (!JobErrorPromise_.TrySet(error)) {
                return;
            }

            LOG_ERROR(error, "Job input/output error, aborting");

            // This is a workaround for YT-2837.
            BIND(&TUserJob::CleanupUserProcesses, MakeWeak(this))
                .Via(PipeIOQueue_->GetInvoker())
                .Run();

            for (auto& reader : TablePipeReaders_) {
                reader->Abort();
            }

            for (auto& writer : TablePipeWriters_) {
                writer->Abort();
            }
        });

        auto runActions = [&] (std::vector<TCallback<void()>>& actions) {
            std::vector<TFuture<void>> result;
            for (auto& action : actions) {
                auto asyncError = action
                    .AsyncVia(PipeIOQueue_->GetInvoker())
                    .Run();
                asyncError.Subscribe(onIOError);
                result.emplace_back(std::move(asyncError));
            }
            return result;
        };

        auto inputFutures = runActions(InputActions_);
        auto outputFutures = runActions(OutputActions_);

        // First, wait for all job output pipes.
        // If job successfully completes or dies prematurely, they close automatically.
        // ToDo(psushin): extract into separate function (e.g. CombineAll?  )
        for (const auto& future : outputFutures) {
            WaitFor(future);
        }

        // Then, wait for job process to finish.
        // Theoretically, process may have explicitely closed its output pipes,
        // but still be doing some computations.
        auto jobExitError = Process_.Wait();
        LOG_INFO(jobExitError, "Job process completed");
        onIOError.Run(jobExitError);

        // Abort input pipes unconditionally.
        // If job didn't read input to the end, pipe writer could be blocked,
        // because we didn't close the reader end (see check_input_fully_consumed).
        for (auto& writer : TablePipeWriters_) {
            writer->Abort();
        }

        // Now, make sure, that input pipes are also completed.
        for (const auto& future : inputFutures) {
            WaitFor(future);
        }
    }

    void FinalizeJobIO()
    {
        for (auto& action : FinalizeActions_) {
            try {
                action.Run();
            } catch (const std::exception& ex) {
                JobErrorPromise_.TrySet(ex);
            }
        }
    }

    i64 GetMemoryUsageByUid(int uid) const
    {
        auto pids = GetPidsByUid(uid);

        i64 rss = 0;
        // Warning: we can account here a ytserver process in executor mode memory consumption.
        // But this is not a problem because it does not consume much.
        for (int pid : pids) {
            try {
                i64 processRss = GetProcessRss(pid);
                LOG_DEBUG("PID: %v, RSS: %v",
                    pid,
                    processRss);
                rss += processRss;
            } catch (const std::exception& ex) {
                LOG_DEBUG(ex, "Failed to get RSS for PID %v", pid);
            }
        }
        return rss;
    }

    void UpdateMemoryUsage(i64 rss)
    {
        auto host = Host.Lock();
        if (!host)
            return;

        i64 delta = rss - MemoryUsage_;
        LOG_DEBUG("Memory usage increased by %v", delta);

        MemoryUsage_ = rss;

        auto resourceUsage = host->GetResourceUsage();
        resourceUsage.set_memory(resourceUsage.memory() + delta);
        host->SetResourceUsage(resourceUsage);
    }

    void CheckMemoryUsage()
    {
        if (!Config_->UserId) {
            LOG_DEBUG("Memory usage control is disabled");
            return;
        }

        i64 rss = GetMemoryUsageByUid(*Config_->UserId);

        if (Memory_.IsCreated()) {
            auto statistics = Memory_.GetStatistics();

            i64 uidRss = rss;
            rss = statistics.Rss + statistics.MappedFile;

            if (rss > 1.05 * uidRss && uidRss > 0) {
                LOG_ERROR("Memory usage measured by cgroup is much greater than via procfs: %v > %v",
                    rss,
                    uidRss);
            }
        }

        CumulativeMemoryUsageMbSec_ += (rss / (1024 * 1024)) * Config_->MemoryWatchdogPeriod.Seconds();

        i64 memoryLimit = UserJobSpec_.memory_limit();
        LOG_DEBUG("Check memory usage (Rss: %v, MemoryLimit: %v)",
            rss,
            memoryLimit);

        if (rss > MemoryUsage_) {
            UpdateMemoryUsage(rss);
        }

        if (rss > memoryLimit) {
            JobErrorPromise_.TrySet(TError(EErrorCode::MemoryLimitExceeded, "Memory limit exceeded")
                << TErrorAttribute("rss", rss)
                << TErrorAttribute("limit", memoryLimit));

            if (!Config_->EnableCGroups) {
                // TODO(psushin): If someone wanted to use
                // YT without cgroups in production than one need to
                // implement kill by uid here
                return;
            }

            YCHECK(Freezer_.IsCreated());

            try {
                Stroka freezerFullPath;
                {
                    TGuard<TSpinLock> guard(FreezerLock_);
                    freezerFullPath = Freezer_.GetFullPath();
                }

                RunKiller(freezerFullPath);
            } catch (const std::exception& ex) {
                LOG_FATAL(ex, "Failed to clean up user processes");
            }
        }
    }

    void CheckBlockIOUsage()
    {
        if (!BlockIO_.IsCreated()) {
            return;
        }

        auto period = Config_->BlockIOWatchdogPeriod;
        auto servicedIOs = BlockIO_.GetIOServiced();

        for (const auto& item : servicedIOs) {
            LOG_DEBUG("Serviced %v IO operations (OperationType: %v, DeviceId: %v)", item.Value, item.Type, item.DeviceId);

            auto previousItemIt = std::find_if(
                LastServicedIOs_.begin(),
                LastServicedIOs_.end(),
                [=] (const TBlockIO::TStatisticsItem& other) {
                    return item.DeviceId == other.DeviceId  && item.Type == other.Type;
                });

            i64 deltaOperations = item.Value;
            if (previousItemIt != LastServicedIOs_.end()) {
                deltaOperations -= previousItemIt->Value;
            }

            if (deltaOperations < 0) {
                LOG_WARNING("%v < 0 IO operations were serviced since the last check (DeviceId: %v)",
                    deltaOperations,
                    item.DeviceId);
            }

            if (deltaOperations > UserJobSpec_.iops_threshold() * period.Seconds()) {
                LOG_DEBUG("Woodpecker detected (DeviceId: %v)", item.DeviceId);
                IsWoodpecker_ = true;
                if (Config_->EnableIopsThrottling) {
                    BlockIO_.ThrottleOperations(item.DeviceId, UserJobSpec_.iops_threshold());
                } 
            }
        }

        LastServicedIOs_ = servicedIOs;
    }
};

////////////////////////////////////////////////////////////////////////////////

IJobPtr CreateUserJob(
    IJobHost* host,
    const TUserJobSpec& UserJobSpec_,
    const TJobId& jobId,
    std::unique_ptr<IUserJobIO> userJobIO)
{
    return New<TUserJob>(
        host,
        UserJobSpec_,
        jobId,
        std::move(userJobIO));
}

#else

IJobPtr CreateUserJob(
    IJobHost* host,
    const TUserJobSpec& UserJobSpec_,
    const TJobId& jobId,
    std::unique_ptr<IUserJobIO> userJobIO)
{
    THROW_ERROR_EXCEPTION("Streaming jobs are supported only under Linux");
}

#endif

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
