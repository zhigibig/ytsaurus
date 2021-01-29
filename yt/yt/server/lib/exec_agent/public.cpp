#include "public.h"

namespace NYT::NExecAgent {

////////////////////////////////////////////////////////////////////////////////

const TEnumIndexedVector<ESandboxKind, TString> SandboxDirectoryNames{
    "sandbox",
    "udf",
    "home",
    "pipes",
    "tmp",
    "cores",
    "logs"
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NExecAgent

