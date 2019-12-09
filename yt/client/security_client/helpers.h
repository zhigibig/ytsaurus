#pragma once

#include "acl.h"
#include "public.h"

#include <yt/client/ypath/public.h>

#include <yt/core/misc/error.h>

namespace NYT::NSecurityClient {

////////////////////////////////////////////////////////////////////////////////

NYPath::TYPath GetUserPath(const TString& name);
NYPath::TYPath GetGroupPath(const TString& name);

////////////////////////////////////////////////////////////////////////////////

ESecurityAction CheckPermissionsByAclAndSubjectClosure(
    const TSerializableAccessControlList& acl,
    const THashSet<TString>& subjectClosure,
    NYTree::EPermissionSet permissions);

void ValidateSecurityTag(const TSecurityTag& tag);
void ValidateSecurityTags(const std::vector<TSecurityTag>& tags);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityClient

