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

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/connection.h>

#include <yt/ytlib/cgroup/cgroup.h>

#include <yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/ytlib/chunk_client/config.h>
#include <yt/ytlib/chunk_client/data_statistics.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

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

////////////////////////////////////////////////////////////////////////////////

TJobProxy::TJobProxy(
    INodePtr configNode,
    const TJobId& jobId)
    : ConfigNode_(configNode)
    , JobId_(jobId)
    , JobThread_(New<TActionQueue>("JobMain"))
    , ControlThread_(New<TActionQueue>("Control"))
    , Logger(JobProxyLogger)
{
    Logger.AddTag("JobId: %v", JobId_);
}

std::vector<NChunkClient::TChunkId> TJobProxy::DumpInputContext(const TJobId& jobId)
{
    ValidateJobId(jobId);
    return Job_->DumpInputContext();
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
    ToProto(req->mutable_statistics(), GetStatistics());

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
        Exit(EJobProxyExitCode::HeartbeatFailed);
    }

    const auto& rsp = rspOrError.Value();
    JobSpec_ = rsp->job_spec();
    ResourceUsage_ = rsp->resource_usage();

    LOG_INFO("Job spec received (JobType: %v, ResourceLimits: %v)\n%v",
        NScheduler::EJobType(rsp->job_spec().type()),
        FormatResources(ResourceUsage_),
        rsp->job_spec().DebugString());

    JobProxyInitialMemoryLimit_ = ResourceUsage_.memory();
    CpuLimit_ = ResourceUsage_.cpu();
}

void TJobProxy::Run()
{
    auto resultOrError = BIND(&TJobProxy::DoRun, Unretained(this))
        .AsyncVia(JobThread_->GetInvoker())
        .Run()
        .Get();

    TJobResult result;
    if (!resultOrError.IsOK()) {
        LOG_ERROR(resultOrError, "Job failed");
        ToProto(result.mutable_error(), resultOrError);
    } else {
        result = resultOrError.Value();
    }

    if (HeartbeatExecutor_) {
        HeartbeatExecutor_->Stop();
    }

    if (MemoryWatchdogExecutor_) {
        MemoryWatchdogExecutor_->Stop();
    }

    RpcServer_->Stop()
        .WithTimeout(RpcServerShutdownTimeout)
        .Get();

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

        ToProto(result.mutable_statistics(), GetStatistics());
    } else {
        result.mutable_statistics();
    }

    ReportResult(result);
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

    if (Config_->IsCGroupSupported(TCpu::Name)) {
        auto cpuCGroup = GetCurrentCGroup<TCpu>();
        cpuCGroup.SetShare(CpuLimit_);
    }

    InputNodeDirectory_ = New<NNodeTrackerClient::TNodeDirectory>();
    InputNodeDirectory_->MergeFrom(schedulerJobSpecExt.input_node_directory());

    AuxNodeDirectory_ = New<NNodeTrackerClient::TNodeDirectory>();
    if (schedulerJobSpecExt.has_aux_node_directory()) {
        AuxNodeDirectory_->MergeFrom(schedulerJobSpecExt.aux_node_directory());
    }

    HeartbeatExecutor_ = New<TPeriodicExecutor>(
        GetSyncInvoker(),
        BIND(&TJobProxy::SendHeartbeat, MakeWeak(this)),
        Config_->HeartbeatPeriod);

    MemoryWatchdogExecutor_ = New<TPeriodicExecutor>(
        GetSyncInvoker(),
        BIND(&TJobProxy::CheckMemoryUsage, MakeWeak(this)),
        Config_->MemoryWatchdogPeriod);

    if (schedulerJobSpecExt.has_user_job_spec()) {
        auto& userJobSpec = schedulerJobSpecExt.user_job_spec();
        JobProxyInitialMemoryLimit_ -= userJobSpec.memory_reserve();
        Job_ = CreateUserJob(
            this,
            userJobSpec,
            JobId_,
            CreateUserJobIO());
    } else {
        Job_ = CreateBuiltinJob();
    }

    JobProxyCurrentMemoryLimit_ = JobProxyInitialMemoryLimit_;

    Job_->Initialize();

    MemoryWatchdogExecutor_->Start();
    HeartbeatExecutor_->Start();

    return Job_->Run();
}

