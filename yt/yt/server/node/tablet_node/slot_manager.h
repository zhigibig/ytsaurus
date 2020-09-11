#pragma once

#include "public.h"

#include <yt/server/node/cluster_node/public.h>

#include <yt/server/lib/hydra/public.h>

#include <yt/ytlib/tablet_client/proto/heartbeat.pb.h>

#include <yt/client/table_client/unversioned_row.h>

#include <yt/core/actions/signal.h>

#include <yt/core/ytree/permission.h>

namespace NYT::NTabletNode {

////////////////////////////////////////////////////////////////////////////////

//! Controls all tablet slots running at this node.
class TSlotManager
    : public TRefCounted
{
public:
    TSlotManager(
        TTabletNodeConfigPtr config,
        NClusterNode::TBootstrap* bootstrap);
    ~TSlotManager();

    void Initialize();

    bool IsOutOfMemory() const;
    bool IsRotationForced(i64 passiveUsage) const;

    //! Sets the total number of tablet slots.
    void SetTabletSlotCount(int slotCount);

    //! Returns the total number of tablet slots.
    int GetTotalTabletSlotCount() const;

    //! Returns the number of available (not used) slots.
    int GetAvailableTabletSlotCount() const;

    //! Returns the number of currently used slots.
    int GetUsedTabletSlotCount() const;

    //! Returns |true| if there are free tablet slots and |false| otherwise.
    bool HasFreeTabletSlots() const;

    //! Returns fraction of CPU used by tablet slots (in terms of resource limits).
    double GetUsedCpu(double cpuPerTabletSlot) const;

    const std::vector<TTabletSlotPtr>& Slots() const;
    TTabletSlotPtr FindSlot(NHydra::TCellId id);
    void CreateSlot(const NTabletClient::NProto::TCreateTabletSlotInfo& createInfo);
    void ConfigureSlot(TTabletSlotPtr slot, const NTabletClient::NProto::TConfigureTabletSlotInfo& configureInfo);
    void RemoveSlot(TTabletSlotPtr slot);


    // The following methods are safe to call them from any thread.

    //! Returns the list of snapshots for all registered tablets.
    std::vector<TTabletSnapshotPtr> GetTabletSnapshots();

    //! Returns the snapshot for a given tablet or |nullptr| if none.
    TTabletSnapshotPtr FindTabletSnapshot(TTabletId tabletId);

    //! Returns the snapshot for a given tablet or throws if no such tablet is known.
    TTabletSnapshotPtr GetTabletSnapshotOrThrow(TTabletId tabletId);

    //! If #timestamp is other than #AsyncLastCommitted then checks
    //! that the Hydra instance has a valid leader lease.
    //! Throws on failure.
    void ValidateTabletAccess(
        const TTabletSnapshotPtr& tabletSnapshot,
        NTransactionClient::TTimestamp timestamp);

    //! Informs the manager that some slot now serves #tablet.
    //! It is fine to update an already registered snapshot.
    void RegisterTabletSnapshot(
        TTabletSlotPtr slot,
        TTablet* tablet,
        std::optional<TLockManagerEpoch> epoch = std::nullopt);

    //! Informs the manager that #tablet is no longer served.
    //! It is fine to attempt to unregister a snapshot that had never been registered.
    void UnregisterTabletSnapshot(TTabletSlotPtr slot, TTablet* tablet);

    //! Informs the manager that #slot no longer serves any tablet.
    void UnregisterTabletSnapshots(TTabletSlotPtr slot);

    //! Informs the manager that #tablet's snapshot must be updated.
    void UpdateTabletSnapshot(TTabletSlotPtr slot, TTablet* tablet);

    //! Returns a thread pool invoker used for building tablet snapshots.
    IInvokerPtr GetSnapshotPoolInvoker();

    void PopulateAlerts(std::vector<TError>* alerts);

    NYTree::IYPathServicePtr GetOrchidService();

    //! Creates and configures a fake tablet slot and validates the tablet cell snapshot.
    void ValidateCellSnapshot(NConcurrency::IAsyncZeroCopyInputStreamPtr reader);

    DECLARE_SIGNAL(void(), BeginSlotScan);
    DECLARE_SIGNAL(void(TTabletSlotPtr), ScanSlot);
    DECLARE_SIGNAL(void(), EndSlotScan);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;

};

DEFINE_REFCOUNTED_TYPE(TSlotManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
