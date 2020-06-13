#pragma once

#include "public.h"

#include <yt/client/object_client/public.h>

#include <yt/client/table_client/schema.h>

#include <yt/client/hydra/public.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct TTabletSnapshot
	: public TIntrinsicRefCounted
{
	NObjectClient::TObjectId TableId;
    NHydra::TRevision MountRevision = NHydra::NullRevision;
    NTableClient::TTableSchemaPtr TableSchema;
};

DEFINE_REFCOUNTED_TYPE(TTabletSnapshot)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
