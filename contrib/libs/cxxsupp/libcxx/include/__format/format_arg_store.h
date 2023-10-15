// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___FORMAT_FORMAT_ARG_STORE_H
#define _LIBCPP___FORMAT_FORMAT_ARG_STORE_H

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

#include <__config>
#include <__format/concepts.h>
#include <__format/format_arg.h>
#include <__iterator/data.h>
#include <__iterator/size.h>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

_LIBCPP_BEGIN_NAMESPACE_STD

#if _LIBCPP_STD_VER > 17

namespace __format {

/// \returns The @c __arg_t based on the type of the formatting argument.
///
/// \pre \c __formattable<_Tp, typename _Context::char_type>
template <class _Context, class _Tp>
consteval __arg_t __determine_arg_t();

// Boolean
template <class, same_as<bool> _Tp>
consteval __arg_t __determine_arg_t() {
  return __arg_t::__boolean;
}

// Char
template <class _Context, same_as<typename _Context::char_type> _Tp>
consteval __arg_t __determine_arg_t() {
  return __arg_t::__char_type;
}
#  ifndef _LIBCPP_HAS_NO_WIDE_CHARACTERS
template <class _Context, class _CharT>
  requires(same_as<typename _Context::char_type, wchar_t> && same_as<_CharT, char>)
consteval __arg_t __determine_arg_t() {
  return __arg_t::__char_type;
}
#  endif

// Signed integers
template <class, __libcpp_signed_integer _Tp>
consteval __arg_t __determine_arg_t() {
  if constexpr (sizeof(_Tp) <= sizeof(int))
    return __arg_t::__int;
  else if constexpr (sizeof(_Tp) <= sizeof(long long))
    return __arg_t::__long_long;
#  ifndef _LIBCPP_HAS_NO_INT128
  else if constexpr (sizeof(_Tp) == sizeof(__int128_t))
    return __arg_t::__i128;
#  endif
  else
    static_assert(sizeof(_Tp) == 0, "an unsupported signed integer was used");
}

// Unsigned integers
template <class, __libcpp_unsigned_integer _Tp>
consteval __arg_t __determine_arg_t() {
  if constexpr (sizeof(_Tp) <= sizeof(unsigned))
    return __arg_t::__unsigned;
  else if constexpr (sizeof(_Tp) <= sizeof(unsigned long long))
    return __arg_t::__unsigned_long_long;
#  ifndef _LIBCPP_HAS_NO_INT128
  else if constexpr (sizeof(_Tp) == sizeof(__uint128_t))
    return __arg_t::__u128;
#  endif
  else
    static_assert(sizeof(_Tp) == 0, "an unsupported unsigned integer was used");
}

// Floating-point
template <class, same_as<float> _Tp>
consteval __arg_t __determine_arg_t() {
  return __arg_t::__float;
}
template <class, same_as<double> _Tp>
consteval __arg_t __determine_arg_t() {
  return __arg_t::__double;
}
template <class, same_as<long double> _Tp>
consteval __arg_t __determine_arg_t() {
  return __arg_t::__long_double;
}

// Char pointer
template <class _Context, class _Tp>
  requires(same_as<typename _Context::char_type*, _Tp> || same_as<const typename _Context::char_type*, _Tp>)
consteval __arg_t __determine_arg_t() {
  return __arg_t::__const_char_type_ptr;
}

// Char array
template <class _Context, class _Tp>
  requires(is_array_v<_Tp> && same_as<_Tp, typename _Context::char_type[extent_v<_Tp>]>)
consteval __arg_t __determine_arg_t() {
  return __arg_t::__string_view;
}

// String view
template <class _Context, class _Tp>
  requires(same_as<typename _Context::char_type, typename _Tp::value_type> &&
           same_as<_Tp, basic_string_view<typename _Tp::value_type, typename _Tp::traits_type>>)
consteval __arg_t __determine_arg_t() {
  return __arg_t::__string_view;
}

// String
template <class _Context, class _Tp>
  requires(
      same_as<typename _Context::char_type, typename _Tp::value_type> &&
      same_as<_Tp, basic_string<typename _Tp::value_type, typename _Tp::traits_type, typename _Tp::allocator_type>>)
consteval __arg_t __determine_arg_t() {
  return __arg_t::__string_view;
}

// Pointers
template <class, class _Ptr>
  requires(same_as<_Ptr, void*> || same_as<_Ptr, const void*> || same_as<_Ptr, nullptr_t>)
consteval __arg_t __determine_arg_t() {
  return __arg_t::__ptr;
}

// Handle
//
// Note this version can't be constrained avoiding ambiguous overloads.
// That means it can be instantiated by disabled formatters. To solve this, a
// constrained version for not formattable formatters is added. That overload
// is marked as deleted to fail creating a storage type for disabled formatters.
template <class _Context, class _Tp>
consteval __arg_t __determine_arg_t() {
  return __arg_t::__handle;
}

template <class _Context, class _Tp>
  requires(!__formattable<_Tp, typename _Context::char_type>)
consteval __arg_t __determine_arg_t() = delete;

template <class _Context, class _Tp>
_LIBCPP_HIDE_FROM_ABI basic_format_arg<_Context> __create_format_arg(_Tp&& __value) noexcept {
  constexpr __arg_t __arg = __determine_arg_t<_Context, remove_cvref_t<_Tp>>();
  static_assert(__arg != __arg_t::__none);

  // Not all types can be used to directly initialize the
  // __basic_format_arg_value.  First handle all types needing adjustment, the
  // final else requires no adjustment.
  if constexpr (__arg == __arg_t::__char_type)
    // On some platforms initializing a wchar_t from a char is a narrowing
    // conversion.
    return basic_format_arg<_Context>{__arg, static_cast<typename _Context::char_type>(__value)};
  else if constexpr (__arg == __arg_t::__int)
    return basic_format_arg<_Context>{__arg, static_cast<int>(__value)};
  else if constexpr (__arg == __arg_t::__long_long)
    return basic_format_arg<_Context>{__arg, static_cast<long long>(__value)};
  else if constexpr (__arg == __arg_t::__unsigned)
    return basic_format_arg<_Context>{__arg, static_cast<unsigned>(__value)};
  else if constexpr (__arg == __arg_t::__unsigned_long_long)
    return basic_format_arg<_Context>{__arg, static_cast<unsigned long long>(__value)};
  else if constexpr (__arg == __arg_t::__string_view)
    // When the _Traits or _Allocator are different an implicit conversion will
    // fail.
    //
    // Note since the input can be an array use the non-member functions to
    // extract the constructor arguments.
    return basic_format_arg<_Context>{
        __arg, basic_string_view<typename _Context::char_type>{_VSTD::data(__value), _VSTD::size(__value)}};
  else if constexpr (__arg == __arg_t::__ptr)
    return basic_format_arg<_Context>{__arg, static_cast<const void*>(__value)};
  else if constexpr (__arg == __arg_t::__handle)
    return basic_format_arg<_Context>{
        __arg, typename __basic_format_arg_value<_Context>::__handle{_VSTD::forward<_Tp>(__value)}};
  else
    return basic_format_arg<_Context>{__arg, __value};
}

template <class _Context, class... _Args>
_LIBCPP_HIDE_FROM_ABI void __create_packed_storage(uint64_t& __types, __basic_format_arg_value<_Context>* __values,
                                                   _Args&&... __args) noexcept {
  int __shift = 0;
  (
      [&] {
        basic_format_arg<_Context> __arg = __create_format_arg<_Context>(_VSTD::forward<_Args>(__args));
        if (__shift != 0)
          __types |= static_cast<uint64_t>(__arg.__type_) << __shift;
        else
          // Assigns the initial value.
          __types = static_cast<uint64_t>(__arg.__type_);
        __shift += __packed_arg_t_bits;
        *__values++ = __arg.__value_;
      }(),
      ...);
}

template <class _Context, class... _Args>
_LIBCPP_HIDE_FROM_ABI void __store_basic_format_arg(basic_format_arg<_Context>* __data, _Args&&... __args) noexcept {
  ([&] { *__data++ = __create_format_arg<_Context>(_VSTD::forward<_Args>(__args)); }(), ...);
}

template <class _Context, size_t N>
struct __packed_format_arg_store {
  __basic_format_arg_value<_Context> __values_[N];
  uint64_t __types_;
};

template <class _Context, size_t N>
struct __unpacked_format_arg_store {
  basic_format_arg<_Context> __args_[N];
};

} // namespace __format

template <class _Context, class... _Args>
struct _LIBCPP_TEMPLATE_VIS __format_arg_store {
  _LIBCPP_HIDE_FROM_ABI
  __format_arg_store(_Args&&... __args) noexcept {
    if constexpr (sizeof...(_Args) != 0) {
      if constexpr (__format::__use_packed_format_arg_store(sizeof...(_Args)))
        __format::__create_packed_storage(__storage.__types_, __storage.__values_, _VSTD::forward<_Args>(__args)...);
      else
        __format::__store_basic_format_arg<_Context>(__storage.__args_, _VSTD::forward<_Args>(__args)...);
    }
  }

  using _Storage = conditional_t<__format::__use_packed_format_arg_store(sizeof...(_Args)),
                                 __format::__packed_format_arg_store<_Context, sizeof...(_Args)>,
                                 __format::__unpacked_format_arg_store<_Context, sizeof...(_Args)>>;

  _Storage __storage;
};

#endif //_LIBCPP_STD_VER > 17

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___FORMAT_FORMAT_ARG_STORE_H
