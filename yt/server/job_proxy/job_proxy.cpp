#include "job_proxy.h"
#include "config.h"
#include "job_prober_service.h"
#include "map_job_io.h"
#include "merge_job.h"
#include "partition_job.h"
#include "partition_map_job_io.h"
#include "partition_reduce_job_io.h"
#include "partition_sort_job.h"
#include "remote_copy_job.h"
#include "simple_sort_job.h"
#include "sorted_merge_job.h"
#include "sorted_reduce_job_io.h"
#include "user_job.h"
#include "user_job_io.h"

#include <yt/server/exec_agent/config.h>

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/connection.h>

#include <yt/ytlib/cgroup/cgroup.h>

#include <yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/ytlib/chunk_client/config.h>
#include <yt/ytlib/chunk_client/data_statistics.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>
#include <yt/ytlib/node_tracker_client/helpers.h>

#include <yt/ytlib/scheduler/public.h>

#include <yt/core/bus/tcp_client.h>
#include <yt/core/bus/tcp_server.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/periodic_executor.h>

#include <yt/core/logging/log_manager.h>

#include <yt/core/misc/lfalloc_helpers.h>
#include <yt/core/misc/proc.h>
#include <yt/core/misc/ref_counted_tracker.h>

#include <yt/core/rpc/bus_channel.h>
#include <yt/core/rpc/helpers.h>
#include <yt/core/rpc/server.h>
#include <yt/core/rpc/bus_server.h>

#include <yt/core/ytree/public.h>