void TJobProxy::ReportResult(const TJobResult& result)
{
    auto req = SupervisorProxy_->OnJobFinished();
    ToProto(req->mutable_job_id(), JobId_);
    *req->mutable_result() = result;

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

    if (Config_->IsCGroupSupported(TCpuAccounting::Name)) {
        auto cpuAccounting = GetCurrentCGroup<TCpuAccounting>();
        auto cpuStatistics = cpuAccounting.GetStatistics();
        statistics.AddSample("/job_proxy/cpu", cpuStatistics);
    }

    if (Config_->IsCGroupSupported(TBlockIO::Name)) {
        auto blockIO = GetCurrentCGroup<TBlockIO>();
        auto blockIOStatistics = blockIO.GetStatistics();
        statistics.AddSample("/job_proxy/block_io", blockIOStatistics);
    }

    statistics.AddSample("/job_proxy/max_memory", MaxMemoryUsage_);
    statistics.AddSample("/job_proxy/memory_limit", JobProxyInitialMemoryLimit_);

    return statistics;
}

TJobProxyConfigPtr TJobProxy::GetConfig()
{
    return Config_;
}

const TJobSpec& TJobProxy::GetJobSpec() const
{
    return JobSpec_;
}

const TNodeResources& TJobProxy::GetResourceUsage() const
{
    return ResourceUsage_;
}

void TJobProxy::SetResourceUsage(const TNodeResources& usage)
{
    ResourceUsage_ = usage;

    // Fire-and-forget.
    auto req = SupervisorProxy_->UpdateResourceUsage();
    ToProto(req->mutable_job_id(), JobId_);
    *req->mutable_resource_usage() = ResourceUsage_;
    req->Invoke().Subscribe(BIND(&TJobProxy::OnResourcesUpdated, MakeWeak(this)));
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
    auto usage = GetResourceUsage();
    usage.set_network(0);
    SetResourceUsage(usage);
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

TNodeDirectoryPtr TJobProxy::GetAuxNodeDirectory() const
{
    return AuxNodeDirectory_;
}

const NNodeTrackerClient::TNodeDescriptor& TJobProxy::LocalDescriptor() const
{
    return LocalDescriptor_;
}

void TJobProxy::CheckMemoryUsage()
{
    auto memoryUsage = GetProcessRss();
    MaxMemoryUsage_ = std::max(MaxMemoryUsage_.load(), memoryUsage);

    LOG_DEBUG("Job proxy memory check (MemoryUsage: %v, MemoryLimit: %v)",
        memoryUsage,
        JobProxyInitialMemoryLimit_);

    LOG_DEBUG("LFAlloc counters (LargeBlocks: %v, SmallBlocks: %v, System: %v, Used: %v, Mmapped: %v)",
        NLFAlloc::GetCurrentLargeBlocks(),
        NLFAlloc::GetCurrentSmallBlocks(),
        NLFAlloc::GetCurrentSystem(),
        NLFAlloc::GetCurrentUsed(),
        NLFAlloc::GetCurrentMmapped());

    if (JobProxyMemoryOvercommitLimit_ && memoryUsage > JobProxyInitialMemoryLimit_ + *JobProxyMemoryOvercommitLimit_) {
        LOG_FATAL("Job proxy exceeded the memory overcommit limit "
            "(MemoryUsage: %v, MemoryLimit: %v, CurrentMemoryLimit: %v, MemoryOvercommitLimit: %v, RefCountedTrakcer: %v)",
            memoryUsage,
            JobProxyInitialMemoryLimit_,
            JobProxyCurrentMemoryLimit_,
            JobProxyMemoryOvercommitLimit_,
            TRefCountedTracker::Get()->GetDebugInfo(2 /* sortByColumn */));
    }

    if (memoryUsage > JobProxyCurrentMemoryLimit_) {
        LOG_WARNING("Job proxy current memory limit exceeded, increasing current memory limit "
            "(MemoryUsage: %v, MemoryLimit: %v, CurrentMemoryLimit: %v, RefCountedTracker: %v)",
            memoryUsage,
            JobProxyInitialMemoryLimit_,
            JobProxyCurrentMemoryLimit_,
            TRefCountedTracker::Get()->GetDebugInfo(2 /* sortByColumn */));
        auto newResourceUsage = ResourceUsage_;
        auto delta = memoryUsage - JobProxyCurrentMemoryLimit_;
        newResourceUsage.set_memory(newResourceUsage.memory() + delta);
        JobProxyCurrentMemoryLimit_ += delta;
        SetResourceUsage(newResourceUsage);
    }
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
