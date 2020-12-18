#include "input_chunk_slice.h"

#include "private.h"
#include "chunk_meta_extensions.h"
#include "input_chunk.h"

#include <yt/client/table_client/row_buffer.h>
#include <yt/client/table_client/serialize.h>

#include <yt/library/erasure/codec.h>

#include <yt/core/misc/numeric_helpers.h>
#include <yt/core/ytree/fluent.h>

#include <yt/core/misc/numeric_helpers.h>

#include <cmath>

namespace NYT::NChunkClient {

using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TLegacyInputSliceLimit::TLegacyInputSliceLimit(const TLegacyReadLimit& other)
{
    YT_VERIFY(!other.HasChunkIndex());
    YT_VERIFY(!other.HasOffset());
    if (other.HasRowIndex()) {
        RowIndex = other.GetRowIndex();
    }
    if (other.HasLegacyKey()) {
        Key = other.GetLegacyKey();
    }
}

TLegacyInputSliceLimit::TLegacyInputSliceLimit(
    const NProto::TReadLimit& other,
    const TRowBufferPtr& rowBuffer,
    TRange<TLegacyKey> keySet)
{
    YT_VERIFY(!other.has_chunk_index());
    YT_VERIFY(!other.has_offset());
    if (other.has_row_index()) {
        RowIndex = other.row_index();
    }
    if (other.has_legacy_key()) {
        NTableClient::FromProto(&Key, other.legacy_key(), rowBuffer);
    }
    if (other.has_key_index()) {
        Key = rowBuffer->Capture(keySet[other.key_index()]);
    }
}

void TLegacyInputSliceLimit::MergeLowerRowIndex(i64 rowIndex)
{
    if (!RowIndex || *RowIndex < rowIndex) {
        RowIndex = rowIndex;
    }
}

void TLegacyInputSliceLimit::MergeUpperRowIndex(i64 rowIndex)
{
    if (!RowIndex || *RowIndex > rowIndex) {
        RowIndex = rowIndex;
    }
}

void TLegacyInputSliceLimit::MergeLowerKey(NTableClient::TLegacyKey key)
{
    if (!Key || Key < key) {
        Key = key;
    }
}

void TLegacyInputSliceLimit::MergeUpperKey(NTableClient::TLegacyKey key)
{
    if (!Key || Key > key) {
        Key = key;
    }
}

void TLegacyInputSliceLimit::MergeLowerLimit(const TLegacyInputSliceLimit& limit)
{
    if (limit.RowIndex) {
        MergeLowerRowIndex(*limit.RowIndex);
    }
    if (limit.Key) {
        MergeLowerKey(limit.Key);
    }
}

void TLegacyInputSliceLimit::MergeUpperLimit(const TLegacyInputSliceLimit& limit)
{
    if (limit.RowIndex) {
        MergeUpperRowIndex(*limit.RowIndex);
    }
    if (limit.Key) {
        MergeUpperKey(limit.Key);
    }
}

void TLegacyInputSliceLimit::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, RowIndex);
    Persist(context, Key);
}

TString ToString(const TLegacyInputSliceLimit& limit)
{
    return Format("RowIndex: %v, Key: %v", limit.RowIndex, limit.Key);
}

void FormatValue(TStringBuilderBase* builder, const TLegacyInputSliceLimit& limit, TStringBuf /*format*/)
{
    builder->AppendFormat("{RowIndex: %v, Key: %v}",
        limit.RowIndex,
        limit.Key);
}

bool IsTrivial(const TLegacyInputSliceLimit& limit)
{
    return !limit.RowIndex && !limit.Key;
}

void ToProto(NProto::TReadLimit* protoLimit, const TLegacyInputSliceLimit& limit)
{
    if (limit.RowIndex) {
        protoLimit->set_row_index(*limit.RowIndex);
    } else {
        protoLimit->clear_row_index();
    }

    if (limit.Key) {
        ToProto(protoLimit->mutable_legacy_key(), limit.Key);
    } else {
        protoLimit->clear_legacy_key();
    }
}

////////////////////////////////////////////////////////////////////////////////

