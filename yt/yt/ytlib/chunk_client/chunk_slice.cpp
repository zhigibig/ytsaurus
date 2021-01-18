#include "chunk_slice.h"
#include "private.h"
#include "chunk_meta_extensions.h"
#include "key_set.h"

#include <yt/ytlib/table_client/chunk_meta_extensions.h>

#include <yt/client/table_client/comparator.h>
#include <yt/client/table_client/key_bound.h>
#include <yt/client/table_client/schema.h>

#include <yt/library/erasure/codec.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/numeric_helpers.h>
#include <yt/core/misc/protobuf_helpers.h>

#include <cmath>

namespace NYT::NChunkClient {

using namespace NTableClient;
using namespace NTableClient::NProto;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TChunkSlice& slice)
{
    return Format("LowerLimit: %v, UpperLimit: %v, RowCount: %v, DataWeight: %v",
        slice.LowerLimit,
        slice.UpperLimit,
        slice.RowCount,
        slice.DataWeight);
}

////////////////////////////////////////////////////////////////////////////////

class TSortedChunkSlicer
    : private TNonCopyable
{
public:
    TSortedChunkSlicer(const NProto::TSliceRequest& sliceReq, const NProto::TChunkMeta& meta)
        : SliceReq_(sliceReq)
        , Meta_(meta)
    {
        auto chunkFormat = ETableChunkFormat(Meta_.version());
        switch (chunkFormat) {
            case ETableChunkFormat::SchemalessHorizontal:
            case ETableChunkFormat::UnversionedColumnar:
            case ETableChunkFormat::VersionedSimple:
            case ETableChunkFormat::VersionedColumnar:
                break;
            default:
                auto chunkId = FromProto<TChunkId>(SliceReq_.chunk_id());
                THROW_ERROR_EXCEPTION("Unsupported format %Qlv for chunk %v",
                    ETableChunkFormat(Meta_.version()),
                    chunkId);
        }

        TComparator chunkComparator;
        if (auto schemaExt = FindProtoExtension<TTableSchemaExt>(Meta_.extensions())) {
            chunkComparator = FromProto<TTableSchema>(*schemaExt).ToComparator();
        } else {
            // NB(gritukan): Very old chunks do not have schema, but they are always sorted in ascending order.
            auto keyColumnsExt = GetProtoExtension<TKeyColumnsExt>(Meta_.extensions());
            int keyColumnCount = keyColumnsExt.names_size();
            chunkComparator = TComparator(std::vector<ESortOrder>(keyColumnCount, ESortOrder::Ascending));
        }

        if (SliceReq_.key_column_count() > chunkComparator.GetLength()) {
            THROW_ERROR_EXCEPTION("Slice request has more key columns than chunk")
                << TErrorAttribute("chunk_key_column_count", chunkComparator.GetLength())
                << TErrorAttribute("request_key_column_count", SliceReq_.key_column_count());
        }
        SliceComparator_ = chunkComparator.Trim(SliceReq_.key_column_count());

        YT_VERIFY(FindBoundaryKeyBounds(Meta_, &ChunkLowerBound_, &ChunkUpperBound_));
        ChunkLowerBound_ = ShortenKeyBound(ChunkLowerBound_, SliceComparator_.GetLength());
        ChunkUpperBound_ = ShortenKeyBound(ChunkUpperBound_, SliceComparator_.GetLength());

        auto miscExt = GetProtoExtension<NProto::TMiscExt>(Meta_.extensions());
        i64 chunkDataWeight = miscExt.has_data_weight()
            ? miscExt.data_weight()
            : miscExt.uncompressed_data_size();

        i64 chunkRowCount = miscExt.row_count();
        YT_VERIFY(chunkRowCount > 0);

        DataWeightPerRow_ = std::max((i64)1, chunkDataWeight / chunkRowCount);

        auto blockMetaExt = GetProtoExtension<TBlockMetaExt>(Meta_.extensions());

        auto blockCount = blockMetaExt.blocks_size();
        BlockDescriptors_.reserve(blockCount);
        for (int blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
            const auto& block = blockMetaExt.blocks(blockIndex);
            YT_VERIFY(block.block_index() == blockIndex);

            auto blockLastKey = FromProto<TUnversionedOwningRow>(block.last_key());
            TUnversionedOwningRow trimedBlockLastKey(
                blockLastKey.begin(),
                blockLastKey.begin() + SliceComparator_.GetLength());
            auto blockUpperBound = TOwningKeyBound::FromRow(
                /* row */std::move(trimedBlockLastKey),
                /* isInclusive */true,
                /* isUpper */true);

            i64 chunkRowCount = block.chunk_row_count();
            i64 rowCount = BlockDescriptors_.empty()
                ? chunkRowCount
                : chunkRowCount - BlockDescriptors_.back().RowCount;

            TBlockDescriptor blockDescriptor{
                .UpperBound = blockUpperBound,
                .RowCount = rowCount,
                .ChunkRowCount = chunkRowCount,
            };
            BlockDescriptors_.push_back(std::move(blockDescriptor));
        }

        TReadLimit sliceLowerLimit(sliceReq.lower_limit(), /* isLower */false, SliceComparator_.GetLength());
        TReadLimit sliceUpperLimit(sliceReq.upper_limit(), /* isUpper */true, SliceComparator_.GetLength());

        if (sliceLowerLimit.KeyBound()) {
            SliceLowerBound_ = sliceLowerLimit.KeyBound();
        } else {
            SliceLowerBound_ = ChunkLowerBound_;
        }

        if (sliceUpperLimit.KeyBound()) {
            SliceUpperBound_ = sliceUpperLimit.KeyBound();
        } else {
            SliceUpperBound_ = ChunkUpperBound_;
        }

        SliceStartRowIndex_ = sliceLowerLimit.GetRowIndex().value_or(0);
        SliceEndRowIndex_ = sliceUpperLimit.GetRowIndex().value_or(chunkRowCount);
    }

    std::vector<TChunkSlice> Slice()
    {
        i64 sliceDataWeight = SliceReq_.slice_data_weight();
        bool sliceByKeys = SliceReq_.slice_by_keys();

        std::vector<TChunkSlice> slices;

        bool sliceStarted = false;
        TOwningKeyBound currentSliceLowerBound;
        i64 currentSliceStartRowIndex = 0;

        auto startSlice = [&] (
            const TOwningKeyBound& sliceLowerBound,
            i64 sliceStartRowIndex)
        {
            YT_VERIFY(!sliceStarted);
            sliceStarted = true;

            currentSliceLowerBound = sliceLowerBound;
            currentSliceStartRowIndex = sliceStartRowIndex;
        };

        auto endSlice = [&] (
            const TOwningKeyBound& sliceUpperBound,
            i64 sliceEndRowIndex)
        {
            YT_VERIFY(sliceStarted);
            sliceStarted = false;

            TChunkSlice slice;
            slice.LowerLimit.KeyBound() = currentSliceLowerBound;
            slice.UpperLimit.KeyBound() = sliceUpperBound;
            slice.RowCount = sliceEndRowIndex - currentSliceStartRowIndex;
            slice.DataWeight = slice.RowCount * DataWeightPerRow_;
            if (!sliceByKeys) {
                slice.LowerLimit.SetRowIndex(currentSliceStartRowIndex);
                slice.UpperLimit.SetRowIndex(sliceEndRowIndex);
            }
            slices.push_back(std::move(slice));
        };

        // Upper bounds of intersection of last block and request.
        TOwningKeyBound lastBlockUpperBound;
        i64 lastBlockEndRowIndex = -1;

        for (int blockIndex = 0; blockIndex < BlockDescriptors_.size(); ++blockIndex) {
            const auto& block = BlockDescriptors_[blockIndex];

            auto blockLowerBound = blockIndex == 0
                ? ChunkLowerBound_
                : BlockDescriptors_[blockIndex - 1].UpperBound.Invert();
            const auto& blockUpperBound = block.UpperBound;

            // This might happen if block consisnts of single key.
            if (SliceComparator_.IsRangeEmpty(blockLowerBound, blockUpperBound)) {
                blockLowerBound = blockLowerBound.ToggleInclusiveness();
                YT_VERIFY(!SliceComparator_.IsRangeEmpty(blockLowerBound, blockUpperBound));
            }

            i64 blockStartRowIndex = blockIndex == 0
                ? 0
                : BlockDescriptors_[blockIndex - 1].ChunkRowCount;
            i64 blockEndRowIndex = block.ChunkRowCount;

            // Block is completely to the left of the request by keys.
            if (SliceComparator_.IsRangeEmpty(SliceLowerBound_, blockUpperBound)) {
                continue;
            }
            // Block is completely to the right of the request by row indices.
            if (SliceStartRowIndex_ >= blockEndRowIndex) {
                continue;
            }

            // Block is completely to the right of the request by keys.
            if (SliceComparator_.IsRangeEmpty(blockLowerBound, SliceUpperBound_)) {
                break;
            }
            // Block is completely to the right of the request by keys.
            if (SliceEndRowIndex_ <= blockStartRowIndex) {
                break;
            }

            // Intersect block's ranges with request's ranges.
            const auto& lowerBound = SliceComparator_.CompareKeyBounds(blockLowerBound, SliceLowerBound_) > 0
                ? blockLowerBound
                : SliceLowerBound_;

            const auto& upperBound = SliceComparator_.CompareKeyBounds(blockUpperBound, SliceUpperBound_) < 0
                ? blockUpperBound
                : SliceUpperBound_;

            i64 startRowIndex = std::max<i64>(blockStartRowIndex, SliceStartRowIndex_);
            i64 endRowIndex = std::min<i64>(blockEndRowIndex, SliceEndRowIndex_);

            if (!sliceStarted) {
                startSlice(lowerBound, startRowIndex);
            }

            if (sliceByKeys) {
                bool canSliceHere = true;
                i64 currentSliceRowCount = endRowIndex - currentSliceStartRowIndex;
                if (blockIndex + 1 < BlockDescriptors_.size()) {
                    const auto& nextBlock = BlockDescriptors_[blockIndex + 1];
                    // We are inside maniac key, so can't slice chunk here.
                    if (upperBound == nextBlock.UpperBound) {
                        canSliceHere = false;
                    }
                }
                if (canSliceHere && currentSliceRowCount * DataWeightPerRow_ >= sliceDataWeight) {
                    endSlice(upperBound, endRowIndex);
                }
            } else {
                i64 rowsPerDataSlice = std::max<i64>(1, DivCeil<i64>(sliceDataWeight, DataWeightPerRow_));
                while (endRowIndex - currentSliceStartRowIndex >= rowsPerDataSlice) {
                    i64 currentSliceEndRowIndex = currentSliceStartRowIndex + rowsPerDataSlice;
                    YT_VERIFY(currentSliceEndRowIndex > startRowIndex &&
                        currentSliceEndRowIndex <= endRowIndex);

                    endSlice(upperBound, currentSliceEndRowIndex);

                    if (currentSliceEndRowIndex < endRowIndex) {
                        startSlice(lowerBound, currentSliceEndRowIndex);
                    } else {
                        break;
                    }
                }
            }

            lastBlockUpperBound = upperBound;
            lastBlockEndRowIndex = endRowIndex;
        }

        // Finish last slice.
        if (sliceStarted) {
            endSlice(lastBlockUpperBound, lastBlockEndRowIndex);
        }

        return slices;
    }

private:
    //! Represents a block of chunk.
    struct TBlockDescriptor
    {
        //! Keys upper bound in block.
        TOwningKeyBound UpperBound;

        //! Amount of rows in block.
        i64 RowCount;

        //! Total amount of rows in block and all the previous rows.
        i64 ChunkRowCount;
    };
    std::vector<TBlockDescriptor> BlockDescriptors_;

    const NProto::TSliceRequest& SliceReq_;
    const NProto::TChunkMeta& Meta_;

    TOwningKeyBound SliceLowerBound_;
    TOwningKeyBound SliceUpperBound_;

    i64 SliceStartRowIndex_;
    i64 SliceEndRowIndex_;

    TComparator SliceComparator_;

    TOwningKeyBound ChunkLowerBound_;
    TOwningKeyBound ChunkUpperBound_;

    i64 DataWeightPerRow_ = 0;
};

