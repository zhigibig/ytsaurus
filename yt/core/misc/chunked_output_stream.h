#pragma once

#include "common.h"
#include "ref.h"

#include <util/stream/output.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TChunkedOutputStreamTag { };

class TChunkedOutputStream
    : public TOutputStream
{
public:
    explicit TChunkedOutputStream(
        size_t initialReserveSize = 4 * 1024,
        size_t maxReserveSize = 64 * 1024,
        TRefCountedTypeCookie tagCookie = GetRefCountedTypeCookie<TChunkedOutputStreamTag>());

    template <class TTag>
    explicit TChunkedOutputStream(
        TTag tag = TTag(),
        size_t initialReserveSize = 4 * 1024,
        size_t maxReserveSize = 64 * 1024)
        : TChunkedOutputStream(
            initialReserveSize,
            maxReserveSize,
            GetRefCountedTypeCookie<TTag>())
    { }

    ~TChunkedOutputStream() throw();

    //! Returns a sequence of written chunks.
    //! The stream is no longer usable after this call.
    std::vector<TSharedRef> Flush();

    //! Returns the number of bytes actually written.
    size_t GetSize() const;
    
    //! Returns the number of bytes actually written plus unused capacity in the
    //! last chunk.
    size_t GetCapacity() const;

    //! Returns a pointer to a contiguous memory block of a given #size.
    //! Do not forget to call #Skip after use.
    char* Preallocate(size_t size);

    //! Marks #size bytes (which were previously preallocated) as used.
    void Advance(size_t size);

    //! TOutputStream override.
    virtual void DoWrite(const void* buf, size_t len) override;

private:
    size_t MaxReserveSize_;
    size_t CurrentReserveSize_;
    TRefCountedTypeCookie TagCookie_;

    size_t FinishedSize_;

    TBlob CurrentChunk_;
    std::vector<TSharedRef> FinishedChunks_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
