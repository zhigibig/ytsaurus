#pragma once

#include "private.h"

#include <Storages/IStorage.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

DB::StoragePtr CreateStorageStub(TTablePtr table);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
