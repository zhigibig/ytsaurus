#pragma once

#include "public.h"

#include <yp/client/api/proto/data_model.pb.h>

#include <yp/server/objects/public.h>

#include <yp/server/objects/proto/objects.pb.h>

namespace NYP {
namespace NServer {
namespace NAccounting {

////////////////////////////////////////////////////////////////////////////////

using TPerSegmentResourceTotals = NClient::NApi::NProto::TPerSegmentResourceTotals;
using TResourceTotals = NClient::NApi::NProto::TResourceTotals;

TResourceTotals ResourceUsageFromPodSpec(
    const NObjects::NProto::TPodSpecOther& spec,
    const NObjects::TObjectId& segmentId);

////////////////////////////////////////////////////////////////////////////////

} // namespace NAccounting
} // namespace NServer
} // namespace NYP

namespace NYP {
namespace NClient {
namespace NApi {
namespace NProto {

////////////////////////////////////////////////////////////////////////////////

TPerSegmentResourceTotals& operator +=(TPerSegmentResourceTotals& lhs, const TPerSegmentResourceTotals& rhs);
TPerSegmentResourceTotals operator +(const TPerSegmentResourceTotals& lhs, const TPerSegmentResourceTotals& rhs);
TPerSegmentResourceTotals& operator -=(TPerSegmentResourceTotals& lhs, const TPerSegmentResourceTotals& rhs);
TPerSegmentResourceTotals operator -(const TPerSegmentResourceTotals& lhs, const TPerSegmentResourceTotals& rhs);

TResourceTotals& operator +=(TResourceTotals& lhs, const TResourceTotals& rhs);
TResourceTotals operator +(const TResourceTotals& lhs, const TResourceTotals& rhs);
TResourceTotals& operator -=(TResourceTotals& lhs, const TResourceTotals& rhs);
TResourceTotals operator -(const TResourceTotals& lhs, const TResourceTotals& rhs);
TResourceTotals operator -(const TResourceTotals& arg);

void FormatValue(NYT::TStringBuilder* builder, const TPerSegmentResourceTotals& totals, TStringBuf format);
void FormatValue(NYT::TStringBuilder* builder, const TResourceTotals& totals, TStringBuf format);
TString ToString(const TPerSegmentResourceTotals& totals);
TString ToString(const TResourceTotals& totals);

////////////////////////////////////////////////////////////////////////////////

} // namespace NProto
} // namespace NApi
} // namespace NClient
} // namespace NYP

