﻿#pragma once

#include "public.h"

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

IJobPtr CreateSimpleSortJob(IJobHost* host);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