TInputSliceLimit::TInputSliceLimit(
    const NProto::TReadLimit& other,
    const TRowBufferPtr& rowBuffer,
    TRange<TLegacyKey> keySet,
    int keyLength,
    bool isUpper)
{
    YT_VERIFY(!other.has_chunk_index());
    YT_VERIFY(!other.has_offset());
    if (other.has_row_index()) {
        RowIndex = other.row_index();
    }
    TUnversionedRow row;

    if (other.has_key_bound_prefix()) {
        NTableClient::FromProto(&KeyBound.Prefix, other.key_bound_prefix(), rowBuffer);
        KeyBound.IsInclusive = other.key_bound_is_inclusive();
        KeyBound.IsUpper = isUpper;
    } else {
        // Build from legacy-serialized read limit.
        if (other.has_legacy_key()) {
            NTableClient::FromProto(&row, other.legacy_key(), rowBuffer);
        }
        if (other.has_key_index()) {
            row = rowBuffer->Capture(keySet[other.key_index()]);
        }
        if (row) {
            KeyBound = KeyBoundFromLegacyRow(row, isUpper, keyLength, rowBuffer);
        } else {
            KeyBound = TKeyBound::MakeUniversal(isUpper);
        }
    }
}

TInputSliceLimit::TInputSliceLimit(bool isUpper)
    : KeyBound(TKeyBound::MakeUniversal(isUpper))
{ }

void TInputSliceLimit::MergeLower(const TInputSliceLimit& other, const std::optional<TComparator>& comparator)
{
    if (!RowIndex || (other.RowIndex && *other.RowIndex > *RowIndex)) {
        RowIndex = other.RowIndex;
    }
    if (comparator) {
        comparator->ReplaceIfStrongerKeyBound(KeyBound, other.KeyBound);
    } else {
        YT_VERIFY(!other.KeyBound);
    }
    YT_VERIFY(!KeyBound || !KeyBound.IsUpper);
}

void TInputSliceLimit::MergeUpper(const TInputSliceLimit& other, const std::optional<TComparator>& comparator)
{
    if (!RowIndex || (other.RowIndex && *other.RowIndex < *RowIndex)) {
        RowIndex = other.RowIndex;
    }
    if (comparator) {
        comparator->ReplaceIfStrongerKeyBound(KeyBound, other.KeyBound);
    } else {
        YT_VERIFY(!other.KeyBound);
    }
    YT_VERIFY(!KeyBound || KeyBound.IsUpper);
}

bool TInputSliceLimit::IsTrivial() const
{
    return (!KeyBound || KeyBound.IsUniversal()) && !RowIndex;
}

void TInputSliceLimit::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, RowIndex);
    Persist(context, KeyBound);
}

TString ToString(const TInputSliceLimit& limit)
{
    return Format("RowIndex: %v, KeyBound: %v", limit.RowIndex, limit.KeyBound);
}

void FormatValue(TStringBuilderBase* builder, const TInputSliceLimit& limit, TStringBuf /*format*/)
{
    builder->AppendFormat("{RowIndex: %v, KeyBound: %v}",
        limit.RowIndex,
        limit.KeyBound);
}

bool IsTrivial(const TInputSliceLimit& limit)
{
    return !limit.RowIndex && (!limit.KeyBound || limit.KeyBound.IsUniversal());
}

void ToProto(NProto::TReadLimit* protoLimit, const TInputSliceLimit& limit)
{
    if (limit.RowIndex) {
        protoLimit->set_row_index(*limit.RowIndex);
    } else {
        protoLimit->clear_row_index();
    }

    if (!limit.KeyBound || limit.KeyBound.IsUniversal()) {
        protoLimit->clear_legacy_key();
        protoLimit->clear_key_bound_prefix();
        protoLimit->clear_key_bound_is_inclusive();
    } else {
        protoLimit->set_key_bound_is_inclusive(limit.KeyBound.IsInclusive);
        auto legacyRow = KeyBoundToLegacyRow(limit.KeyBound);
        ToProto(protoLimit->mutable_legacy_key(), legacyRow);
        ToProto(protoLimit->mutable_key_bound_prefix(), limit.KeyBound.Prefix);
    }
}

////////////////////////////////////////////////////////////////////////////////

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    TLegacyKey lowerKey,
    TLegacyKey upperKey)
    : InputChunk_(inputChunk)
    , DataWeight_(inputChunk->GetDataWeight())
    , RowCount_(inputChunk->GetRowCount())
{
    if (inputChunk->LowerLimit()) {
        LegacyLowerLimit_ = TLegacyInputSliceLimit(*inputChunk->LowerLimit());
    }
    if (lowerKey) {
        LegacyLowerLimit_.MergeLowerKey(lowerKey);
    }

    if (inputChunk->UpperLimit()) {
        LegacyUpperLimit_ = TLegacyInputSliceLimit(*inputChunk->UpperLimit());
    }
    if (upperKey) {
        LegacyUpperLimit_.MergeUpperKey(upperKey);
    }
}

TInputChunkSlice::TInputChunkSlice(const TInputChunkSlice& inputSlice)
    : InputChunk_(inputSlice.GetInputChunk())
    , IsLegacy(inputSlice.IsLegacy)
    , PartIndex_(inputSlice.GetPartIndex())
    , SizeOverridden_(inputSlice.GetSizeOverridden())
    , DataWeight_(inputSlice.GetDataWeight())
    , RowCount_(inputSlice.GetRowCount())
{
    if (inputSlice.IsLegacy) {
        LegacyLowerLimit_ = inputSlice.LegacyLowerLimit_;
        LegacyUpperLimit_ = inputSlice.LegacyUpperLimit_;
    } else {
        LowerLimit_ = inputSlice.LowerLimit_;
        UpperLimit_ = inputSlice.UpperLimit_;
    }
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkSlice& inputSlice,
    TLegacyKey lowerKey,
    TLegacyKey upperKey)
    : InputChunk_(inputSlice.GetInputChunk())
    , LegacyLowerLimit_(inputSlice.LegacyLowerLimit())
    , LegacyUpperLimit_(inputSlice.LegacyUpperLimit())
    , IsLegacy(inputSlice.IsLegacy)
    , PartIndex_(inputSlice.GetPartIndex())
    , SizeOverridden_(inputSlice.GetSizeOverridden())
    , DataWeight_(inputSlice.GetDataWeight())
    , RowCount_(inputSlice.GetRowCount())
{
    YT_VERIFY(inputSlice.IsLegacy);

    if (lowerKey) {
        LegacyLowerLimit_.MergeLowerKey(lowerKey);
    }
    if (upperKey) {
        LegacyUpperLimit_.MergeUpperKey(upperKey);
    }
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkSlice& inputSlice,
    const TComparator& comparator,
    TKeyBound lowerKeyBound,
    TKeyBound upperKeyBound)
    : InputChunk_(inputSlice.GetInputChunk())
    , LowerLimit_(inputSlice.LowerLimit())
    , UpperLimit_(inputSlice.UpperLimit())
    , IsLegacy(false)
    , PartIndex_(inputSlice.GetPartIndex())
    , SizeOverridden_(inputSlice.GetSizeOverridden())
    , DataWeight_(inputSlice.GetDataWeight())
    , RowCount_(inputSlice.GetRowCount())
{
    YT_VERIFY(!inputSlice.IsLegacy);

    LowerLimit_.KeyBound = comparator.StrongerKeyBound(LowerLimit_.KeyBound, lowerKeyBound);
    UpperLimit_.KeyBound = comparator.StrongerKeyBound(UpperLimit_.KeyBound, upperKeyBound);
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkSlice& chunkSlice,
    i64 lowerRowIndex,
    i64 upperRowIndex,
    i64 dataWeight)
    : InputChunk_(chunkSlice.GetInputChunk())
    , LegacyLowerLimit_(chunkSlice.LegacyLowerLimit())
    , LegacyUpperLimit_(chunkSlice.LegacyUpperLimit())
    , LowerLimit_(chunkSlice.LowerLimit())
    , UpperLimit_(chunkSlice.UpperLimit())
    , IsLegacy(chunkSlice.IsLegacy)
{
    if (IsLegacy) {
        LegacyLowerLimit_.RowIndex = lowerRowIndex;
        LegacyUpperLimit_.RowIndex = upperRowIndex;
    } else {
        LowerLimit_.RowIndex = lowerRowIndex;
        UpperLimit_.RowIndex = upperRowIndex;
    }
    OverrideSize(upperRowIndex - lowerRowIndex, dataWeight);
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    int partIndex,
    i64 lowerRowIndex,
    i64 upperRowIndex,
    i64 dataWeight)
    : InputChunk_(inputChunk)
    , PartIndex_(partIndex)
{
    if (inputChunk->LowerLimit()) {
        LegacyLowerLimit_ = TLegacyInputSliceLimit(*inputChunk->LowerLimit());
    }
    LegacyLowerLimit_.MergeLowerRowIndex(lowerRowIndex);

    if (inputChunk->UpperLimit()) {
        LegacyUpperLimit_ = TLegacyInputSliceLimit(*inputChunk->UpperLimit());
    }
    LegacyUpperLimit_.MergeUpperRowIndex(upperRowIndex);

    OverrideSize(*LegacyUpperLimit_.RowIndex - *LegacyLowerLimit_.RowIndex, std::max<i64>(1, dataWeight * inputChunk->GetColumnSelectivityFactor()));
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    const TRowBufferPtr& rowBuffer,
    const NProto::TChunkSlice& protoChunkSlice,
    TRange<TLegacyKey> keySet)
    : TInputChunkSlice(inputChunk)
{
    LegacyLowerLimit_.MergeLowerLimit(TLegacyInputSliceLimit(protoChunkSlice.lower_limit(), rowBuffer, keySet));
    LegacyUpperLimit_.MergeUpperLimit(TLegacyInputSliceLimit(protoChunkSlice.upper_limit(), rowBuffer, keySet));
    PartIndex_ = DefaultPartIndex;

    if (protoChunkSlice.has_row_count_override() || protoChunkSlice.has_data_weight_override()) {
        YT_VERIFY((protoChunkSlice.has_row_count_override() && protoChunkSlice.has_data_weight_override()));
        OverrideSize(protoChunkSlice.row_count_override(), std::max<i64>(1, protoChunkSlice.data_weight_override() * inputChunk->GetColumnSelectivityFactor()));
    }
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkSlice& chunkSlice,
    const TRowBufferPtr& rowBuffer,
    const NProto::TChunkSlice& protoChunkSlice,
    TRange<TLegacyKey> keySet)
    : InputChunk_(chunkSlice.GetInputChunk())
    , LegacyLowerLimit_(chunkSlice.LegacyLowerLimit())
    , LegacyUpperLimit_(chunkSlice.LegacyUpperLimit())
    , IsLegacy(chunkSlice.IsLegacy)
{
    YT_VERIFY(chunkSlice.IsLegacy);
    LegacyLowerLimit_.MergeLowerLimit(TLegacyInputSliceLimit(protoChunkSlice.lower_limit(), rowBuffer, keySet));
    LegacyUpperLimit_.MergeUpperLimit(TLegacyInputSliceLimit(protoChunkSlice.upper_limit(), rowBuffer, keySet));

    PartIndex_ = DefaultPartIndex;

    if (protoChunkSlice.has_row_count_override() || protoChunkSlice.has_data_weight_override()) {
        YT_VERIFY(protoChunkSlice.has_row_count_override() && protoChunkSlice.has_data_weight_override());
        OverrideSize(protoChunkSlice.row_count_override(), std::max<i64>(1, protoChunkSlice.data_weight_override() * chunkSlice.GetInputChunk()->GetColumnSelectivityFactor()));
    }
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkSlice& chunkSlice,
    const TComparator& comparator,
    const TRowBufferPtr& rowBuffer,
    const NProto::TChunkSlice& protoChunkSlice,
    TRange<TLegacyKey> keySet)
    : InputChunk_(chunkSlice.GetInputChunk())
    , LegacyLowerLimit_(chunkSlice.LegacyLowerLimit())
    , LegacyUpperLimit_(chunkSlice.LegacyUpperLimit())
    , IsLegacy(chunkSlice.IsLegacy)
{
    YT_VERIFY(!chunkSlice.IsLegacy);
    LowerLimit_.MergeLower(TInputSliceLimit(protoChunkSlice.lower_limit(), rowBuffer, keySet, comparator.GetLength(), /* isUpper */ false), comparator);
    UpperLimit_.MergeUpper(TInputSliceLimit(protoChunkSlice.upper_limit(), rowBuffer, keySet, comparator.GetLength(), /* isUpper */ true), comparator);

    PartIndex_ = DefaultPartIndex;

    if (protoChunkSlice.has_row_count_override() || protoChunkSlice.has_data_weight_override()) {
        YT_VERIFY(protoChunkSlice.has_row_count_override() && protoChunkSlice.has_data_weight_override());
        OverrideSize(protoChunkSlice.row_count_override(), std::max<i64>(1, protoChunkSlice.data_weight_override() * chunkSlice.GetInputChunk()->GetColumnSelectivityFactor()));
    }
}

