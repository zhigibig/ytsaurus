#pragma once

#include <yt/yt/core/misc/public.h>

namespace NYT::NJobProxy {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ERROR_ENUM(
    ((MemoryLimitExceeded)       (1200))
    ((MemoryCheckFailed)         (1201))
    ((JobTimeLimitExceeded)      (1202))
    ((UnsupportedJobType)        (1203))
    ((JobNotPrepared)            (1204))
    ((UserJobFailed)             (1205))
    ((UserJobProducedCoreFiles)  (1206))
    ((ShallowMergeFailed)        (1207))
);

DECLARE_REFCOUNTED_STRUCT(IJobSpecHelper)
DECLARE_REFCOUNTED_STRUCT(IUserJobIOFactory)
DECLARE_REFCOUNTED_STRUCT(IUserJobReadController)
DECLARE_REFCOUNTED_CLASS(TJobTestingOptions)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