namespace NYT {
namespace NJobProxy {

using namespace NScheduler;
using namespace NExecAgent;
using namespace NExecAgent::NProto;
using namespace NBus;
using namespace NRpc;
using namespace NApi;
using namespace NScheduler;
using namespace NScheduler::NProto;
using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NNodeTrackerClient::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NConcurrency;
using namespace NCGroup;
using namespace NYTree;
using namespace NYson;

using NJobTrackerClient::TStatistics;

////////////////////////////////////////////////////////////////////////////////

static const auto RpcServerShutdownTimeout = TDuration::Seconds(15);

// Option cpu.share is limited to [2, 1024], see http://git.kernel.org/cgit/linux/kernel/git/tip/tip.git/tree/kernel/sched/sched.h#n279
// To overcome this limitation we consider one cpu_limit unit as ten cpu.shares units.
static const int CpuShareMultiplier = 10;

////////////////////////////////////////////////////////////////////////////////

TJobProxy::TJobProxy(
    INodePtr configNode,
    const TOperationId& operationId,
    const TJobId& jobId)
    : ConfigNode_(configNode)
    , OperationId_(operationId)
    , JobId_(jobId)
    , JobThread_(New<TActionQueue>("JobMain"))
    , ControlThread_(New<TActionQueue>("Control"))
    , Logger(JobProxyLogger)
{
    Logger.AddTag("OperationId: %v, JobId: %v",
        OperationId_,
        JobId_);
}

std::vector<NChunkClient::TChunkId> TJobProxy::DumpInputContext(const TJobId& jobId)
{
    ValidateJobId(jobId);
    return Job_->DumpInputContext();
}

Stroka TJobProxy::GetStderr(const TJobId& jobId)
{
    ValidateJobId(jobId);
    return Job_->GetStderr();
}

TYsonString TJobProxy::Strace(const TJobId& jobId)
{
    ValidateJobId(jobId);
    return Job_->StraceJob();
}

void TJobProxy::SignalJob(const TJobId& jobId, const Stroka& signalName)
{
    ValidateJobId(jobId);
    Job_->SignalJob(signalName);
}

TYsonString TJobProxy::PollJobShell(const TJobId& jobId, const TYsonString& parameters)
{
    ValidateJobId(jobId);
    return Job_->PollJobShell(parameters);
}

IServerPtr TJobProxy::GetRpcServer() const
{
    return RpcServer_;
}

void TJobProxy::ValidateJobId(const TJobId& jobId)
{
    if (JobId_ != jobId) {
        THROW_ERROR_EXCEPTION("Job id mismatch: expected %v, got %v",
            JobId_,
            jobId);
    }

    if (!Job_) {
        THROW_ERROR_EXCEPTION("Job has not started yet");
    }
}

void TJobProxy::SendHeartbeat()
{
    auto req = SupervisorProxy_->OnJobProgress();
    ToProto(req->mutable_job_id(), JobId_);
    req->set_progress(Job_->GetProgress());
    req->set_statistics(ConvertToYsonString(GetStatistics()).Data());

    req->Invoke().Subscribe(BIND(&TJobProxy::OnHeartbeatResponse, MakeWeak(this)));

    LOG_DEBUG("Supervisor heartbeat sent");
}

void TJobProxy::OnHeartbeatResponse(const TError& error)
{
    if (!error.IsOK()) {
        // NB: user process is not killed here.
        // Good user processes are supposed to die themselves
        // when io pipes are closed.
        // Bad processes will die at container shutdown.
        LOG_ERROR(error, "Error sending heartbeat to supervisor");
        Exit(EJobProxyExitCode::HeartbeatFailed);
    }

    LOG_DEBUG("Successfully reported heartbeat to supervisor");
}

void TJobProxy::RetrieveJobSpec()
{
    LOG_INFO("Requesting job spec");

    auto req = SupervisorProxy_->GetJobSpec();
    ToProto(req->mutable_job_id(), JobId_);

    auto rspOrError = req->Invoke().Get();
    if (!rspOrError.IsOK()) {
        LOG_ERROR(rspOrError, "Failed to get job spec");
        Exit(EJobProxyExitCode::GetJobSpecFailed);
    }

    const auto& rsp = rspOrError.Value();
    JobSpec_ = rsp->job_spec();
    const auto& resourceUsage = rsp->resource_usage();

    LOG_INFO("Job spec received (JobType: %v, ResourceLimits: {Cpu: %v, Memory: %v, Network: %v)\n%v",
        NScheduler::EJobType(rsp->job_spec().type()),
        resourceUsage.cpu(),
        resourceUsage.memory(),
        resourceUsage.network(),
        rsp->job_spec().DebugString());

    JobProxyMemoryReserve_ = resourceUsage.memory();
    CpuLimit_ = resourceUsage.cpu();
    NetworkUsage_ = resourceUsage.network();

    // We never report to node less memory usage, than was initially reserved.
    TotalMaxMemoryUsage_ = JobProxyMemoryReserve_;

    std::vector<Stroka> annotations{
        Format("OperationId: %v", OperationId_),
        Format("JobId: %v", JobId_),
        Format("JobType: %v", EJobType(JobSpec_.type()))
    };

    for (auto* descriptor : {
        &Config_->JobIO->TableReader->WorkloadDescriptor,
        &Config_->JobIO->TableWriter->WorkloadDescriptor,
        &Config_->JobIO->ErrorFileWriter->WorkloadDescriptor
    })
    {
        descriptor->Annotations.insert(
            descriptor->Annotations.end(),
            annotations.begin(),
            annotations.end());
    }
}

void TJobProxy::Run()
{
    auto startTime = Now();
    auto resultOrError = BIND(&TJobProxy::DoRun, Unretained(this))
        .AsyncVia(JobThread_->GetInvoker())
        .Run()
        .Get();
    auto finishTime = Now();

    TJobResult result;
    if (!resultOrError.IsOK()) {
        LOG_ERROR(resultOrError, "Job failed");
        ToProto(result.mutable_error(), resultOrError);
    } else {
        result = resultOrError.Value();
    }

    // Reliably terminate all async calls before reporting result.
    if (HeartbeatExecutor_) {
        WaitFor(HeartbeatExecutor_->Stop())
            .ThrowOnError();
    }

    if (MemoryWatchdogExecutor_) {
        WaitFor(MemoryWatchdogExecutor_->Stop())
            .ThrowOnError();
    }

    RpcServer_->Stop()
        .WithTimeout(RpcServerShutdownTimeout)
        .Get();

    TNullable<TYsonString> statistics;

    if (Job_) {
        auto failedChunkIds = Job_->GetFailedChunkIds();
        LOG_INFO("Found %v failed chunks", static_cast<int>(failedChunkIds.size()));

        // For erasure chunks, replace part id with whole chunk id.
        auto* schedulerResultExt = result.MutableExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
        for (const auto& chunkId : failedChunkIds) {
            auto actualChunkId = IsErasureChunkPartId(chunkId)
                ? ErasureChunkIdFromPartId(chunkId)
                : chunkId;
            ToProto(schedulerResultExt->add_failed_chunk_ids(), actualChunkId);
        }

        statistics = ConvertToYsonString(GetStatistics());
    }

    CheckResult(result);

    ReportResult(result, statistics, startTime, finishTime);
}

std::unique_ptr<IUserJobIO> TJobProxy::CreateUserJobIO()
{
    auto jobType = NScheduler::EJobType(JobSpec_.type());

    switch (jobType) {
        case NScheduler::EJobType::Map:
            return CreateMapJobIO(this);

        case NScheduler::EJobType::OrderedMap:
            return CreateOrderedMapJobIO(this);

        case NScheduler::EJobType::SortedReduce:
            return CreateSortedReduceJobIO(this);

        case NScheduler::EJobType::PartitionMap:
            return CreatePartitionMapJobIO(this);

        // ToDo(psushin): handle separately to form job result differently.
        case NScheduler::EJobType::ReduceCombiner:
        case NScheduler::EJobType::PartitionReduce:
            return CreatePartitionReduceJobIO(this);

        default:
            YUNREACHABLE();
    }
}

IJobPtr TJobProxy::CreateBuiltinJob()
{
    auto jobType = NScheduler::EJobType(JobSpec_.type());
    switch (jobType) {
        case NScheduler::EJobType::OrderedMerge:
            return CreateOrderedMergeJob(this);

        case NScheduler::EJobType::UnorderedMerge:
            return CreateUnorderedMergeJob(this);

        case NScheduler::EJobType::SortedMerge:
            return CreateSortedMergeJob(this);

        case NScheduler::EJobType::FinalSort:
        case NScheduler::EJobType::IntermediateSort:
            return CreatePartitionSortJob(this);

        case NScheduler::EJobType::SimpleSort:
            return CreateSimpleSortJob(this);

        case NScheduler::EJobType::Partition:
            return CreatePartitionJob(this);

        case NScheduler::EJobType::RemoteCopy:
            return CreateRemoteCopyJob(this);

        default:
            YUNREACHABLE();
    }
}

TJobResult TJobProxy::DoRun()
{
    try {
        Config_->Load(ConfigNode_);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing job proxy configuration")
            << ex;
    }

    auto environmentConfig = ConvertTo<TJobEnvironmentConfigPtr>(Config_->JobEnvironment);
    if (environmentConfig->Type == EJobEnvironmentType::Cgroups) {
        CGroupsConfig_ = ConvertTo<TCGroupJobEnvironmentConfigPtr>(Config_->JobEnvironment);
    }

    LocalDescriptor_ = NNodeTrackerClient::TNodeDescriptor(Config_->Addresses, Config_->Rack);

    RpcServer_ = CreateBusServer(CreateTcpBusServer(Config_->RpcServer));
    RpcServer_->RegisterService(CreateJobProberService(this));
    RpcServer_->Start();

    auto supervisorClient = CreateTcpBusClient(Config_->SupervisorConnection);
    auto supervisorChannel = CreateBusChannel(supervisorClient);

    SupervisorProxy_.reset(new TSupervisorServiceProxy(supervisorChannel));
    SupervisorProxy_->SetDefaultTimeout(Config_->SupervisorRpcTimeout);

    auto clusterConnection = CreateConnection(Config_->ClusterConnection);

    Client_ = clusterConnection->CreateClient(TClientOptions(NSecurityClient::JobUserName));

    RetrieveJobSpec();

    const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    NLFAlloc::SetBufferSize(schedulerJobSpecExt.lfalloc_buffer_size());
    JobProxyMemoryOvercommitLimit_ =
        schedulerJobSpecExt.has_job_proxy_memory_overcommit_limit() ?
        MakeNullable(schedulerJobSpecExt.job_proxy_memory_overcommit_limit()) :
        Null;

    RefCountedTrackerLogPeriod_ = FromProto<TDuration>(schedulerJobSpecExt.job_proxy_ref_counted_tracker_log_period());

    if (CGroupsConfig_ && CGroupsConfig_->IsCGroupSupported(TCpu::Name)) {
        auto cpuCGroup = GetCurrentCGroup<TCpu>();
        cpuCGroup.SetShare(CpuLimit_ * CpuShareMultiplier);
    }

    InputNodeDirectory_ = New<NNodeTrackerClient::TNodeDirectory>();
    InputNodeDirectory_->MergeFrom(schedulerJobSpecExt.input_node_directory());

    HeartbeatExecutor_ = New<TPeriodicExecutor>(
        GetSyncInvoker(),
        BIND(&TJobProxy::SendHeartbeat, MakeWeak(this)),
        Config_->HeartbeatPeriod);

    auto jobEnvironmentConfig = ConvertTo<TJobEnvironmentConfigPtr>(Config_->JobEnvironment);
    MemoryWatchdogExecutor_ = New<TPeriodicExecutor>(
        GetSyncInvoker(),
        BIND(&TJobProxy::CheckMemoryUsage, MakeWeak(this)),
        jobEnvironmentConfig->MemoryWatchdogPeriod);

    if (schedulerJobSpecExt.has_user_job_spec()) {
        auto& userJobSpec = schedulerJobSpecExt.user_job_spec();
        JobProxyMemoryReserve_ -= userJobSpec.memory_reserve();
        LOG_DEBUG("Adjusting job proxy memory limit (JobProxyMemoryReserve: %v, UserJobMemoryReserve: %v)",
            JobProxyMemoryReserve_,
            userJobSpec.memory_reserve());
        Job_ = CreateUserJob(
            this,
            userJobSpec,
            JobId_,
            CreateUserJobIO());
    } else {
        Job_ = CreateBuiltinJob();
    }

    Job_->Initialize();

    MemoryWatchdogExecutor_->Start();
    HeartbeatExecutor_->Start();

    return Job_->Run();
}

void TJobProxy::ReportResult(
    const TJobResult& result,
    const TNullable<TYsonString>& statistics,
    TInstant startTime,
    TInstant finishTime)
{
    if (!SupervisorProxy_) {
        LOG_ERROR("Supervisor channel is not available");
        Exit(EJobProxyExitCode::ResultReportFailed);
    }

    auto req = SupervisorProxy_->OnJobFinished();
    ToProto(req->mutable_job_id(), JobId_);
    *req->mutable_result() = result;
    if (statistics) {
        req->set_statistics(statistics->Data());
    }
    req->set_start_time(ToProto(startTime));
    req->set_finish_time(ToProto(finishTime));

    auto rspOrError = req->Invoke().Get();
    if (!rspOrError.IsOK()) {
        LOG_ERROR(rspOrError, "Failed to report job result");
        Exit(EJobProxyExitCode::ResultReportFailed);
    }
}

TStatistics TJobProxy::GetStatistics() const
{
    YCHECK(Job_);
    auto statistics = Job_->GetStatistics();

    if (CGroupsConfig_ && CGroupsConfig_->IsCGroupSupported(TCpuAccounting::Name)) {
        auto cpuAccounting = GetCurrentCGroup<TCpuAccounting>();
        auto cpuStatistics = cpuAccounting.GetStatistics();
        statistics.AddSample("/job_proxy/cpu", cpuStatistics);
    }

    if (CGroupsConfig_ && CGroupsConfig_->IsCGroupSupported(TBlockIO::Name)) {
        auto blockIO = GetCurrentCGroup<TBlockIO>();
        auto blockIOStatistics = blockIO.GetStatistics();
        statistics.AddSample("/job_proxy/block_io", blockIOStatistics);
    }

    statistics.AddSample("/job_proxy/max_memory", JobProxyMaxMemoryUsage_);
    statistics.AddSample("/job_proxy/memory_reserve", JobProxyMemoryReserve_);

    statistics.SetTimestamp(TInstant::Now());

    return statistics;
}

TCGroupJobEnvironmentConfigPtr TJobProxy::GetCGroupsConfig() const
{
    return CGroupsConfig_;
}

TJobProxyConfigPtr TJobProxy::GetConfig() const
{
    return Config_;
}

const TOperationId& TJobProxy::GetOperationId() const
{
    return OperationId_;
}

const TJobId& TJobProxy::GetJobId() const
{
    return JobId_;
}

const TJobSpec& TJobProxy::GetJobSpec() const
{
    return JobSpec_;
}

void TJobProxy::UpdateResourceUsage()
{
    // Fire-and-forget.
    auto req = SupervisorProxy_->UpdateResourceUsage();
    ToProto(req->mutable_job_id(), JobId_);
    auto* resourceUsage = req->mutable_resource_usage();
    resourceUsage->set_cpu(CpuLimit_);
    resourceUsage->set_network(NetworkUsage_);
    resourceUsage->set_memory(TotalMaxMemoryUsage_);
    req->Invoke().Subscribe(BIND(&TJobProxy::OnResourcesUpdated, MakeWeak(this)));
}

void TJobProxy::SetUserJobMemoryUsage(i64 memoryUsage)
{
    UserJobCurrentMemoryUsage_ = memoryUsage;
}

void TJobProxy::OnResourcesUpdated(const TError& error)
{
    if (!error.IsOK()) {
        LOG_ERROR(error, "Failed to update resource usage");
        Exit(EJobProxyExitCode::ResourcesUpdateFailed);
    }

    LOG_DEBUG("Successfully updated resource usage");
}

void TJobProxy::ReleaseNetwork()
{
    LOG_DEBUG("Releasing network");
    NetworkUsage_ = 0;
    UpdateResourceUsage();
}

void TJobProxy::OnPrepared()
{
    LOG_DEBUG("Job prepared");

    auto req = SupervisorProxy_->OnJobPrepared();
    ToProto(req->mutable_job_id(), JobId_);
    req->Invoke();
}

NApi::IClientPtr TJobProxy::GetClient() const
{
    return Client_;
}

IBlockCachePtr TJobProxy::GetBlockCache() const
{
    return GetNullBlockCache();
}

TNodeDirectoryPtr TJobProxy::GetInputNodeDirectory() const
{
    return InputNodeDirectory_;
}

const NNodeTrackerClient::TNodeDescriptor& TJobProxy::LocalDescriptor() const
{
    return LocalDescriptor_;
}

void TJobProxy::CheckMemoryUsage()
{
    i64 jobProxyMemoryUsage = GetProcessRss();
    JobProxyMaxMemoryUsage_ = std::max(JobProxyMaxMemoryUsage_.load(), jobProxyMemoryUsage);

    LOG_DEBUG("Job proxy memory check (JobProxyMemoryUsage: %v, JobProxyMaxMemoryUsage: %v, JobProxyMemoryReserve: %v, UserJobCurrentMemoryUsage: %v)",
        jobProxyMemoryUsage,
        JobProxyMaxMemoryUsage_.load(),
        JobProxyMemoryReserve_,
        UserJobCurrentMemoryUsage_.load());

    LOG_DEBUG("LFAlloc counters (LargeBlocks: %v, SmallBlocks: %v, System: %v, Used: %v, Mmapped: %v)",
        NLFAlloc::GetCurrentLargeBlocks(),
        NLFAlloc::GetCurrentSmallBlocks(),
        NLFAlloc::GetCurrentSystem(),
        NLFAlloc::GetCurrentUsed(),
        NLFAlloc::GetCurrentMmapped());

    if (JobProxyMaxMemoryUsage_.load() > JobProxyMemoryReserve_) {
        if (TInstant::Now() - LastRefCountedTrackerLogTime_ > RefCountedTrackerLogPeriod_) {
            LOG_WARNING("Job proxy used more memory than estimated "
                "(JobProxyMaxMemoryUsage: %v, JobProxyMemoryReserve: %v, RefCountedTracker: %v)",
                JobProxyMaxMemoryUsage_.load(),
                JobProxyMemoryReserve_,
                TRefCountedTracker::Get()->GetDebugInfo(2 /* sortByColumn */));
            LastRefCountedTrackerLogTime_ = TInstant::Now();
        }
    }

    if (JobProxyMemoryOvercommitLimit_ && jobProxyMemoryUsage > JobProxyMemoryReserve_ + *JobProxyMemoryOvercommitLimit_) {
        LOG_FATAL("Job proxy exceeded the memory overcommit limit "
            "(JobProxyMemoryUsage: %v, JobProxyMemoryReserve: %v, MemoryOvercommitLimit: %v, RefCountedTracker: %v)",
            jobProxyMemoryUsage,
            JobProxyMemoryReserve_,
            JobProxyMemoryOvercommitLimit_,
            TRefCountedTracker::Get()->GetDebugInfo(2 /* sortByColumn */));
    }

    i64 totalMemoryUsage = UserJobCurrentMemoryUsage_ + jobProxyMemoryUsage;

    if (TotalMaxMemoryUsage_ < totalMemoryUsage) {
        LOG_DEBUG("Total memory usage increased from %v to %v, asking node for resource usage update",
            TotalMaxMemoryUsage_,
            totalMemoryUsage);
        TotalMaxMemoryUsage_ = totalMemoryUsage;
        UpdateResourceUsage();
    }
}

void TJobProxy::CheckResult(const TJobResult& jobResult)
{
    const auto& schedulerJobSpecExt = JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
    const auto& schedulerJobResultExt = jobResult.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
    const auto& userJobSpec = schedulerJobSpecExt.user_job_spec();

    // If we were provided with stderr_table_spec we are expected to write stderr and provide some results.
    YCHECK(!userJobSpec.has_stderr_table_spec() || schedulerJobResultExt.has_stderr_table_boundary_keys());
}

void TJobProxy::Exit(EJobProxyExitCode exitCode)
{
    if (Job_) {
        Job_->Abort();
    }

    NLogging::TLogManager::Get()->Shutdown();
    _exit(static_cast<int>(exitCode));
}

NLogging::TLogger TJobProxy::GetLogger() const
{
    return Logger;
}

IInvokerPtr TJobProxy::GetControlInvoker() const
{
    return ControlThread_->GetInvoker();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
