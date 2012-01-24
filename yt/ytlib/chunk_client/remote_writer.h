#pragma once

#include "common.h"
#include "async_writer.h"
#include "chunk_service.pb.h"

#include <ytlib/misc/configurable.h>
#include <ytlib/misc/metric.h>
#include <ytlib/misc/semaphore.h>
#include <ytlib/misc/thread_affinity.h>
#include <ytlib/actions/action_queue.h>
#include <ytlib/logging/tagged_logger.h>
#include <ytlib/ytree/ypath_detail.h>
#include <ytlib/chunk_holder/chunk_holder_service_proxy.h>
#include <ytlib/chunk_server/chunk_ypath_proxy.h>

#include <util/generic/deque.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

class TRemoteWriter
    : public IAsyncWriter
{
public:
    typedef TIntrusivePtr<TRemoteWriter> TPtr;

    struct TConfig
        : public TConfigurable
    {
        typedef TIntrusivePtr<TConfig> TPtr;

        //! Maximum window size (in bytes).
        int WindowSize;
        
        //! Maximum group size (in bytes).
        int GroupSize;
        
        //! RPC requests timeout.
        /*!
         *  This timeout is especially useful for PutBlocks calls to ensure that
         *  uploading is not stalled.
         */
        TDuration HolderRpcTimeout;

        //! Maximum allowed period of time without RPC requests to holders.
        /*!
         *  If the writer remains inactive for the given period, it sends #TChunkHolderProxy::PingSession.
         */
        TDuration SessionPingInterval;

        TConfig()
        {
            Register("window_size", WindowSize)
                .Default(4 * 1024 * 1024)
                .GreaterThan(0);
            Register("group_size", GroupSize)
                .Default(1024 * 1024)
                .GreaterThan(0);
            Register("holder_rpc_timeout", HolderRpcTimeout)
                .Default(TDuration::Seconds(30));
            Register("session_ping_interval", SessionPingInterval)
                .Default(TDuration::Seconds(10));
        }

        virtual void DoValidate() const
        {
            if (WindowSize < GroupSize) {
                ythrow yexception() << "\"window_size\" cannot be less than \"group_size\"";
            }
        }
    };

    /*!
     * \note Thread affinity: ClientThread.
     */
    TRemoteWriter(
        TConfig* config, 
        const TChunkId& chunkId,
        const yvector<Stroka>& addresses);

    /*!
     * \note Thread affinity: ClientThread.
     */
    virtual TAsyncError::TPtr AsyncWriteBlock(const TSharedRef& data);

    /*!
     * \note Thread affinity: ClientThread.
     */
    virtual TAsyncError::TPtr AsyncClose(const NChunkHolder::NProto::TChunkAttributes& attributes);
    
    /*!
     * \note Thread affinity: any.
     */
    void Cancel(const TError& error);

    ~TRemoteWriter();

    /*!
     * \note Thread affinity: any.
     */
    Stroka GetDebugInfo();

    //! Returns the id of the chunk being uploaded.
    /*!
     * \note Thread affinity: any.
     */
    TChunkId GetChunkId() const;

    //! Returns the confirmation request for the uploaded chunk.
    /*!
     *  This method call only be called when the writer is successfully closed.
     *  
     * \note Thread affinity: ClientThread.
     */
    NChunkServer::TChunkYPathProxy::TReqConfirm::TPtr GetConfirmRequest();

private:
    //! A group is a bunch of blocks that is sent in a single RPC request.
    class TGroup;
    typedef TIntrusivePtr<TGroup> TGroupPtr;

    struct TNode;
    typedef TIntrusivePtr<TNode> TNodePtr;
    
    typedef ydeque<TGroupPtr> TWindow;

    typedef NChunkHolder::TChunkHolderServiceProxy TProxy;
    typedef TProxy::EErrorCode EErrorCode;

    TChunkId ChunkId;
    TConfig::TPtr Config;

    TAsyncStreamState State;

    bool InitComplete;

    //! This flag is raised whenever #Close is invoked.
    //! All access to this flag happens from #WriterThread.
    bool IsCloseRequested;
    NChunkHolder::NProto::TChunkAttributes Attributes;

    TWindow Window;
    TAsyncSemaphore WindowSlots;

    yvector<TNodePtr> Nodes;

    //! Number of nodes that are still alive.
    int AliveNodeCount;

    //! A new group of blocks that is currently being filled in by the client.
    //! All access to this field happens from client thread.
    TGroupPtr CurrentGroup;

    //! Number of blocks that are already added via #AddBlock. 
    int BlockCount;

    TMetric StartChunkTiming;
    TMetric PutBlocksTiming;
    TMetric SendBlocksTiming;
    TMetric FlushBlockTiming;
    TMetric FinishChunkTiming;

    NLog::TTaggedLogger Logger;

    /*!
     * Invoked from #Close.
     * Sets #IsCloseRequested.
     */
    void DoClose(const NChunkHolder::NProto::TChunkAttributes& attributes);
    
    /*!
     * Invoked from #Cancel
     */
    void DoCancel();

    void AddGroup(TGroupPtr group);

    void RegisterReadyEvent(TFuture<TVoid>::TPtr windowReady);

    void OnNodeDied(int node);

    void ShiftWindow();

    TProxy::TInvFlushBlock::TPtr FlushBlock(int node, int blockIndex);

    void OnBlockFlushed(int node, int blockIndex);

    void OnWindowShifted(int blockIndex);

    void InitializeNodes(const yvector<Stroka>& addresses);

    void StartSession();

    TProxy::TInvStartChunk::TPtr StartChunk(int node);

    void OnChunkStarted(int node);

    void OnSessionStarted();

    void CloseSession();

    TProxy::TInvFinishChunk::TPtr FinishChunk(int node);

    void OnChunkFinished(int node);

    void OnSessionFinished();

    void PingSession(int node);
    void SchedulePing(int node);
    void CancelPing(int node);
    void CancelAllPings();

    template<class TResponse>
    void CheckResponse(
        typename TResponse::TPtr rsp, 
        int node, 
        IAction::TPtr onSuccess,
        TMetric* metric);

    void AddBlock(TVoid, const TSharedRef& data);

    DECLARE_THREAD_AFFINITY_SLOT(ClientThread);
    DECLARE_THREAD_AFFINITY_SLOT(WriterThread);

};

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT

