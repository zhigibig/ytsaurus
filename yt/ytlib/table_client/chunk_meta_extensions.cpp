#include "chunk_meta_extensions.h"

#include <yt/ytlib/chunk_client/chunk_spec.h>

namespace NYT {
namespace NTableClient {

using namespace NProto;

using NChunkClient::NProto::TChunkMeta;
using NChunkClient::EChunkType;

////////////////////////////////////////////////////////////////////////////////

size_t TBoundaryKeys::SpaceUsed() const
{
    return
       sizeof(*this) +
       MinKey.GetSpaceUsed() - sizeof(MinKey) +
       MaxKey.GetSpaceUsed() - sizeof(MaxKey);
}

void TBoundaryKeys::Persist(const NPhoenix::TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, MinKey);
    Persist(context, MaxKey);
}

Stroka ToString(const TBoundaryKeys& keys)
{
    return Format("MinKey: %v, MaxKey: %v", keys.MinKey, keys.MaxKey);
}

////////////////////////////////////////////////////////////////////////////////

bool TryGetBoundaryKeys(const TChunkMeta& chunkMeta, TOwningKey* minKey, TOwningKey* maxKey)
{
    if (chunkMeta.version() == static_cast<int>(ETableChunkFormat::Old)) {
        auto boundaryKeys = FindProtoExtension<TOldBoundaryKeysExt>(chunkMeta.extensions());
        if (!boundaryKeys) {
            return false;
        }
        FromProto(minKey, boundaryKeys->start());
        FromProto(maxKey, boundaryKeys->end());
    } else {
        auto boundaryKeys = FindProtoExtension<TBoundaryKeysExt>(chunkMeta.extensions());
        if (!boundaryKeys) {
            return false;
        }
        FromProto(minKey, boundaryKeys->min());
        FromProto(maxKey, boundaryKeys->max());
    }
    return true;
}

std::unique_ptr<TBoundaryKeys> GetBoundaryKeys(const TChunkMeta& chunkMeta)
{
    std::unique_ptr<TBoundaryKeys> keys(new TBoundaryKeys());
    if (!TryGetBoundaryKeys(chunkMeta, &keys->MinKey, &keys->MaxKey)) {
        keys.reset();
    }
    return keys;
}

TChunkMeta FilterChunkMetaByPartitionTag(const TChunkMeta& chunkMeta, int partitionTag)
{
    YCHECK(chunkMeta.type() == static_cast<int>(EChunkType::Table));
    auto filteredChunkMeta = chunkMeta;

    if (chunkMeta.version() == static_cast<int>(ETableChunkFormat::Old)) {
        auto channelsExt = GetProtoExtension<TChannelsExt>(chunkMeta.extensions());
        // Partition chunks must have only one channel.
        YCHECK(channelsExt.items_size() == 1);

        std::vector<TBlockInfo> filteredBlocks;
        for (const auto& blockInfo : channelsExt.items(0).blocks()) {
            YCHECK(blockInfo.partition_tag() != DefaultPartitionTag);
            if (blockInfo.partition_tag() == partitionTag) {
                filteredBlocks.push_back(blockInfo);
            }
        }

        NYT::ToProto(channelsExt.mutable_items(0)->mutable_blocks(), filteredBlocks);
        SetProtoExtension(filteredChunkMeta.mutable_extensions(), channelsExt);
    } else {
        // New chunks.
        auto blockMetaExt = GetProtoExtension<TBlockMetaExt>(chunkMeta.extensions());

        std::vector<TBlockMeta> filteredBlocks;
        for (const auto& blockMeta : blockMetaExt.blocks()) {
            YCHECK(blockMeta.partition_index() != DefaultPartitionTag);
            if (blockMeta.partition_index() == partitionTag) {
                filteredBlocks.push_back(blockMeta);
            }
        }

        NYT::ToProto(blockMetaExt.mutable_blocks(), filteredBlocks);
        SetProtoExtension(filteredChunkMeta.mutable_extensions(), blockMetaExt);
    }

    return filteredChunkMeta;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
