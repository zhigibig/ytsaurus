#pragma once

#include <yt/client/table_client/public.h>
#include <yt/core/yson/public.h>

#include <vector>

namespace NYT::NComplexTypes {

////////////////////////////////////////////////////////////////////////////////

Y_FORCE_INLINE void EnsureYsonToken(
    const NTableClient::TComplexTypeFieldDescriptor& descriptor,
    NYson::EYsonItemType actual,
    NYson::EYsonItemType expected);

Y_FORCE_INLINE void EnsureYsonToken(
    const NTableClient::TComplexTypeFieldDescriptor& descriptor,
    const NYson::TYsonPullParserCursor& cursor,
    NYson::EYsonItemType expected);

void ThrowUnexpectedYsonTokenException(
    const NTableClient::TComplexTypeFieldDescriptor& descriptor,
    NYson::EYsonItemType actual,
    const std::vector<NYson::EYsonItemType>& expected);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NComplexTypes

#define CHECK_YSON_TOKEN_INL_H_
#include "check_yson_token-inl.h"
#undef CHECK_YSON_TOKEN_INL_H_
