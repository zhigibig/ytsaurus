#pragma once

#include "public.h"

#include "chunk_meta_extensions.h"

#include <ytlib/chunk_client/chunk_meta_extensions.h>
#include <ytlib/chunk_client/chunk_reader_base.h>
#include <ytlib/chunk_client/public.h>
#include <ytlib/chunk_client/read_limit.h>
#include <ytlib/chunk_client/sequential_reader.h>

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

class TChunkReaderBase
    : public virtual NChunkClient::IChunkReaderBase
{
public:
    TChunkReaderBase(
        TChunkReaderConfigPtr config,
        const NChunkClient::TReadLimit& lowerLimit,
        const NChunkClient::TReadLimit& upperLimit,
        NChunkClient::IChunkReaderPtr underlyingReader,
        const NChunkClient::NProto::TMiscExt& misc,
        NChunkClient::IBlockCachePtr blockCache);

    virtual TFuture<void> Open() override;

    virtual TFuture<void> GetReadyEvent() override;

    virtual NChunkClient::NProto::TDataStatistics GetDataStatistics() const;

    virtual TFuture<void> GetFetchingCompletedEvent();

protected:
    mutable NLogging::TLogger Logger;

    const TChunkReaderConfigPtr Config_;
    const NChunkClient::TReadLimit LowerLimit_;
    const NChunkClient::TReadLimit UpperLimit_;
    const NChunkClient::IBlockCachePtr BlockCache_;
    const NChunkClient::IChunkReaderPtr UnderlyingReader_;

    NChunkClient::TSequentialReaderPtr SequentialReader_;

    NChunkClient::NProto::TMiscExt Misc_;
    TFuture<void> ReadyEvent_;

    bool BlockEnded_ = false;

    TChunkedMemoryPool MemoryPool_;

    // A bunch of cached callbacks.
    TCallback<TFuture<void>()> SwitchBlockCallback_;
    TClosure InitFirstBlockCallback_;
    TClosure InitNextBlockCallback_;


    static int GetBlockIndexByKey(
        const TKey& key, 
        const std::vector<TOwningKey>& blockIndexKeys, 
        int beginBlockIndex = 0);

    // These methods return min block index, satisfying the lower limit.
    int ApplyLowerRowLimit(const NProto::TBlockMetaExt& blockMeta) const;
    int ApplyLowerKeyLimit(const NProto::TBlockMetaExt& blockMeta) const;
    int ApplyLowerKeyLimit(const std::vector<TOwningKey>& blockIndexKeys) const;

    // These methods return max block index, satisfying the upper limit.
    int ApplyUpperRowLimit(const NProto::TBlockMetaExt& blockMeta) const;
    int ApplyUpperKeyLimit(const NProto::TBlockMetaExt& blockMeta) const;
    int ApplyUpperKeyLimit(const std::vector<TOwningKey>& blockIndexKeys) const;

    void DoOpen();

    TFuture<void> FetchNextBlock();

    TFuture<void> SwitchBlock();
    static TFuture<void> SwitchBlockThunk(const TWeakPtr<TChunkReaderBase>& weakThis);

    bool OnBlockEnded();

    virtual std::vector<NChunkClient::TSequentialReader::TBlockInfo> GetBlockSequence() = 0;

    virtual void InitFirstBlock() = 0;
    virtual void InitNextBlock() = 0;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
