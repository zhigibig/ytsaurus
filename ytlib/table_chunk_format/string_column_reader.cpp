#include "string_column_reader.h"

#include "column_reader_detail.h"
#include "private.h"
#include "helpers.h"
#include "compressed_integer_vector.h"

#include <yt/core/misc/bitmap.h>
#include <yt/core/misc/zigzag.h>

namespace NYT {
namespace NTableChunkFormat {

using namespace NTableClient;
using namespace NProto;

////////////////////////////////////////////////////////////////////////////////

template <EValueType ValueType, bool Scan>
class TStringValueExtractorBase
{
protected:
    TStringValueExtractorBase(const TSegmentMeta& segmentMeta)
        : StringMeta_(segmentMeta.GetExtension(TStringSegmentMeta::string_segment_meta))
    { }

    const NProto::TStringSegmentMeta& StringMeta_;

    using TOffsetsReader = TCompressedUnsignedVectorReader<ui32, Scan>;

    TOffsetsReader OffsetsReader_;
    const char* StringData_;

    ui32 GetOffset(i64 offsetIndex) const
    {
        return StringMeta_.expected_length() * (offsetIndex + 1) + 
            ZigZagDecode32(OffsetsReader_[offsetIndex]);
    }

    void SetStringValue(TUnversionedValue* value, i64 offsetIndex) const
    {
        ui32 padding = offsetIndex == 0 ? 0 : GetOffset(offsetIndex - 1);
        value->Data.String = StringData_ + padding;
        value->Length = GetOffset(offsetIndex) - padding;

        value->Type = ValueType;
    }
};

////////////////////////////////////////////////////////////////////////////////

template <EValueType ValueType, bool Scan>
class TDictionaryStringValueExtractorBase
    : public TStringValueExtractorBase<ValueType, Scan>
{
public:
    using TStringValueExtractorBase<ValueType, Scan>::TStringValueExtractorBase;

    void ExtractValue(TUnversionedValue* value, i64 valueIndex) const
    {
        if (IdsReader_[valueIndex] == 0) {
            value->Type = EValueType::Null;
        } else {
            SetStringValue(value, IdsReader_[valueIndex] - 1);
        }
    }

protected:
    using TIdsReader = TCompressedUnsignedVectorReader<ui32, Scan>;
    TIdsReader IdsReader_;

    using TStringValueExtractorBase<ValueType, Scan>::SetStringValue;
    using TStringValueExtractorBase<ValueType, Scan>::OffsetsReader_;
    using TStringValueExtractorBase<ValueType, Scan>::StringData_;
    using typename TStringValueExtractorBase<ValueType, Scan>::TOffsetsReader;

    void InitDictionaryReader(const char* ptr)
    {
        IdsReader_ = TIdsReader(reinterpret_cast<const ui64*>(ptr));
        ptr += IdsReader_.GetByteSize();

        OffsetsReader_ = TOffsetsReader(reinterpret_cast<const ui64*>(ptr));
        ptr += OffsetsReader_.GetByteSize();

        StringData_ = ptr;
    }
};

////////////////////////////////////////////////////////////////////////////////

template <EValueType ValueType, bool Scan>
class TDirectStringValueExtractorBase
    : public TStringValueExtractorBase<ValueType, Scan>
{
public:
    using TStringValueExtractorBase<ValueType, Scan>::TStringValueExtractorBase;

    void ExtractValue(TUnversionedValue* value, i64 valueIndex) const
    {
        if (NullBitmap_[valueIndex]) {
            value->Type = EValueType::Null;
        } else {
            SetStringValue(value, valueIndex);
        }
    }

protected:
    TReadOnlyBitmap<ui64> NullBitmap_;

    using TStringValueExtractorBase<ValueType, Scan>::SetStringValue;
    using TStringValueExtractorBase<ValueType, Scan>::OffsetsReader_;
    using TStringValueExtractorBase<ValueType, Scan>::StringData_;
    using typename TStringValueExtractorBase<ValueType, Scan>::TOffsetsReader;

    void InitDirectReader(const char* ptr)
    {
        OffsetsReader_ = TOffsetsReader(reinterpret_cast<const ui64*>(ptr));
        ptr += OffsetsReader_.GetByteSize();

        NullBitmap_ = TReadOnlyBitmap<ui64>(
            reinterpret_cast<const ui64*>(ptr),
            OffsetsReader_.GetSize());
        ptr += NullBitmap_.GetByteSize();

        StringData_ = ptr;
    }
};

////////////////////////////////////////////////////////////////////////////////

template <EValueType ValueType>
class TDirectDenseVersionedStringValueExtractor
    : public TDenseVersionedValueExtractorBase
    , public TDirectStringValueExtractorBase<ValueType, true>
{
public:
    TDirectDenseVersionedStringValueExtractor(TRef data, const TSegmentMeta& meta, bool aggregate)
        : TDenseVersionedValueExtractorBase(meta, aggregate)
        , TDirectStringValueExtractorBase<ValueType, true>(meta)
    {
        const char* ptr = data.Begin();
        ptr += InitDenseReader(ptr);
        TDirectStringValueExtractorBase<ValueType, true>::InitDirectReader(ptr);
    }
};

////////////////////////////////////////////////////////////////////////////////

template <EValueType ValueType>
class TDictionaryDenseVersionedStringValueExtractor
    : public TDenseVersionedValueExtractorBase
    , public TDictionaryStringValueExtractorBase<ValueType, true>
{
public:
    TDictionaryDenseVersionedStringValueExtractor(TRef data, const TSegmentMeta& meta, bool aggregate)
        : TDenseVersionedValueExtractorBase(meta, aggregate)
        , TDictionaryStringValueExtractorBase<ValueType, true>(meta)
    {
        const char* ptr = data.Begin();
        ptr += InitDenseReader(ptr);
        TDictionaryStringValueExtractorBase<ValueType, true>::InitDictionaryReader(ptr);
    }
};

////////////////////////////////////////////////////////////////////////////////

template <EValueType ValueType>
class TDirectSparseVersionedStringValueExtractor
    : public TSparseVersionedValueExtractorBase
    , public TDirectStringValueExtractorBase<ValueType, true>
{
public:
    TDirectSparseVersionedStringValueExtractor(TRef data, const TSegmentMeta& meta, bool aggregate)
        : TSparseVersionedValueExtractorBase(meta, aggregate)
        , TDirectStringValueExtractorBase<ValueType, true>(meta)
    {
        const char* ptr = data.Begin();
        ptr += InitSparseReader(ptr);
        TDirectStringValueExtractorBase<ValueType, true>::InitDirectReader(ptr);
    }
};

////////////////////////////////////////////////////////////////////////////////

template <EValueType ValueType>
class TDictionarySparseVersionedStringValueExtractor
    : public TSparseVersionedValueExtractorBase
    , public TDictionaryStringValueExtractorBase<ValueType, true>
{
public:
    TDictionarySparseVersionedStringValueExtractor(TRef data, const TSegmentMeta& meta, bool aggregate)
        : TSparseVersionedValueExtractorBase(meta, aggregate)
        , TDictionaryStringValueExtractorBase<ValueType, true>(meta)
    {
        const char* ptr = data.Begin();
        ptr += InitSparseReader(ptr);
        TDictionaryStringValueExtractorBase<ValueType, true>::InitDictionaryReader(ptr);
    }
};

////////////////////////////////////////////////////////////////////////////////

template <bool Scan = true>
class TDirectRleStringUnversionedValueExtractor
    : public TRleValueExtractorBase<Scan>
    , public TDirectStringValueExtractorBase<EValueType::String, Scan>
{
public:
    TDirectRleStringUnversionedValueExtractor(TRef data, const TSegmentMeta& meta)
        : TDirectStringValueExtractorBase<EValueType::String, Scan>(meta)
    {
        const char* ptr = data.Begin();
        RowIndexReader_ = TRowIndexReader(reinterpret_cast<const ui64*>(ptr));
        ptr += RowIndexReader_.GetByteSize();

        TDirectStringValueExtractorBase<EValueType::String, Scan>::InitDirectReader(ptr);
    }

private:
    using TRleValueExtractorBase<Scan>::RowIndexReader_;
    using typename TRleValueExtractorBase<Scan>::TRowIndexReader;
};

////////////////////////////////////////////////////////////////////////////////

template <bool Scan = true>
class TDictionaryRleStringUnversionedValueExtractor
    : public TRleValueExtractorBase<Scan>
    , public TDictionaryStringValueExtractorBase<EValueType::String, Scan>
{
public:
    TDictionaryRleStringUnversionedValueExtractor(TRef data, const TSegmentMeta& meta)
        : TDictionaryStringValueExtractorBase<EValueType::String, Scan>(meta)
    {
        const char* ptr = data.Begin();
        RowIndexReader_ = TRowIndexReader(reinterpret_cast<const ui64*>(ptr));
        ptr += RowIndexReader_.GetByteSize();

        TDictionaryStringValueExtractorBase<EValueType::String, Scan>::InitDictionaryReader(ptr);
    }

private:
    using TRleValueExtractorBase<Scan>::RowIndexReader_;
    using typename TRleValueExtractorBase<Scan>::TRowIndexReader;
};

////////////////////////////////////////////////////////////////////////////////

template <bool Scan = true>
class TDictionaryDenseStringUnversionedValueExtractor
    : public TDictionaryStringValueExtractorBase<EValueType::String, Scan>
{
public:
    TDictionaryDenseStringUnversionedValueExtractor(TRef data, const TSegmentMeta& meta)
        : TDictionaryStringValueExtractorBase<EValueType::String, Scan>(meta)
    {
        const char* ptr = data.Begin();
        TDictionaryStringValueExtractorBase<EValueType::String, Scan>::InitDictionaryReader(ptr);
    }
};

////////////////////////////////////////////////////////////////////////////////

template <bool Scan = true>
class TDirectDenseStringUnversionedValueExtractor
    : public TDirectStringValueExtractorBase<EValueType::String, Scan>
{
public:
    TDirectDenseStringUnversionedValueExtractor(TRef data, const TSegmentMeta& meta)
        : TDirectStringValueExtractorBase<EValueType::String, Scan>(meta)
    {
        TDirectStringValueExtractorBase<EValueType::String, Scan>::InitDirectReader(data.Begin());
        YCHECK(meta.row_count() == OffsetsReader_.GetSize());
    }

private:
    using TDirectStringValueExtractorBase<EValueType::String, Scan>::OffsetsReader_;
};

////////////////////////////////////////////////////////////////////////////////

template <EValueType ValueType>
class TVersionedStringColumnReader
    : public TVersionedColumnReaderBase
{
public:
    TVersionedStringColumnReader(const TColumnMeta& columnMeta, int columnId, bool aggregate)
        : TVersionedColumnReaderBase(columnMeta, columnId, aggregate)
    { }

private:
    virtual std::unique_ptr<IVersionedSegmentReader> CreateSegmentReader(int segmentIndex) override
    {
        using TDirectDenseReader = TDenseVersionedSegmentReader<
            TDirectDenseVersionedStringValueExtractor<ValueType>>;
        using TDictionaryDenseReader = TDenseVersionedSegmentReader<
            TDictionaryDenseVersionedStringValueExtractor<ValueType>>;
        using TDirectSparseReader = TSparseVersionedSegmentReader<
            TDirectSparseVersionedStringValueExtractor<ValueType>>;
        using TDictionarySparseReader = TSparseVersionedSegmentReader<
            TDictionarySparseVersionedStringValueExtractor<ValueType>>;

        const auto& meta = ColumnMeta_.segments(segmentIndex);
        auto segmentType = EVersionedStringSegmentType(meta.type());

        switch (segmentType) {
            case EVersionedStringSegmentType::DirectDense:
                return DoCreateSegmentReader<TDirectDenseReader>(meta);

            case EVersionedStringSegmentType::DictionaryDense:
                return DoCreateSegmentReader<TDictionaryDenseReader>(meta);

            case EVersionedStringSegmentType::DirectSparse:
                return DoCreateSegmentReader<TDirectSparseReader>(meta);

            case EVersionedStringSegmentType::DictionarySparse:
                return DoCreateSegmentReader<TDictionarySparseReader>(meta);

            default:
                YUNREACHABLE();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IVersionedColumnReader> CreateVersionedStringColumnReader(
    const TColumnMeta& columnMeta,
    int columnId,
    bool aggregate)
{
    return std::make_unique<TVersionedStringColumnReader<EValueType::String>>(
        columnMeta,
        columnId,
        aggregate);
}

std::unique_ptr<IVersionedColumnReader> CreateVersionedAnyColumnReader(
    const TColumnMeta& columnMeta,
    int columnId,
    bool aggregate)
{
    return std::make_unique<TVersionedStringColumnReader<EValueType::Any>>(
        columnMeta,
        columnId,
        aggregate);
}

////////////////////////////////////////////////////////////////////////////////

class TUnversionedStringColumnReader
    : public TUnversionedColumnReaderBase
{
public:
    TUnversionedStringColumnReader(const TColumnMeta& columnMeta, int columnIndex, int columnId)
        : TUnversionedColumnReaderBase(
            columnMeta,
            columnIndex,
            columnId)
    { }

    virtual std::pair<i64, i64> GetEqualRange(
        const TUnversionedValue& value,
        i64 lowerRowIndex,
        i64 upperRowIndex) override
    {
        return DoGetEqualRange<EValueType::String>(
            value, 
            lowerRowIndex, 
            upperRowIndex);
    }

private:
    virtual std::unique_ptr<IUnversionedSegmentReader> CreateSegmentReader(int segmentIndex, bool scan) override
    {
        typedef TDenseUnversionedSegmentReader<
            EValueType::String,
            TDirectDenseStringUnversionedValueExtractor<true>> TDirectDenseScanReader;

        typedef TDenseUnversionedSegmentReader<
            EValueType::String,
            TDirectDenseStringUnversionedValueExtractor<false>> TDirectDenseLookupReader;

        typedef TDenseUnversionedSegmentReader<
            EValueType::String,
            TDictionaryDenseStringUnversionedValueExtractor<true>> TDictionaryDenseScanReader;

        typedef TDenseUnversionedSegmentReader<
            EValueType::String,
            TDictionaryDenseStringUnversionedValueExtractor<false>> TDictionaryDenseLookupReader;

        typedef TRleUnversionedSegmentReader<
            EValueType::String,
            TDirectRleStringUnversionedValueExtractor<true>> TDirectRleScanReader;

        typedef TRleUnversionedSegmentReader<
            EValueType::String,
            TDirectRleStringUnversionedValueExtractor<false>> TDirectRleLookupReader;

        typedef TRleUnversionedSegmentReader<
            EValueType::String,
            TDictionaryRleStringUnversionedValueExtractor<true>> TDictionaryRleScanReader;

        typedef TRleUnversionedSegmentReader<
            EValueType::String,
            TDictionaryRleStringUnversionedValueExtractor<false>> TDictionaryRleLookupReader;

        const auto& meta = ColumnMeta_.segments(segmentIndex);
        auto segmentType = EUnversionedStringSegmentType(meta.type());

        switch (segmentType) {
            case EUnversionedStringSegmentType::DirectDense:
                if (scan) {
                    return DoCreateSegmentReader<TDirectDenseScanReader>(meta);
                } else {
                    return DoCreateSegmentReader<TDirectDenseLookupReader>(meta);
                }

            case EUnversionedStringSegmentType::DictionaryDense:
                if (scan) {
                    return DoCreateSegmentReader<TDictionaryDenseScanReader>(meta);
                } else {
                    return DoCreateSegmentReader<TDictionaryDenseLookupReader>(meta);
                }

            case EUnversionedStringSegmentType::DirectRle:
                if (scan) {
                    return DoCreateSegmentReader<TDirectRleScanReader>(meta);
                } else {
                    return DoCreateSegmentReader<TDirectRleLookupReader>(meta);
                }

            case EUnversionedStringSegmentType::DictionaryRle:
                if (scan) {
                    return DoCreateSegmentReader<TDictionaryRleScanReader>(meta);
                } else {
                    return DoCreateSegmentReader<TDictionaryRleLookupReader>(meta);
                }

            default:
                YUNREACHABLE();
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IUnversionedColumnReader> CreateUnversionedStringColumnReader(
    const TColumnMeta& columnMeta,
    int columnIndex,
    int columnId)
{
    return std::make_unique<TUnversionedStringColumnReader>(
        columnMeta,
        columnIndex,
        columnId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableChunkFormat
} // namespace NYT
