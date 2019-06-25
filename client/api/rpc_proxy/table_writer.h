#pragma once

#include "api_service_proxy.h"

namespace NYT::NApi::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

TFuture<ITableWriterPtr> CreateTableWriter(
    TApiServiceProxy::TReqWriteTablePtr request);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy

