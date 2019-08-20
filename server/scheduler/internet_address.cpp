#include "internet_address.h"

namespace NYP::NServer::NScheduler {

////////////////////////////////////////////////////////////////////////////////

TInternetAddress::TInternetAddress(
    const TObjectId& id,
    const TObjectId& ip4AddressPoolId,
    NYT::NYson::TYsonString labels,
    NClient::NApi::NProto::TInternetAddressSpec spec,
    NClient::NApi::NProto::TInternetAddressStatus status)
    : TObject(id, std::move(labels))
    , ParentId_(ip4AddressPoolId)
    , Spec_(spec)
    , Status_(status)
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NScheduler