////////////////////////////////////////////////////////////////////////////////

std::vector<TChunkSlice> SliceChunk(
    const NProto::TSliceRequest& sliceReq,
    const NProto::TChunkMeta& meta)
{
    TSortedChunkSlicer slicer(sliceReq, meta);
    return slicer.Slice();
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TChunkSlice* protoChunkSlice, const TChunkSlice& chunkSlice)
{
    if (!chunkSlice.LowerLimit.IsTrivial()) {
        ToProto(protoChunkSlice->mutable_lower_limit(), chunkSlice.LowerLimit);
    }
    if (!chunkSlice.UpperLimit.IsTrivial()) {
        ToProto(protoChunkSlice->mutable_upper_limit(), chunkSlice.UpperLimit);
    }
    protoChunkSlice->set_data_weight_override(chunkSlice.DataWeight);
    protoChunkSlice->set_row_count_override(chunkSlice.RowCount);
}

void ToProto(
    const TKeySetWriterPtr& keysWriter,
    const TKeySetWriterPtr& keyBoundsWriter,
    NProto::TChunkSlice* protoChunkSlice,
    const TChunkSlice& chunkSlice)
{
    if (chunkSlice.LowerLimit.KeyBound()) {
        int index = keyBoundsWriter->WriteKey(chunkSlice.LowerLimit.KeyBound().Prefix);
        protoChunkSlice->mutable_lower_limit()->set_key_bound_is_inclusive(chunkSlice.LowerLimit.KeyBound().IsInclusive);
        YT_VERIFY(keysWriter->WriteKey(KeyBoundToLegacyRow(chunkSlice.LowerLimit.KeyBound())) == index);
        protoChunkSlice->mutable_lower_limit()->set_key_index(index);
    }

    if (chunkSlice.LowerLimit.GetRowIndex()) {
        protoChunkSlice->mutable_lower_limit()->set_row_index(*chunkSlice.LowerLimit.GetRowIndex());
    }

    if (chunkSlice.UpperLimit.KeyBound()) {
        int index = keyBoundsWriter->WriteKey(chunkSlice.UpperLimit.KeyBound().Prefix);
        protoChunkSlice->mutable_upper_limit()->set_key_bound_is_inclusive(chunkSlice.UpperLimit.KeyBound().IsInclusive);
        YT_VERIFY(keysWriter->WriteKey(KeyBoundToLegacyRow(chunkSlice.UpperLimit.KeyBound())) == index);
        protoChunkSlice->mutable_upper_limit()->set_key_index(index);
    }

    if (chunkSlice.UpperLimit.GetRowIndex()) {
        protoChunkSlice->mutable_upper_limit()->set_row_index(*chunkSlice.UpperLimit.GetRowIndex());
    }

    protoChunkSlice->set_data_weight_override(chunkSlice.DataWeight);
    protoChunkSlice->set_row_count_override(chunkSlice.RowCount);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
