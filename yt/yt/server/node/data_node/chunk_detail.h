#pragma once

#include "public.h"
#include "chunk.h"

#include <yt/yt/server/node/cluster_node/bootstrap.h>

#include <yt/yt/core/profiling/timing.h>

#include <yt/yt/core/concurrency/spinlock.h>

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

//! Chunk properties that can be obtained during the filesystem scan.
struct TChunkDescriptor
{
    TChunkDescriptor() = default;

    explicit TChunkDescriptor(TChunkId id, i64 diskSpace = 0)
        : Id(id)
        , DiskSpace(diskSpace)
    { }

    TChunkId Id;
    i64 DiskSpace = 0;

    // For journal chunks only.
    i64 RowCount = 0;
    bool Sealed = false;
};

////////////////////////////////////////////////////////////////////////////////

//! A base for any IChunk implementation.
class TChunkBase
    : public IChunk
{
public:
    virtual TChunkId GetId() const override;
    virtual const TLocationPtr& GetLocation() const override;
    virtual TString GetFileName() const override;

    virtual int GetVersion() const override;
    virtual int IncrementVersion() override;

    virtual TFuture<void> PrepareToReadChunkFragments(
        const NChunkClient::TClientChunkReadOptions& options) override;
    virtual NIO::IIOEngine::TReadRequest MakeChunkFragmentReadRequest(
        const NIO::TChunkFragmentDescriptor& fragmentDescriptor) override;

    virtual void AcquireReadLock() override;
    virtual void ReleaseReadLock() override;

    virtual void AcquireUpdateLock() override;
    virtual void ReleaseUpdateLock() override;

    virtual TFuture<void> ScheduleRemove() override;
    virtual bool IsRemoveScheduled() const override;

    virtual void TrySweepReader() override;

protected:
    NClusterNode::IBootstrapBase* const Bootstrap_;
    const TLocationPtr Location_;
    const TChunkId Id_;

    std::atomic<int> Version_ = 0;

    YT_DECLARE_SPINLOCK(NConcurrency::TReaderWriterSpinLock, LifetimeLock_);
    std::atomic<int> ReadLockCounter_ = 0;
    int UpdateLockCounter_ = 0;
    TFuture<void> RemovedFuture_;
    TPromise<void> RemovedPromise_;
    std::atomic<bool> RemoveScheduled_ = false;
    std::atomic<bool> Removing_ = false;
    // Incremented by 2 on each read lock acquisition since last sweep scheduling.
    // The lowest bit indicates if sweep has been scheduled.
    std::atomic<ui64> ReaderSweepLatch_ = 0;


    struct TReadSessionBase
        : public TRefCounted
    {
        NProfiling::TWallTimer SessionTimer;
        std::optional<TChunkReadGuard> ChunkReadGuard;
        TChunkReadOptions Options;
    };

    using TReadSessionBasePtr = TIntrusivePtr<TReadSessionBase>;


    struct TReadMetaSession
        : public TReadSessionBase
    { };

    using TReadMetaSessionPtr = TIntrusivePtr<TReadMetaSession>;


    TChunkBase(
        NClusterNode::IBootstrapBase* bootstrap,
        TLocationPtr location,
        TChunkId id);
    ~TChunkBase();

    void StartAsyncRemove();
    virtual TFuture<void> AsyncRemove() = 0;

    virtual void ReleaseReader(NConcurrency::TSpinlockWriterGuard<NConcurrency::TReaderWriterSpinLock>& writerGuard);

    static NChunkClient::TRefCountedChunkMetaPtr FilterMeta(
        NChunkClient::TRefCountedChunkMetaPtr meta,
        const std::optional<std::vector<int>>& extensionTags);

    void StartReadSession(
        const TReadSessionBasePtr& session,
        const TChunkReadOptions& options);
    void ProfileReadBlockSetLatency(const TReadSessionBasePtr& session);
    void ProfileReadMetaLatency(const TReadSessionBasePtr& session);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode

