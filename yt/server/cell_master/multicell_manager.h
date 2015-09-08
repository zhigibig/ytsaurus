#pragma once

#include "public.h"

#include <core/misc/ref.h>

#include <core/actions/signal.h>

#include <core/rpc/public.h>

#include <ytlib/election/public.h>

#include <ytlib/object_client/public.h>

namespace NYT {
namespace NCellMaster {

////////////////////////////////////////////////////////////////////////////////

class TMulticellManager
    : public TRefCounted
{
public:
    TMulticellManager(
        TMulticellManagerConfigPtr config,
        TBootstrap* bootstrap);
    ~TMulticellManager();

    void PostToMaster(
        NRpc::IClientRequestPtr request,
        NObjectClient::TCellTag cellTag,
        bool reliable = true);
    void PostToMaster(
        const NObjectClient::TObjectId& objectId,
        NRpc::IServiceContextPtr context,
        NObjectClient::TCellTag cellTag,
        bool reliable = true);
    void PostToMaster(
        const ::google::protobuf::MessageLite& requestMessage,
        NObjectClient::TCellTag cellTag,
        bool reliable = true);
    void PostToMaster(
        TSharedRefArray requestMessage,
        NObjectClient::TCellTag cellTag,
        bool reliable = true);

    void PostToSecondaryMasters(
        NRpc::IClientRequestPtr request,
        bool reliable = true);
    void PostToSecondaryMasters(
        const NObjectClient::TObjectId& objectId,
        NRpc::IServiceContextPtr context,
        bool reliable = true);
    void PostToSecondaryMasters(
        const ::google::protobuf::MessageLite& requestMessage,
        bool reliable = true);
    void PostToSecondaryMasters(
        TSharedRefArray requestMessage,
        bool reliable = true);

    //! Returns |true| if there is a registered secondary master with a given cell tag.
    bool IsRegisteredSecondaryMaster(NObjectClient::TCellTag cellTag);

    //! Returns the list of cell tags for all registered secondary masters,
    //! in a stable order.
    std::vector<NObjectClient::TCellTag> GetRegisteredSecondaryMasterCellTags();

    //! Picks a random (but deterministically chosen) secondary cell for
    //! a new chunk owner. Cells with less-than-average number of chunks are preferred.
    //! If no secondary cells are registered then #InvalidCellTag is returned.
    NObjectClient::TCellTag PickCellForNode();

    DECLARE_SIGNAL(void(NObjectClient::TCellTag), SecondaryMasterRegistered);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TMulticellManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellMaster
} // namespace NYT
