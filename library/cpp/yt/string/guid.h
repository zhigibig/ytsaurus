#include <library/cpp/yt/misc/guid.h>

#include "format.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

void FormatValue(TStringBuilderBase* builder, TGuid value, TStringBuf /*format*/);
TString ToString(TGuid guid);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
