#pragma once

#include "public.h"

#include <yt/core/yson/string.h>

namespace NYT::NYTree {

////////////////////////////////////////////////////////////////////////////////

struct IAttributeDictionary
{
    virtual ~IAttributeDictionary() = default;

    using TKey = TString;
    using TValue = NYson::TYsonString;
    using TKeyValuePair = std::pair<TKey, TValue>;

    //! Returns the list of all keys in the dictionary.
    virtual std::vector<TString> ListKeys() const = 0;

    //! Returns the list of all key-value pairs in the dictionary.
    virtual std::vector<TKeyValuePair> ListPairs() const = 0;

    //! Returns the value of the attribute (null indicates that the attribute is not found).
    virtual NYson::TYsonString FindYson(const TString& key) const = 0;

    //! Sets the value of the attribute.
    virtual void SetYson(const TString& key, const NYson::TYsonString& value) = 0;

    //! Removes the attribute.
    //! Returns |true| if the attribute was removed or |false| if there is no attribute with this key.
    virtual bool Remove(const TString& key) = 0;

    // Extension methods

    //! Removes all attributes.
    void Clear();

    //! Returns the value of the attribute (throws an exception if the attribute is not found).
    NYson::TYsonString GetYson(const TString& key) const;

    //! Finds the attribute and deserializes its value.
    //! Throws if no such value is found.
    template <class T>
    T Get(const TString& key) const;

    //! Same as #Get but removes the value.
    template <class T>
    T GetAndRemove(const TString& key);

    //! Finds the attribute and deserializes its value.
    //! Uses default value if no such attribute is found.
    template <class T>
    T Get(const TString& key, const T& defaultValue) const;

    //! Same as #Get but removes the value if it exists.
    template <class T>
    T GetAndRemove(const TString& key, const T& defaultValue);

    //! Finds the attribute and deserializes its value.
    //! Returns null if no such attribute is found.
    template <class T>
    typename TOptionalTraits<T>::TOptional Find(const TString& key) const;

    //! Same as #Find but removes the value if it exists.
    template <class T>
    typename TOptionalTraits<T>::TOptional FindAndRemove(const TString& key);

    //! Returns |true| iff the given key is present.
    bool Contains(const TString& key) const;

    //! Sets the attribute with a serialized value.
    template <class T>
    void Set(const TString& key, const T& value);

    //! Constructs an instance from a map node (by serializing the values).
    static std::unique_ptr<IAttributeDictionary> FromMap(const IMapNodePtr& node);

    //! Converts attributes to map node.
    IMapNodePtr ToMap() const;

    //! Adds more attributes from another map node.
    void MergeFrom(const IMapNodePtr other);

    //! Adds more attributes from another attribute dictionary.
    void MergeFrom(const IAttributeDictionary& other);

    //! Constructs an ephemeral copy.
    std::unique_ptr<IAttributeDictionary> Clone() const;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYTree

