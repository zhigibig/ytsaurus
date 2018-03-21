#include "service_detail.h"
#include "bootstrap.h"
#include "yt_connector.h"

namespace NYP {
namespace NServer {
namespace NMaster {

////////////////////////////////////////////////////////////////////////////////

TServiceBase::TServiceBase(
    TBootstrap* bootstrap,
    const NYT::NRpc::TServiceDescriptor& descriptor,
    const NLogging::TLogger& logger)
    : NRpc::TServiceBase(
        bootstrap->GetWorkerPoolInvoker(),
        descriptor,
        logger)
    , Bootstrap_(bootstrap)
{ }

void TServiceBase::BeforeInvoke(NRpc::IServiceContext* /*context*/)
{
    const auto& ytConnector = Bootstrap_->GetYTConnector();
    if (!ytConnector->IsConnected()) {
        THROW_ERROR_EXCEPTION(
            NRpc::EErrorCode::Unavailable,
            "Master is not connected to YT");
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMaster
} // namespace NServer
} // namespace NYP
