#pragma once

#include <yt/server/clickhouse/interop/api.h>

#include <string>

namespace NYT {
namespace NClickHouse {

////////////////////////////////////////////////////////////////////////////////

NInterop::IServerPtr CreateServer(
    NInterop::ILoggerPtr logger,
    NInterop::IStoragePtr storage,
    NInterop::ICoordinationServicePtr coordinationService,
    std::string configFile,
    ui16 rpcPort,
    ui16 tcpPort);

}   // namespace NClickHouse
}   // namespace NYT