TInputChunkSlice::TInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    const TRowBufferPtr& rowBuffer,
    const NProto::TChunkSpec& protoChunkSpec)
    : TInputChunkSlice(inputChunk)
{
    static TRange<TLegacyKey> DummyKeys;
    LegacyLowerLimit_.MergeLowerLimit(TLegacyInputSliceLimit(protoChunkSpec.lower_limit(), rowBuffer, DummyKeys));
    LegacyUpperLimit_.MergeUpperLimit(TLegacyInputSliceLimit(protoChunkSpec.upper_limit(), rowBuffer, DummyKeys));
    PartIndex_ = DefaultPartIndex;

    if (protoChunkSpec.has_row_count_override() || protoChunkSpec.has_data_weight_override()) {
        YT_VERIFY((protoChunkSpec.has_row_count_override() && protoChunkSpec.has_data_weight_override()));
        OverrideSize(protoChunkSpec.row_count_override(), std::max<i64>(1, protoChunkSpec.data_weight_override() * inputChunk->GetColumnSelectivityFactor()));
    }
}

std::vector<TInputChunkSlicePtr> TInputChunkSlice::SliceEvenly(i64 sliceDataWeight, i64 sliceRowCount, TRowBufferPtr rowBuffer) const
{
    YT_VERIFY(sliceDataWeight > 0);
    YT_VERIFY(sliceRowCount > 0);

    i64 lowerRowIndex;
    i64 upperRowIndex;

    if (IsLegacy) {
        lowerRowIndex = LegacyLowerLimit_.RowIndex.value_or(0);
        upperRowIndex = LegacyUpperLimit_.RowIndex.value_or(InputChunk_->GetRowCount());
    } else {
        lowerRowIndex = LowerLimit_.RowIndex.value_or(0);
        upperRowIndex = UpperLimit_.RowIndex.value_or(InputChunk_->GetRowCount());
    }
    i64 rowCount = upperRowIndex - lowerRowIndex;

    i64 count = std::max(DivCeil(GetDataWeight(), sliceDataWeight), DivCeil(rowCount, sliceRowCount));
    count = std::max(std::min(count, rowCount), static_cast<i64>(1));

    std::vector<TInputChunkSlicePtr> result;
    for (i64 i = 0; i < count; ++i) {
        i64 sliceLowerRowIndex = lowerRowIndex + rowCount * i / count;
        i64 sliceUpperRowIndex = lowerRowIndex + rowCount * (i + 1) / count;
        if (sliceLowerRowIndex < sliceUpperRowIndex) {
            result.push_back(New<TInputChunkSlice>(
                *this,
                sliceLowerRowIndex,
                sliceUpperRowIndex,
                DivCeil(GetDataWeight(), count)));
        }
    }
    if (rowBuffer) {
        result.front()
            ->LegacyLowerLimit().Key = rowBuffer->Capture(LegacyLowerLimit_.Key);
        result.back()
            ->LegacyUpperLimit().Key = rowBuffer->Capture(LegacyUpperLimit_.Key);
    }

    for (const auto& slice : result) {
        YT_VERIFY(slice->IsLegacy == IsLegacy);
    }

    return result;
}

std::pair<TInputChunkSlicePtr, TInputChunkSlicePtr> TInputChunkSlice::SplitByRowIndex(i64 splitRow) const
{
    i64 lowerRowIndex;
    i64 upperRowIndex;
    if (IsLegacy) {
        lowerRowIndex = LegacyLowerLimit_.RowIndex.value_or(0);
        upperRowIndex = LegacyUpperLimit_.RowIndex.value_or(InputChunk_->GetRowCount());
    } else {
        lowerRowIndex = LowerLimit_.RowIndex.value_or(0);
        upperRowIndex = UpperLimit_.RowIndex.value_or(InputChunk_->GetRowCount());
    }

    i64 rowCount = upperRowIndex - lowerRowIndex;

    YT_VERIFY(splitRow >= 0 && splitRow <= rowCount);

    return std::make_pair(
        New<TInputChunkSlice>(
            *this,
            lowerRowIndex,
            lowerRowIndex + splitRow,
            std::max<i64>(1, GetDataWeight() * 1.0 / rowCount * splitRow)),
        New<TInputChunkSlice>(
            *this,
            lowerRowIndex + splitRow,
            upperRowIndex,
            std::max<i64>(1, GetDataWeight() * 1.0 / rowCount * (rowCount - splitRow))));
}

i64 TInputChunkSlice::GetLocality(int replicaPartIndex) const
{
    i64 result = GetDataWeight();

    if (PartIndex_ == DefaultPartIndex) {
        // For erasure chunks without specified part index,
        // data size is assumed to be split evenly between data parts.
        auto codecId = InputChunk_->GetErasureCodec();
        if (codecId != NErasure::ECodec::None) {
            auto* codec = NErasure::GetCodec(codecId);
            int dataPartCount = codec->GetDataPartCount();
            result = (result + dataPartCount - 1) / dataPartCount;
        }
    } else if (PartIndex_ != replicaPartIndex) {
        result = 0;
    }

    return result;
}

int TInputChunkSlice::GetPartIndex() const
{
    return PartIndex_;
}

i64 TInputChunkSlice::GetMaxBlockSize() const
{
    return InputChunk_->GetMaxBlockSize();
}

bool TInputChunkSlice::GetSizeOverridden() const
{
    return SizeOverridden_;
}

i64 TInputChunkSlice::GetDataWeight() const
{
    return SizeOverridden_ ? DataWeight_ : InputChunk_->GetDataWeight();
}

i64 TInputChunkSlice::GetRowCount() const
{
    return SizeOverridden_ ? RowCount_ : InputChunk_->GetRowCount();
}

