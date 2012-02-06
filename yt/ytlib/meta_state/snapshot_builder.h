#pragma once

#include "common.h"
#include "meta_state_manager_proxy.h"
#include "snapshot_store.h"
#include "meta_state.h"
#include "decorated_meta_state.h"
#include "cell_manager.h"
#include "change_log_cache.h"

#include <ytlib/election/election_manager.h>
#include <ytlib/rpc/client.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

class TSnapshotBuilder
    : public TRefCounted
{
public:
    typedef TIntrusivePtr<TSnapshotBuilder> TPtr;

    struct TConfig
        : public TConfigurable
    {
        typedef TIntrusivePtr<TConfig> TPtr;

        TDuration Timeout;

        TConfig()
        {
            Register("timeout", Timeout)
                .GreaterThan(TDuration())
                .Default(TDuration::Minutes(1));
        }
    };

    DECLARE_ENUM(EResultCode,
        (OK)
        (InvalidVersion)
        (AlreadyInProgress)
    );

    struct TLocalResult
    {
        EResultCode ResultCode;
        TChecksum Checksum;

        explicit TLocalResult(
            EResultCode resultCode = EResultCode::OK,
            TChecksum checksum = 0)
            : ResultCode(resultCode)
            , Checksum(checksum)
        { }
    };

    TSnapshotBuilder(
        TConfig* config,
        TCellManager::TPtr cellManager,
        TDecoratedMetaState::TPtr metaState,
        TChangeLogCache::TPtr changeLogCache,
        TSnapshotStore::TPtr snapshotStore,
        TEpoch epoch,
        IInvoker::TPtr serviceInvoker);

    /*!
     * \returns OK if distributed session is started,
     *          AlreadyInProgress if the previous session is not completed yet.
     *
     * \note Thread affinity: StateThread
     */
    EResultCode CreateDistributed();

    /*!
     * \note Thread affinity: StateThread
     */
    TLocalResult CreateLocal(TMetaVersion version);

private:
    DECLARE_THREAD_AFFINITY_SLOT(StateThread);

    class TSession;

    typedef TMetaStateManagerProxy TProxy;

    TConfig::TPtr Config;
    TCellManager::TPtr CellManager;
    TDecoratedMetaState::TPtr MetaState;
    TSnapshotStore::TPtr SnapshotStore;
    TChangeLogCache::TPtr ChangeLogCache;
    TEpoch Epoch;
    IInvoker::TPtr ServiceInvoker;
    IInvoker::TPtr StateInvoker;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
