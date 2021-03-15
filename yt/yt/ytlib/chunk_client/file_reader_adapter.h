#pragma once

#include "public.h"

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

IChunkReaderAllowingRepairPtr CreateFileReaderAdapter(TFileReaderPtr underlying);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
