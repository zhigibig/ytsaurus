#ifndef STRING_INL_H_
#error "Direct inclusion of this file is not allowed, include string.h"
// For the sake of sane code completion.
#include "string.h"
#endif

namespace NYT::NYson {

////////////////////////////////////////////////////////////////////////////////

namespace NDetail {

template <typename TLeft, typename TRight>
bool Equals(const TLeft& lhs, const TRight& rhs)
{
    auto lhsNull = !lhs.operator bool();
    auto rhsNull = !rhs.operator bool();
    if (lhsNull != rhsNull) {
        return false;
    }
    if (lhsNull && rhsNull) {
        return true;
    }
    return
        lhs.AsStringBuf() == rhs.AsStringBuf() &&
        lhs.GetType() == rhs.GetType();
}

} // namespace NDetail

inline bool operator == (const TYsonString& lhs, const TYsonString& rhs)
{
    return NDetail::Equals(lhs, rhs);
}

inline bool operator == (const TYsonString& lhs, const TYsonStringBuf& rhs)
{
    return NDetail::Equals(lhs, rhs);
}

inline bool operator == (const TYsonStringBuf& lhs, const TYsonString& rhs)
{
    return NDetail::Equals(lhs, rhs);
}

inline bool operator == (const TYsonStringBuf& lhs, const TYsonStringBuf& rhs)
{
    return NDetail::Equals(lhs, rhs);
}

inline bool operator != (const TYsonString& lhs, const TYsonString& rhs)
{
    return !(lhs == rhs);
}

inline bool operator != (const TYsonString& lhs, const TYsonStringBuf& rhs)
{
    return !(lhs == rhs);
}

inline bool operator != (const TYsonStringBuf& lhs, const TYsonString& rhs)
{
    return !(lhs == rhs);
}

inline bool operator != (const TYsonStringBuf& lhs, const TYsonStringBuf& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYson

//! A hasher for TYsonString
template <>
struct THash<NYT::NYson::TYsonString>
{
    size_t operator () (const NYT::NYson::TYsonString& str) const
    {
        return str.ComputeHash();
    }
};

//! A hasher for TYsonStringBuf
template <>
struct THash<NYT::NYson::TYsonStringBuf>
{
    size_t operator () (const NYT::NYson::TYsonStringBuf& str) const
    {
        return THash<TStringBuf>()(str.AsStringBuf());
    }
};