void TInputChunkSlice::OverrideSize(i64 rowCount, i64 dataWeight)
{
    RowCount_ = rowCount;
    DataWeight_ = dataWeight;
    SizeOverridden_ = true;
}

void TInputChunkSlice::ApplySamplingSelectivityFactor(double samplingSelectivityFactor)
{
    i64 rowCount = GetRowCount() * samplingSelectivityFactor;
    i64 dataWeight = GetDataWeight() * samplingSelectivityFactor;
    OverrideSize(rowCount, dataWeight);
}

void TInputChunkSlice::TransformToLegacy(const TRowBufferPtr& rowBuffer)
{
    YT_VERIFY(!IsLegacy);

    LegacyLowerLimit_.RowIndex = LowerLimit_.RowIndex;
    if (LowerLimit_.KeyBound.IsUniversal()) {
        LegacyLowerLimit_.Key = TLegacyKey();
    } else {
        LegacyLowerLimit_.Key = KeyBoundToLegacyRow(LowerLimit_.KeyBound, rowBuffer);
    }
    LegacyUpperLimit_.RowIndex = UpperLimit_.RowIndex;
    if (UpperLimit_.KeyBound.IsUniversal()) {
        LegacyUpperLimit_.Key = TLegacyKey();
    } else {
        LegacyUpperLimit_.Key = KeyBoundToLegacyRow(UpperLimit_.KeyBound, rowBuffer);
    }
    LowerLimit_ = TInputSliceLimit();
    UpperLimit_ = TInputSliceLimit();

    IsLegacy = true;
}

void TInputChunkSlice::TransformToNew(const TRowBufferPtr& rowBuffer, int keyLength)
{
    YT_VERIFY(IsLegacy);

    LowerLimit_.RowIndex = LegacyLowerLimit_.RowIndex;
    LowerLimit_.KeyBound = KeyBoundFromLegacyRow(LegacyLowerLimit_.Key, /* isUpper */ false, keyLength, rowBuffer);
    UpperLimit_.RowIndex = LegacyUpperLimit_.RowIndex;
    UpperLimit_.KeyBound = KeyBoundFromLegacyRow(LegacyUpperLimit_.Key, /* isUpper */ true, keyLength, rowBuffer);
    LegacyLowerLimit_ = TLegacyInputSliceLimit();
    LegacyUpperLimit_ = TLegacyInputSliceLimit();

    IsLegacy = false;
}

void TInputChunkSlice::TransformToNewKeyless()
{
    YT_VERIFY(IsLegacy);
    YT_VERIFY(!LegacyLowerLimit_.Key);
    YT_VERIFY(!LegacyUpperLimit_.Key);
    LowerLimit_.RowIndex = LegacyLowerLimit_.RowIndex;
    UpperLimit_.RowIndex = LegacyUpperLimit_.RowIndex;
    LegacyLowerLimit_ = TLegacyInputSliceLimit();
    LegacyUpperLimit_ = TLegacyInputSliceLimit();

    IsLegacy = false;
}

void TInputChunkSlice::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, InputChunk_);
    Persist(context, LegacyLowerLimit_);
    Persist(context, LegacyUpperLimit_);
    Persist(context, LowerLimit_);
    Persist(context, UpperLimit_);
    Persist(context, IsLegacy);
    Persist(context, PartIndex_);
    Persist(context, SizeOverridden_);
    Persist(context, RowCount_);
    Persist(context, DataWeight_);
}

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TInputChunkSlicePtr& slice)
{
    return Format("ChunkId: %v, LowerLimit: %v, UpperLimit: %v, RowCount: %v, DataWeight: %v, PartIndex: %v",
        slice->GetInputChunk()->ChunkId(),
        slice->IsLegacy ? ToString(slice->LegacyLowerLimit()) : ToString(slice->LowerLimit()),
        slice->IsLegacy ? ToString(slice->LegacyUpperLimit()) : ToString(slice->UpperLimit()),
        slice->GetRowCount(),
        slice->GetDataWeight(),
        slice->GetPartIndex());
}

////////////////////////////////////////////////////////////////////////////////

TInputChunkSlicePtr CreateInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    TLegacyKey lowerKey,
    TLegacyKey upperKey)
{
    return New<TInputChunkSlice>(inputChunk, lowerKey, upperKey);
}

TInputChunkSlicePtr CreateInputChunkSlice(const TInputChunkSlice& inputSlice)
{
    return New<TInputChunkSlice>(inputSlice);
}

