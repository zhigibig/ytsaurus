#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/ytlib/chunk_client/data_statistics.pb.h>

#include <yt/core/yson/public.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

struct TChunkTreeStatistics
{
    //! Total number of rows in the tree.
    i64 RowCount = 0;

    //! Number of addressable rows in the tree. Typically equals to #RowCount but can be
    //! larger if some initial prefix of the rowset was trimmed.
    i64 LogicalRowCount = 0;

    //! Sum of uncompressed data sizes of chunks in the tree.
    i64 UncompressedDataSize = 0;

    //! Sum of compressed data sizes of chunks in the tree.
    i64 CompressedDataSize = 0;

    //! Sum of data weights of chunks in the tree.
    i64 DataWeight = 0;

    //! Disk space occupied on data nodes by regular chunks (without replication).
    i64 RegularDiskSpace = 0;

    //! Disk space occupied on data nodes by erasure chunks (including parity parts).
    i64 ErasureDiskSpace = 0;

    //! Total number of chunks in the tree.
    int ChunkCount = 0;

    //! Number of addressable chunks in the tree. Typically equals to #ChunkCount but can be
    //! larger if some initial prefix of the rowset was trimmed.
    i64 LogicalChunkCount = 0;

    //! Total number of chunk lists in the tree.
    int ChunkListCount = 0;

    //! Chunks have zero ranks.
    //! Chunk lists have rank |1 + maxChildRank|, where |maxChildRank = 0| if there are no children.
    int Rank = 0;

    //! |false| indicates that there is an unsealed journal chunk at the end.
    bool Sealed = true;

    void Accumulate(const TChunkTreeStatistics& other);
    void Deaccumulate(const TChunkTreeStatistics& other);

    NChunkClient::NProto::TDataStatistics ToDataStatistics() const;

    void Save(NCellMaster::TSaveContext& context) const;
    void Load(NCellMaster::TLoadContext& context);

    bool operator == (const TChunkTreeStatistics& other) const;
    bool operator != (const TChunkTreeStatistics& other) const;
};

Stroka ToString(const TChunkTreeStatistics& statistics);

void Serialize(const TChunkTreeStatistics& statistics, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
