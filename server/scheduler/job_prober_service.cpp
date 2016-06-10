#include "job_prober_service.h"
#include "private.h"
#include "scheduler.h"

#include <yt/server/cell_scheduler/bootstrap.h>

#include <yt/ytlib/job_prober_client/job_signal.h>

#include <yt/ytlib/scheduler/job_prober_service_proxy.h>

#include <yt/core/rpc/service_detail.h>

namespace NYT {
namespace NScheduler {

using namespace NRpc;
using namespace NCellScheduler;
using namespace NConcurrency;
using namespace NJobProberClient;
using namespace NYson;

////////////////////////////////////////////////////////////////////

class TJobProberService
    : public TServiceBase
{
public:
    TJobProberService(TBootstrap* bootstrap)
        : TServiceBase(
            bootstrap->GetControlInvoker(),
            TJobProberServiceProxy::GetServiceName(),
            SchedulerLogger,
            TJobProberServiceProxy::GetProtocolVersion())
        , Bootstrap_(bootstrap)
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(DumpInputContext));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Strace));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(SignalJob));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AbandonJob));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(PollJobShell));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AbortJob));
    }

private:
    TBootstrap* const Bootstrap_;

    DECLARE_RPC_SERVICE_METHOD(NProto, DumpInputContext)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        const auto& path = request->path();
        context->SetRequestInfo("JobId: %v, Path: %v",
            jobId,
            path);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        WaitFor(scheduler->DumpInputContext(jobId, path))
            .ThrowOnError();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, Strace)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        context->SetRequestInfo("JobId: %v", jobId);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        auto trace = WaitFor(scheduler->Strace(jobId))
            .ValueOrThrow();

        context->SetResponseInfo("Trace: %v", trace.Data());

        ToProto(response->mutable_trace(), trace.Data());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, SignalJob)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        const auto& signalName = request->signal_name();

        ValidateSignalName(request->signal_name());

        context->SetRequestInfo("JobId: %v, SignalName: %v",
            jobId,
            signalName);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        WaitFor(scheduler->SignalJob(jobId, signalName))
            .ThrowOnError();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, AbandonJob)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        context->SetRequestInfo("JobId: %v", jobId);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        WaitFor(scheduler->AbandonJob(jobId))
            .ThrowOnError();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, PollJobShell)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        const auto& parameters = request->parameters();

        context->SetRequestInfo("JobId: %v, Parameters: %v",
            jobId,
            parameters);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        auto result = WaitFor(scheduler->PollJobShell(jobId, TYsonString(parameters)))
            .ValueOrThrow();

        ToProto(response->mutable_result(), result.Data());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NProto, AbortJob)
    {
        auto jobId = FromProto<TJobId>(request->job_id());
        context->SetRequestInfo("JobId: %v", jobId);

        auto scheduler = Bootstrap_->GetScheduler();
        scheduler->ValidateConnected();

        WaitFor(scheduler->AbortJob(jobId))
            .ThrowOnError();

        context->Reply();
    }
};

IServicePtr CreateJobProberService(TBootstrap* bootstrap)
{
    return New<TJobProberService>(bootstrap);
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
