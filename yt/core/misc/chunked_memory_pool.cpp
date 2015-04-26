#include "stdafx.h"
#include "chunked_memory_pool.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

const size_t TChunkedMemoryPool::DefaultChunkSize = 4096;
const double TChunkedMemoryPool::DefaultMaxSmallBlockSizeRatio = 0.25;

////////////////////////////////////////////////////////////////////////////////

TChunkedMemoryPool::TChunkedMemoryPool(
    size_t chunkSize,
    double maxSmallBlockSizeRatio,
    TRefCountedTypeCookie tagCookie)
    : ChunkSize_(chunkSize)
    , MaxSmallBlockSize_(static_cast<size_t>(ChunkSize_ * maxSmallBlockSizeRatio))
    , TagCookie_(tagCookie)
{
    SetupFreeZone();
}

char* TChunkedMemoryPool::AllocateUnalignedSlow(size_t size)
{
    auto* large = AllocateSlowCore(size);
    if (large) {
        return large;
    }
    return AllocateUnaligned(size);
}

char* TChunkedMemoryPool::AllocateAlignedSlow(size_t size, size_t align)
{
    // NB: Do not rely on any particular alignment of chunks.
    auto* large = AllocateSlowCore(size + align);
    if (large) {
        return AlignPtr(large, size);
    }
    return AllocateAligned(size, align);
}

char* TChunkedMemoryPool::AllocateSlowCore(size_t size)
{
    if (size > MaxSmallBlockSize_) {
        return AllocateLargeBlock(size).Begin();
    }

    if (CurrentChunkIndex_ + 1 >= Chunks_.size()) {
        AllocateChunk();
    } else {
        SwitchChunk();
    }

    return nullptr;
}

void TChunkedMemoryPool::Clear()
{
    CurrentChunkIndex_ = 0;
    Size_ = 0;
    SetupFreeZone();

    for (const auto& block : LargeBlocks_) {
        Capacity_ -= block.Size();
    }
    LargeBlocks_.clear();
}

i64 TChunkedMemoryPool::GetSize() const
{
    return Size_;
}

i64 TChunkedMemoryPool::GetCapacity() const
{
    return Capacity_;
}

void TChunkedMemoryPool::AllocateChunk()
{
    auto chunk = TSharedRef::Allocate(ChunkSize_, false, TagCookie_);
    Chunks_.push_back(chunk);
    Capacity_ += ChunkSize_;
    CurrentChunkIndex_ = static_cast<int>(Chunks_.size()) - 1;
    SetupFreeZone();
}

void TChunkedMemoryPool::SwitchChunk()
{
    ++CurrentChunkIndex_;
    SetupFreeZone();
}

void TChunkedMemoryPool::SetupFreeZone()
{
    if (CurrentChunkIndex_ >= Chunks_.size()) {
        FreeZoneBegin_ = nullptr;
        FreeZoneEnd_ = nullptr;
    } else {
        auto& chunk = Chunks_[CurrentChunkIndex_];
        FreeZoneBegin_ = chunk.Begin();
        FreeZoneEnd_ = chunk.End();
    }
}

TSharedRef TChunkedMemoryPool::AllocateLargeBlock(size_t size)
{
    auto block = TSharedRef::Allocate(size, false, TagCookie_);
    LargeBlocks_.push_back(block);
    Capacity_ += size;
    return block;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
