#pragma once

#include "public.h"

#include <yt/client/chunk_client/chunk_replica.h>
#include <yt/ytlib/chunk_client/session_id.h>

#include <yt/server/node/cell_node/public.h>

#include <yt/core/concurrency/public.h>
#include <yt/core/concurrency/thread_affinity.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

//! Manages chunk uploads.
/*!
 *  Thread affinity: ControlThread
 */
class TSessionManager
    : public TRefCounted
{
    DEFINE_BYVAL_RW_PROPERTY(bool, DisableWriteSessions, false);

public:
    using TSessionPtrList = SmallVector<ISessionPtr, 1>;

    TSessionManager(
        TDataNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap);

    //! Starts a new chunk upload session.
    /*!
     *  Chunk files are opened asynchronously, however the call returns immediately.
     */
    ISessionPtr StartSession(TSessionId sessionId, const TSessionOptions& options);

    //! Finds session by session ID. Returns |nullptr| if no session is found.
    //! Session ID must not specify AllMediaIndex as medium index.
    ISessionPtr FindSession(TSessionId sessionId);

    //! Finds session by session ID. Throws if no session is found.
    //! Session ID must not specify AllMediaIndex as medium index.
    ISessionPtr GetSessionOrThrow(TSessionId sessionId);

    //! Returns the number of currently active sessions of a given type.
    int GetSessionCount(ESessionType type);

private:
    const TDataNodeConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;

    THashMap<TSessionId, ISessionPtr> SessionMap_;

    ISessionPtr CreateSession(TSessionId sessionId, const TSessionOptions& options);

    void OnSessionLeaseExpired(TSessionId sessionId);
    void OnSessionFinished(const TWeakPtr<ISession>& session, const TError& error);

    void RegisterSession(const ISessionPtr& session);
    void UnregisterSession(const ISessionPtr& session);

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

};

DEFINE_REFCOUNTED_TYPE(TSessionManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode

