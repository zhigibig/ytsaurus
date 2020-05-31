#pragma once

#include "public.h"

#include <yt/client/table_client/public.h>
#include <yt/client/table_client/versioned_reader.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

using TSchemafulReaderFactory = std::function<ISchemafulUnversionedReaderPtr(
    const TTableSchema&,
    const TColumnFilter&)>;

IVersionedReaderPtr CreateVersionedReaderAdapter(
    TSchemafulReaderFactory createReader,
    const TTableSchema& schema,
    const TColumnFilter& columnFilter,
    TTimestamp timestamp);

////////////////////////////////////////////////////////////////////////////////

IVersionedReaderPtr CreateTimestampResettingAdapter(
    IVersionedReaderPtr undlerlyingReader,
    TTimestamp timestamp);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
