#pragma once

#include "object.h"

#include <yp/server/objects/proto/autogen.pb.h>

#include <yt/core/misc/ref_tracked.h>
#include <yt/core/misc/property.h>

namespace NYP::NServer::NObjects {

////////////////////////////////////////////////////////////////////////////////

class TProject
    : public TObject
    , public NYT::TRefTracked<TProject>
{
public:
    static constexpr EObjectType Type = EObjectType::Project;

    TProject(
        const TObjectId& id,
        IObjectTypeHandler* typeHandler,
        ISession* session);

    virtual EObjectType GetType() const override;

    static const TScalarAttributeSchema<TProject, TString> OwnerIdSchema;
    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TString>, OwnerId);

    class TSpec
    {
    public:
        explicit TSpec(TProject* project);

        using TAccountAttribute = TManyToOneAttribute<TProject, TAccount>;
        static const TAccountAttribute::TSchema AccountSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TAccountAttribute, Account);

        using TEtc = NProto::TProjectSpecEtc;
        static const TScalarAttributeSchema<TProject, TEtc> EtcSchema;
        DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TEtc>, Etc);
    };

    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TSpec, Spec);

    using TStatus = NClient::NApi::NProto::TProjectStatus;
    static const TScalarAttributeSchema<TProject, TStatus> StatusSchema;
    DEFINE_BYREF_RW_PROPERTY_NO_INIT(TScalarAttribute<TStatus>, Status);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects
