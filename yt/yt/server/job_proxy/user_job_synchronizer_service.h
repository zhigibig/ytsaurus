#pragma once

#include <yt/yt/server/lib/user_job_synchronizer_client/public.h>

namespace NYT::NJobProxy {

////////////////////////////////////////////////////////////////////////////////

struct TExecutorInfo
{
    pid_t ProcessPid = 0;
};

NRpc::IServicePtr CreateUserJobSynchronizerService(
    const NLogging::TLogger& logger,
    TPromise<TExecutorInfo> executorPreparedPromise,
    IInvokerPtr controlInvoker);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
