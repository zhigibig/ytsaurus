#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/core/actions/future.h>

#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/misc/error.h>

#include <yt/core/profiling/profiler.h>

#include <set>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

class TGarbageCollector
    : public TRefCounted
{
public:
    TGarbageCollector(
        TObjectManagerConfigPtr config,
        NCellMaster::TBootstrap* bootstrap);

    void Start();
    void Stop();

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);
    void Clear();

    TFuture<void> Collect();

    int EphemeralRefObject(TObjectBase* object, TEpoch epoch);
    int EphemeralUnrefObject(TObjectBase* object, TEpoch epoch);

    int WeakRefObject(TObjectBase* object, TEpoch epoch);
    int WeakUnrefObject(TObjectBase* object, TEpoch epoch);

    void RegisterZombie(TObjectBase* object);
    void UnregisterZombie(TObjectBase* object);
    void DestroyZombie(TObjectBase* object);

    void Reset();

    void CheckEmpty();

    int GetZombieCount() const;
    int GetEphemeralGhostCount() const;
    int GetWeakGhostCount() const;
    int GetLockedCount() const;

private:
    void ClearWeakGhosts();
    void ClearEphemeralGhosts();

    const TObjectManagerConfigPtr Config_;
    NCellMaster::TBootstrap* const Bootstrap_;

    NConcurrency::TPeriodicExecutorPtr SweepExecutor_;

    //! Contains objects with zero ref counter.
    //! These are ready for IObjectTypeHandler::Destroy call.
    THashSet<TObjectBase*> Zombies_;

    //! Contains objects with zero ref counter, zero weak ref counter, and positive ephemeral ref counter.
    //! These were already destroyed (via IObjectTypeHandler::Destroy) and await disposal (via |delete|).
    //! Not persisted.
    THashSet<TObjectBase*> EphemeralGhosts_;

    //! Contains objects with zero ref counter and positive weak ref counter
    //! (ephemeral ref counter may be zero or positive, it doesn't matter).
    //! These were already destroyed (via IObjectTypeHandler::Destroy) and await disposal (via |delete|).
    //! Persisted.
    THashSet<TObjectBase*> WeakGhosts_;

    //! This promise is set each time #GCQueue becomes empty.
    TPromise<void> CollectPromise_;

    //! The total number of locked objects, including ghosts.
    int LockedObjectCount_ = 0;

    DECLARE_THREAD_AFFINITY_SLOT(AutomatonThread);


    void OnSweep();
    bool IsRecovery();

};

DEFINE_REFCOUNTED_TYPE(TGarbageCollector)

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
