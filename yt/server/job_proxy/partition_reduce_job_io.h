﻿#pragma once

#include "public.h"

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////

std::unique_ptr<IUserJobIO> CreatePartitionReduceJobIO(IJobHost* host);

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
