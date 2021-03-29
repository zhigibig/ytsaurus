#include "codec.h"

#include <yt/yt/core/misc/assert.h>
#include <yt/yt/core/misc/error.h>

namespace NYT::NErasure {

using namespace ::NErasure;

////////////////////////////////////////////////////////////////////////////////

int ICodec::GetTotalPartCount() const
{
    return GetDataPartCount() + GetParityPartCount();
}

////////////////////////////////////////////////////////////////////////////////

struct TJerasureBlobTag
{ };

struct TLrcBufferTag
{ };

////////////////////////////////////////////////////////////////////////////////

struct TCodecTraits
{
    using TBlobType = TSharedRef;
    using TMutableBlobType = TSharedMutableRef;
    using TBufferType = TBlob;
    using ECodecType = ECodec;

    static void Check(bool expr, const char* strExpr, const char* file, int line)
    {
        if (Y_UNLIKELY(!expr)) {
            ::NYT::NDetail::AssertTrapImpl("YT_VERIFY", strExpr, file, line);
            Y_UNREACHABLE();
        }
    }

    static TMutableBlobType AllocateBlob(size_t size)
    {
        return TMutableBlobType::Allocate<TJerasureBlobTag>(size, false);
    }

    static TBufferType AllocateBuffer(size_t size)
    {
        // Only LRC now uses buffer allocation.
        return TBufferType(TLrcBufferTag(), size);
    }

    static TBlobType FromBufferToBlob(TBufferType&& blob)
    {
        return TBlobType::FromBlob(std::move(blob));
    }
};

////////////////////////////////////////////////////////////////////////////////

template <ECodec CodecId, class TUnderlying>
class TCodec
    : public ICodec
{
public:
    virtual ECodec GetId() const override
    {
        return CodecId;
    }

    virtual std::vector<TSharedRef> Encode(const std::vector<TSharedRef>& blocks) const override
    {
        return Underlying_.Encode(blocks);
    }

    virtual std::vector<TSharedRef> Decode(
        const std::vector<TSharedRef>& blocks,
        const TPartIndexList& erasedIndices) const override
    {
        return Underlying_.Decode(blocks, erasedIndices);
    }

    virtual bool CanRepair(const TPartIndexList& erasedIndices) const override
    {
        return Underlying_.CanRepair(erasedIndices);
    }

    virtual bool CanRepair(const TPartIndexSet& erasedIndices) const override
    {
        return Underlying_.CanRepair(erasedIndices);
    }

    virtual std::optional<TPartIndexList> GetRepairIndices(const TPartIndexList& erasedIndices) const override
    {
        return Underlying_.GetRepairIndices(erasedIndices);
    }

    virtual int GetDataPartCount() const override
    {
        return Underlying_.GetDataPartCount();
    }

    virtual int GetParityPartCount() const override
    {
        return Underlying_.GetParityPartCount();
    }

    virtual int GetGuaranteedRepairablePartCount() const override
    {
        return Underlying_.GetGuaranteedRepairablePartCount();
    }

    virtual int GetWordSize() const override
    {
        return Underlying_.GetWordSize();
    }

private:
    TUnderlying Underlying_;
};

////////////////////////////////////////////////////////////////////////////////

ICodec* GetCodec(ECodec id)
{
    switch (id) {
        // NB: This codec uses Jerasure as a backend.
        case ECodec::ReedSolomon_6_3: {
            static TCodec<ECodec::ReedSolomon_6_3, TCauchyReedSolomonJerasure<6, 3, 8, TCodecTraits>> result;
            return &result;
        }
        // NB: This codec uses ISA-l as a backend.
        case ECodec::IsaReedSolomon_6_3: {
            static TCodec<ECodec::IsaReedSolomon_6_3, TReedSolomonIsa<6, 3, 8, TCodecTraits>> result;
            return &result;
        }
        // NB: This codec uses ISA-l as a backend.
        case ECodec::ReedSolomon_3_3: {
            static TCodec<ECodec::ReedSolomon_3_3, TReedSolomonIsa<3, 3, 8, TCodecTraits>> result;
            return &result;
        }
        // NB: This codec uses Jerasure as a backend.
        case ECodec::JerasureLrc_12_2_2: {
            static TCodec<ECodec::JerasureLrc_12_2_2, TLrcJerasure<12, 4, 8, TCodecTraits>> result;
            return &result;
        }
        // NB: This codec uses ISA-l as a backend.
        case ECodec::IsaLrc_12_2_2: {
            static TCodec<ECodec::IsaLrc_12_2_2, TLrcIsa<12, 4, 8, TCodecTraits>> result;
            return &result;
        }
        default:
            THROW_ERROR_EXCEPTION("Unknown erasure codec %Qlv", id);
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient

