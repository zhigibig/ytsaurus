#pragma once
#ifndef CHUNKED_MEMORY_POOL_INL_H_
#error "Direct inclusion of this file is not allowed, include chunked_memory_pool.h"
#endif

#include "serialize.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

inline char* TChunkedMemoryPool::AllocateUnaligned(size_t size)
{
    // Fast path.
    if (FreeZoneEnd_ >= FreeZoneBegin_ + size) {
        FreeZoneEnd_ -= size;
        Size_ += size;
        return FreeZoneEnd_;
    }

    // Slow path.
    return AllocateUnalignedSlow(size);
}

inline char* TChunkedMemoryPool::AllocateAligned(size_t size, int align)
{
    // NB: This can lead to FreeZoneBegin_ >= FreeZoneEnd_ in which case the chunk is full.
    FreeZoneBegin_ = AlignUp(FreeZoneBegin_, align);

    // Fast path.
    if (FreeZoneBegin_ + size <= FreeZoneEnd_) {
        char* result = FreeZoneBegin_;
        Size_ += size;
        FreeZoneBegin_ += size;
        return result;
    }

    // Slow path.
    return AllocateAlignedSlow(size, align);
}

template <class T>
inline T* TChunkedMemoryPool::AllocateUninitialized(int n, int align)
{
    return reinterpret_cast<T*>(AllocateAligned(sizeof(T) * n, align));
}

inline char* TChunkedMemoryPool::Capture(TRef src, int align)
{
    auto* dst = AllocateAligned(src.Size(), align);
    ::memcpy(dst, src.Begin(), src.Size());
    return dst;
}

inline void TChunkedMemoryPool::Free(char* from, char* to)
{
    if (FreeZoneBegin_ == to) {
        FreeZoneBegin_ = from;
    }
    if (FreeZoneEnd_ == from) {
        FreeZoneEnd_ = to;
    }
}

inline void TChunkedMemoryPool::Clear()
{
    Size_ = 0;

    if (Chunks_.empty()) {
        FreeZoneBegin_ = nullptr;
        FreeZoneEnd_ = nullptr;
        NextChunkIndex_ = 0;
    } else {
        FreeZoneBegin_ = Chunks_.front()->GetRef().Begin();
        FreeZoneEnd_ = Chunks_.front()->GetRef().End();
        NextChunkIndex_ = 1;
    }

    for (const auto& block : OtherBlocks_) {
        Capacity_ -= block->GetRef().Size();
    }
    OtherBlocks_.clear();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
