#pragma once

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

int GetVersionMajor();
int GetVersionMinor();
int GetVersionPatch();
const char* GetBranch();
const char* GetVersion();
const char* GetBuildHost();
const char* GetBuildTime();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

