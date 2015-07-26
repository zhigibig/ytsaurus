#pragma once

#include "public.h"
#include "permission.h"

#include <core/yson/consumer.h>

#include <core/misc/error.h>
#include <core/misc/nullable.h>

#include <core/actions/future.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

struct ISystemAttributeProvider
{
    virtual ~ISystemAttributeProvider()
    { }

    //! Describes a system attribute.
    struct TAttributeDescriptor
    {
        const char* Key = nullptr;
        bool Present = true;
        bool Opaque = false;
        bool Custom = false;
        bool Removable = false;
        bool Replicated = false;
        EPermissionSet WritePermission = EPermission::Write;

        TAttributeDescriptor& SetPresent(bool value)
        {
            Present = value;
            return *this;
        }

        TAttributeDescriptor& SetOpaque(bool value)
        {
            Opaque = value;
            return *this;
        }

        TAttributeDescriptor& SetCustom(bool value)
        {
            Custom = value;
            return *this;
        }

        TAttributeDescriptor& SetRemovable(bool value)
        {
            Removable = value;
            return *this;
        }

        TAttributeDescriptor& SetReplicated(bool value)
        {
            Replicated = value;
            return *this;
        }

        TAttributeDescriptor& SetWritePermission(EPermission value)
        {
            WritePermission = value;
            return *this;
        }

        TAttributeDescriptor(const char* key)
            : Key(key)
        { }
    };

    //! Populates the list of all system attributes supported by this object.
    /*!
     *  \note
     *  Must not clear #attributes since additional items may be added in inheritors.
     */
    virtual void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) = 0;

    //! Populates the list of all builtin attributes supported by this object.
    void ListBuiltinAttributes(std::vector<TAttributeDescriptor>* descriptors);

    //! Gets the value of a builtin attribute.
    /*!
     *  \returns |false| if there is no builtin attribute with the given key.
     */
    virtual bool GetBuiltinAttribute(const Stroka& key, NYson::IYsonConsumer* consumer) = 0;

    //! Asynchronously gets the value of a builtin attribute.
    /*!
     *  \returns Null if there is no such async builtin attribute with the given key.
     */
    virtual TFuture<void> GetBuiltinAttributeAsync(const Stroka& key, NYson::IYsonConsumer* consumer) = 0;

    //! Sets the value of a builtin attribute.
    /*!
     *  \returns |false| if there is no writable builtin attribute with the given key.
     */
    virtual bool SetBuiltinAttribute(const Stroka& key, const TYsonString& value) = 0;

    //! Removes value of a builtin attribute.
    /*!
     *  \returns |false| if there is no removalbe builtin attribute with the given key.
     */
    virtual bool RemoveBuiltinAttribute(const Stroka& key) = 0;


    // Extension methods.

    //! Returns an instance of TAttributeDescriptor matching a given #key or |Null| if no such
    //! builtin attribute is known.
    TNullable<TAttributeDescriptor> FindBuiltinAttributeDescriptor(const Stroka& key);

    //! A wrapper around interface method that returns the YSON string instead
    //! of writing it into a consumer.
    TNullable<NYTree::TYsonString> GetBuiltinAttribute(const Stroka& key);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
