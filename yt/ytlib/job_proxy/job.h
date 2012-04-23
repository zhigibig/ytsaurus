﻿#pragma once

#include <ytlib/scheduler/job.pb.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

struct IJob
{
    virtual ~IJob();

    virtual NScheduler::NProto::TJobResult Run() = 0;
    // virtual TProgress GetProgress() = 0 const;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
