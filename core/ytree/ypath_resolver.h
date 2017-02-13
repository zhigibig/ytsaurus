#pragma once

#include <yt/core/ypath/public.h>

#include <yt/core/misc/nullable.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

TNullable<i64> TryGetInt64(const TStringBuf& yson, const NYPath::TYPath& ypath);
TNullable<ui64> TryGetUint64(const TStringBuf& yson, const NYPath::TYPath& ypath);
TNullable<bool> TryGetBoolean(const TStringBuf& yson, const NYPath::TYPath& ypath);
TNullable<double> TryGetDouble(const TStringBuf& yson, const NYPath::TYPath& ypath);
TNullable<Stroka> TryGetString(const TStringBuf& yson, const NYPath::TYPath& ypath);

} // namespace NYTree
} // namespace NYT
