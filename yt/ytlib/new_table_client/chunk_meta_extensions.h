#pragma once

#include <core/misc/protobuf_helpers.h>

#include <ytlib/new_table_client/chunk_meta.pb.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

DECLARE_PROTO_EXTENSION(NVersionedTableClient::NProto::TTableSchema, 17)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