TInputChunkSlicePtr CreateInputChunkSlice(
    const TInputChunkSlice& inputSlice,
    TLegacyKey lowerKey,
    TLegacyKey upperKey)
{
    return New<TInputChunkSlice>(inputSlice, lowerKey, upperKey);
}

TInputChunkSlicePtr CreateInputChunkSlice(
    const TInputChunkSlice& inputSlice,
    const TComparator& comparator,
    TKeyBound lowerKeyBound,
    TKeyBound upperKeyBound)
{
    return New<TInputChunkSlice>(inputSlice, comparator, lowerKeyBound, upperKeyBound);
}

TInputChunkSlicePtr CreateInputChunkSlice(
    const TInputChunkPtr& inputChunk,
    const NTableClient::TRowBufferPtr& rowBuffer,
    const NProto::TChunkSpec& protoChunkSpec)
{
    return New<TInputChunkSlice>(inputChunk, rowBuffer, protoChunkSpec);
}

std::vector<TInputChunkSlicePtr> CreateErasureInputChunkSlices(
    const TInputChunkPtr& inputChunk,
    NErasure::ECodec codecId)
{
    std::vector<TInputChunkSlicePtr> slices;

    i64 dataSize = inputChunk->GetUncompressedDataSize();
    i64 rowCount = inputChunk->GetRowCount();

    auto* codec = NErasure::GetCodec(codecId);
    int dataPartCount = codec->GetDataPartCount();

    for (int partIndex = 0; partIndex < dataPartCount; ++partIndex) {
        i64 sliceLowerRowIndex = rowCount * partIndex / dataPartCount;
        i64 sliceUpperRowIndex = rowCount * (partIndex + 1) / dataPartCount;
        if (sliceLowerRowIndex < sliceUpperRowIndex) {
            auto chunkSlice = New<TInputChunkSlice>(
                inputChunk,
                partIndex,
                sliceLowerRowIndex,
                sliceUpperRowIndex,
                (dataSize + dataPartCount - 1) / dataPartCount);
            slices.emplace_back(std::move(chunkSlice));
        }
    }

    return slices;
}

void InferLimitsFromBoundaryKeys(const TInputChunkSlicePtr& chunkSlice, const TRowBufferPtr& rowBuffer, std::optional<int> keyColumnCount)
{
    if (const auto& boundaryKeys = chunkSlice->GetInputChunk()->BoundaryKeys()) {
        if (keyColumnCount) {
            chunkSlice->LegacyLowerLimit().MergeLowerKey(GetStrictKey(boundaryKeys->MinKey, *keyColumnCount, rowBuffer));
            chunkSlice->LegacyUpperLimit().MergeUpperKey(GetStrictKeySuccessor(boundaryKeys->MaxKey, *keyColumnCount, rowBuffer));
        } else {
            chunkSlice->LegacyLowerLimit().MergeLowerKey(boundaryKeys->MinKey);
            chunkSlice->LegacyUpperLimit().MergeUpperKey(GetKeySuccessor(boundaryKeys->MaxKey, rowBuffer));
        }
    }
}

std::vector<TInputChunkSlicePtr> SliceChunkByRowIndexes(
    const TInputChunkPtr& inputChunk,
    i64 sliceDataWeight,
    i64 sliceRowCount)
{
    return CreateInputChunkSlice(inputChunk)->SliceEvenly(sliceDataWeight, sliceRowCount);
}

