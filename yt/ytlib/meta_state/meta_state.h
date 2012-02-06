#pragma once

#include "common.h"

#include <ytlib/actions/future.h>
#include <ytlib/misc/ref.h>

#include <util/stream/input.h>
#include <util/stream/output.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

struct IMetaState
    : public virtual TRefCounted
{
    typedef TIntrusivePtr<IMetaState> TPtr;

    virtual void Save(TOutputStream* output) = 0;
    virtual void Load(TInputStream* input) = 0;

    virtual void ApplyChange(const TRef& changeData) = 0;
    virtual void Clear() = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
