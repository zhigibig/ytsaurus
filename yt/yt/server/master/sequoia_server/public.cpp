#include "public.h"

namespace NYT::NSequoiaServer {

////////////////////////////////////////////////////////////////////////////////

EAevum GetCurrentAevum()
{
    return TEnumTraits<EAevum>::GetMaxValue();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSequoiaServer
