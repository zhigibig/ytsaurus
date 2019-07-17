#pragma once

#include <yt/ytlib/table_client/public.h>

#include <yt/client/table_client/schema.h>

#include <yt/core/logging/log.h>

#include <DataStreams/IBlockInputStream.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

DB::BlockInputStreamPtr CreateBlockInputStream(
    NTableClient::ISchemalessReaderPtr reader,
    NTableClient::TTableSchema readSchema,
    NLogging::TLogger logger);

////////////////////////////////////////////////////////////////////////////////

}
