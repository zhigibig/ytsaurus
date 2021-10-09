#pragma once

#include "public.h"
#include "object_detail.h"
#include "type_handler_detail.h"

#include <yt/yt/server/master/cell_master/public.h>

namespace NYT::NObjectServer {

////////////////////////////////////////////////////////////////////////////////

class TMasterObject
    : public TNonversionedObjectBase
{
public:
    explicit TMasterObject(TObjectId id);

    TString GetLowercaseObjectName() const override;
    TString GetCapitalizedObjectName() const override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectServer
