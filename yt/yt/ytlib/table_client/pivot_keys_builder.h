#pragma once

#include "public.h"

#include <yt/yt/ytlib/table_client/slice_boundary_key.h>

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/client/table_client/comparator.h>

namespace NYT::NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TReshardPivotKeysBuilder
{
public:
    DEFINE_BYVAL_RW_PROPERTY(i64, ExpectedTabletSize);

public:
    TReshardPivotKeysBuilder(
        TComparator comparator,
        int keyColumnCount,
        int tabletCount,
        double accuracy,
        i64 expectedTabletSize);

    void AddChunk(const NYT::NChunkClient::NProto::TChunkSpec& chunkSpec);
    void AddChunk(const NChunkClient::TWeightedInputChunkPtr& chunk);
    void AddSlice(const NChunkClient::TInputChunkSlicePtr& slice);

    void ComputeChunksForSlicing();
    void ComputeSlicedChunksPivotKeys();

    void SetFirstPivotKey(const TLegacyOwningKey& key);

    bool AreAllPivotsFound() const;
    std::vector<TLegacyOwningKey> GetPivotKeys() const;

    const THashMap<NChunkClient::TInputChunkPtr, i64>& GetChunksForSlicing() const;

private:
    using TBoundaryKeyIterator = std::vector<TSliceBoundaryKey>::iterator;

    struct TComputeState
    {
        i64 CurrentStartedChunksSize = 0;
        i64 CurrentFinishedChunksSize = 0;
        THashMap<NChunkClient::TInputChunkPtr, i64> ChunkForSlicingToSize;
        THashMap<NChunkClient::TInputChunkPtr, i64> CurrentChunkToSize;
    };

    struct TPivot
    {
        TLegacyOwningKey Key;
        std::optional<i64> TabletSize;
        std::optional<i64> BruteTabletSize;
    };

    const int KeyColumnCount_;
    const int TabletCount_;
    const double Accuracy_;
    std::vector<TPivot> Pivots_;
    std::function<bool(const TSliceBoundaryKey&, const TSliceBoundaryKey&)> SliceBoundaryKeyCompare_;

    i64 TotalSizeAfterSlicing_ = 0;
    std::vector<TSliceBoundaryKey> SliceBoundaryKeys_;
    std::vector<TSliceBoundaryKey> ChunkBoundaryKeys_;
    TComputeState State_;

    void UpdateCurrentChunksAndSizes(
        TBoundaryKeyIterator boundaryKey,
        TBoundaryKeyIterator boundaryEnd);

    i64 LowerPivotZone(i64 tabletIndex) const;
    i64 UpperPivotZone(i64 tabletIndex) const;

    bool IsPivotKeyZone(i64 size, i64 tabletIndex) const;
    bool IsLowerPivotZone(i64 size, i64 tabletIndex) const;
    bool IsUpperPivotZone(i64 size, i64 tabletIndex) const;

    bool IsBetterSize(i64 newSize, i64 currentSize) const;

    bool CanSplitHere(TBoundaryKeyIterator boundaryKey, i64 tabletIndex) const;
    bool IsKeyGreaterThanPreviousPivot(TBoundaryKeyIterator boundaryKey, i64 tabletIndex) const;

    TBoundaryKeyIterator AddChunksToSplit(TBoundaryKeyIterator begin, i64 previousTabletIndex);

    void AddFullChunks();
    void RecalculateExpectedTabletSize();
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
