#include "resource.h"

namespace NYP::NServer::NScheduler {

////////////////////////////////////////////////////////////////////////////////

TResource::TResource(
        const TObjectId& id,
        NYT::NYson::TYsonString labels,
        TNode* node,
        EResourceKind kind,
        NClient::NApi::NProto::TResourceSpec spec,
        std::vector<NClient::NApi::NProto::TResourceStatus_TAllocation> scheduledAllocations,
        std::vector<NClient::NApi::NProto::TResourceStatus_TAllocation> actualAllocations)
    : TObject(id, std::move(labels))
    , Node_(node)
    , Kind_(kind)
    , Spec_(std::move(spec))
    , ScheduledAllocations_(std::move(scheduledAllocations))
    , ActualAllocations_(std::move(actualAllocations))
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NScheduler
