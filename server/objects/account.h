#pragma once

#include "object.h"

#include <yp/server/objects/proto/objects.pb.h>

#include <yp/client/api/proto/data_model.pb.h>

#include <yt/core/misc/ref_tracked.h>

namespace NYP {
namespace NServer {
namespace NObjects {

////////////////////////////////////////////////////////////////////////////////

class TAccount
    : public TObject
    , public NYT::TRefTracked<TAccount>
{
public:
    static constexpr EObjectType Type = EObjectType::Account;

    TAccount(
        const TObjectId& id,
        IObjectTypeHandler* typeHandler,
        ISession* session);

    virtual EObjectType GetType() const override;

    using TStatus = NClient::NApi::NProto::TAccountStatus;
    static const TScalarAttributeSchema<TAccount, TStatus> StatusSchema;
    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TStatus>, Status);

    class TSpec
    {
    public:
        explicit TSpec(TAccount* account);

        static const TManyToOneAttributeSchema<TAccount, TAccount> ParentSchema;
        using TParentAttribute = TManyToOneAttribute<TAccount, TAccount>;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TParentAttribute, Parent);

        static const TOneToManyAttributeSchema<TAccount, TAccount> ChildrenSchema;
        using TChildrenAttribute = TOneToManyAttribute<TAccount, TAccount>;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TChildrenAttribute, Children);

        using TOther = NProto::TAccountSpecOther;
        static const TScalarAttributeSchema<TAccount, TOther> OtherSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TOther>, Other);
    };

    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TSpec, Spec);

    static const TOneToManyAttributeSchema<TAccount, TPodSet> PodSetsSchema;
    using TPodSets = TOneToManyAttribute<TAccount, TPodSet>;
    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TPodSets, PodSets);

    virtual bool IsBuiltin() const override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjects
} // namespace NServer
} // namespace NYP
