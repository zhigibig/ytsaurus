#pragma once

#include "public.h"

#include <yt/client/hydra/public.h>
#include <yt/client/object_client/public.h>

#include <yt/core/concurrency/public.h>

namespace NYT::NApi {

////////////////////////////////////////////////////////////////////////////////

struct IFileReader
    : public NConcurrency::IAsyncZeroCopyInputStream
{
    //! Returns ID of file node.
    virtual NObjectClient::TObjectId GetId() const = 0;

    //! Returns revision of file node.
    virtual NHydra::TRevision GetRevision() const = 0;
};

DEFINE_REFCOUNTED_TYPE(IFileReader)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi
