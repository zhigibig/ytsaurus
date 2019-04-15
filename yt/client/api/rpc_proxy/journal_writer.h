#pragma once

#include "api_service_proxy.h"

namespace NYT::NApi::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

IJournalWriterPtr CreateRpcProxyJournalWriter(
    TApiServiceProxy::TReqWriteJournalPtr request);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy

