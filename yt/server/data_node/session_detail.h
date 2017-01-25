#pragma once

#include "public.h"
#include "session.h"

#include <yt/server/cell_node/public.h>

#include <yt/core/concurrency/thread_affinity.h>

#include <yt/core/logging/log.h>

#include <yt/core/profiling/profiler.h>

namespace NYT {
namespace NDataNode {

////////////////////////////////////////////////////////////////////////////////

class TSessionBase
    : public ISession
{
public:
    TSessionBase(
        TDataNodeConfigPtr config,
        NCellNode::TBootstrap* bootstrap,
        const TSessionId& sessionId,
        const TSessionOptions& options,
        TStoreLocationPtr location,
        NConcurrency::TLease lease);

    ~TSessionBase();

    virtual const TChunkId& GetChunkId() const& override;
    virtual const TSessionId& GetId() const& override;
    virtual ESessionType GetType() const override;
    virtual const TWorkloadDescriptor& GetWorkloadDescriptor() const override;
    TStoreLocationPtr GetStoreLocation() const override;

    virtual TFuture<void> Start() override;

    virtual void Ping() override;

    virtual void Cancel(const TError& error) override;

    virtual TFuture<IChunkPtr> Finish(
        const NChunkClient::NProto::TChunkMeta* chunkMeta,
        const TNullable<int>& blockCount) override;

    virtual TFuture<void> PutBlocks(
        int startBlockIndex,
        const std::vector<TSharedRef>& blocks,
        bool enableCaching) override;

    virtual TFuture<void> SendBlocks(
        int startBlockIndex,
        int blockCount,
        const NNodeTrackerClient::TNodeDescriptor& targetDescriptor) override;

    virtual TFuture<void> FlushBlocks(int blockIndex) override;

    DEFINE_SIGNAL(void(const TError& error), Finished);

protected:
    const TDataNodeConfigPtr Config_;
    NCellNode::TBootstrap* const Bootstrap_;
    const TSessionId SessionId_;
    const TSessionOptions Options_;
    const TStoreLocationPtr Location_;
    const NConcurrency::TLease Lease_;

    IInvokerPtr WriteInvoker_;

    bool Active_ = false;

    NLogging::TLogger Logger;
    NProfiling::TProfiler Profiler;


    virtual TFuture<void> DoStart() = 0;
    virtual void DoCancel() = 0;
    virtual TFuture<IChunkPtr> DoFinish(
        const NChunkClient::NProto::TChunkMeta* chunkMeta,
        const TNullable<int>& blockCount) = 0;
    virtual TFuture<void> DoPutBlocks(
        int startBlockIndex,
        const std::vector<TSharedRef>& blocks,
        bool enableCaching) = 0;
    virtual TFuture<void> DoSendBlocks(
        int startBlockIndex,
        int blockCount,
        const NNodeTrackerClient::TNodeDescriptor& target) = 0;
    virtual TFuture<void> DoFlushBlocks(int blockIndex) = 0;

    void ValidateActive();

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDataNode
} // namespace NYT