void ToProto(NProto::TChunkSpec* chunkSpec, const TInputChunkSlicePtr& inputSlice, std::optional<TComparator> comparator, EDataSourceType dataSourceType)
{
    // The chunk spec in the slice has arrived from master, so it can't possibly contain any extensions
    // except misc and boundary keys (in sorted merge or reduce). Jobs request boundary keys
    // from the nodes when needed, so we remove it here, to optimize traffic from the scheduler and
    // proto serialization time.

    ToProto(chunkSpec, inputSlice->GetInputChunk(), dataSourceType);

    if (inputSlice->IsLegacy) {
        if (!IsTrivial(inputSlice->LegacyLowerLimit())) {
            // NB(psushin): if lower limit key is less than min chunk key, we can eliminate it from job spec.
            // Moreover, it is important for GetJobInputPaths handle to work properly.
            bool pruneKeyLimit = dataSourceType == EDataSourceType::UnversionedTable
                && inputSlice->LegacyLowerLimit().Key
                && inputSlice->GetInputChunk()->BoundaryKeys()
                && inputSlice->LegacyLowerLimit().Key <= inputSlice->GetInputChunk()->BoundaryKeys()->MinKey;

            if (pruneKeyLimit && inputSlice->LegacyLowerLimit().RowIndex) {
                TLegacyInputSliceLimit inputSliceLimit;
                inputSliceLimit.RowIndex = inputSlice->LegacyLowerLimit().RowIndex;
                ToProto(chunkSpec->mutable_lower_limit(), inputSliceLimit);
            } else if (!pruneKeyLimit) {
                ToProto(chunkSpec->mutable_lower_limit(), inputSlice->LegacyLowerLimit());
            }
        }

        if (!IsTrivial(inputSlice->LegacyUpperLimit())) {
            // NB(psushin): if upper limit key is greater than max chunk key, we can eliminate it from job spec.
            // Moreover, it is important for GetJobInputPaths handle to work properly.
            bool pruneKeyLimit = dataSourceType == EDataSourceType::UnversionedTable
                && inputSlice->LegacyUpperLimit().Key
                && inputSlice->GetInputChunk()->BoundaryKeys()
                && inputSlice->LegacyUpperLimit().Key > inputSlice->GetInputChunk()->BoundaryKeys()->MaxKey;

            if (pruneKeyLimit && inputSlice->LegacyUpperLimit().RowIndex) {
                TLegacyInputSliceLimit inputSliceLimit;
                inputSliceLimit.RowIndex = inputSlice->LegacyUpperLimit().RowIndex;
                ToProto(chunkSpec->mutable_upper_limit(), inputSliceLimit);
            } else if (!pruneKeyLimit) {
                ToProto(chunkSpec->mutable_upper_limit(), inputSlice->LegacyUpperLimit());
            }
        }
    } else {
        // TODO(max42): YT-13961. Revise this logic.
        // TODO(max42): YT-14023. NB: right now we MUST keep pruning key bounds that are implied by chunk boundary keys
        // as failure to do so would break readers when reducing by shorter key than present in chunk schema.
        // Do not remove this logic unless there are no more nodes on 20.3.

        auto chunkMinKeyBound = TKeyBound::MakeUniversal(/* isUpper */ false);
        auto chunkMaxKeyBound = TKeyBound::MakeUniversal(/* isUpper */ true);

        // NB: for dynamic table data slices involving dynamic stores boundary keys may contain sentinels.
        // But we do not prune limits for them anyway.
        if (const auto& boundaryKeys = inputSlice->GetInputChunk()->BoundaryKeys();
            boundaryKeys && dataSourceType == EDataSourceType::UnversionedTable)
        {
            chunkMinKeyBound = TKeyBound::FromRow(boundaryKeys->MinKey, /* isInclusive */ true, /* isUpper */ false);
            chunkMaxKeyBound = TKeyBound::FromRow(boundaryKeys->MaxKey, /* isInclusive */ true, /* isUpper */ true);
        }

        // NB: we prune non-trivial key bounds only if comparator is passed.
        // In particular, sorted controller always passes comparator. In the rest
        // of cases we do not prune it but it will not trigger YT-14023 as key lengths
        // will be proper (due to marvelous coincedence).

        if (!inputSlice->LowerLimit().IsTrivial()) {
            auto lowerLimitToSerialize = inputSlice->LowerLimit();
            if (!inputSlice->LowerLimit().KeyBound || inputSlice->LowerLimit().KeyBound.IsUniversal() ||
                (dataSourceType == EDataSourceType::UnversionedTable && comparator &&
                comparator->CompareKeyBounds(inputSlice->LowerLimit().KeyBound, chunkMinKeyBound) <= 0))
            {
                lowerLimitToSerialize.KeyBound = TKeyBound();
            }
            ToProto(chunkSpec->mutable_lower_limit(), lowerLimitToSerialize);
        }

        if (!inputSlice->UpperLimit().IsTrivial()) {
            auto upperLimitToSerialize = inputSlice->UpperLimit();
            if (!inputSlice->UpperLimit().KeyBound || inputSlice->UpperLimit().KeyBound.IsUniversal() ||
                (dataSourceType == EDataSourceType::UnversionedTable &&
                comparator && comparator->CompareKeyBounds(inputSlice->UpperLimit().KeyBound, chunkMaxKeyBound) >= 0))
            {
                upperLimitToSerialize.KeyBound = TKeyBound();
            }
            ToProto(chunkSpec->mutable_upper_limit(), upperLimitToSerialize);
        }
    }

    chunkSpec->set_data_weight_override(inputSlice->GetDataWeight());

    // NB(psushin): always setting row_count_override is important for GetJobInputPaths handle to work properly.
    chunkSpec->set_row_count_override(inputSlice->GetRowCount());
    if (inputSlice->GetInputChunk()->IsDynamicStore()) {
        ToProto(chunkSpec->mutable_tablet_id(), inputSlice->GetInputChunk()->TabletId());
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
