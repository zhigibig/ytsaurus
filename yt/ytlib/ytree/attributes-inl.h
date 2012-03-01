#ifndef ATTRIBUTES_INL_H_
#error "Direct inclusion of this file is not allowed, include attributes.h"
#endif
#undef ATTRIBUTES_INL_H_

#include "serialize.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

template <class T>
typename TDeserializeTraits<T>::TReturnType IAttributeDictionary::Get(const Stroka& name) const
{
    const auto& yson = GetYson(name);
    return DeserializeFromYson<T>(yson);
}

template <class T>
T IAttributeDictionary::Get(const Stroka& name, const T& defaultValue) const
{
    return Find<T>(name).Get(defaultValue); // assuming that Find returns TNullable
}

template <class T>
typename TNullableTraits<
    typename TDeserializeTraits<T>::TReturnType
>::TNullableType IAttributeDictionary::Find(const Stroka& name) const
{
    const auto& yson = FindYson(name);
    if (!yson) {
        return
            typename TNullableTraits<
                typename TDeserializeTraits<T>::TReturnType
            >::TNullableType();
    }
    return DeserializeFromYson<T>(*yson);
}

template <class T>
void IAttributeDictionary::Set(const Stroka& name, const T& value)
{
    const auto& yson = SerializeToYson(value);
    SetYson(name, yson);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
