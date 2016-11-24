#include "chunk.h"
#include "chunk_list.h"
#include "chunk_tree_statistics.h"

#include <yt/server/cell_master/serialize.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/core/erasure/codec.h>

namespace NYT {
namespace NChunkServer {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NObjectServer;
using namespace NObjectClient;
using namespace NSecurityServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

const TChunk::TCachedReplicas TChunk::EmptyCachedReplicas;
const TChunk::TStoredReplicas TChunk::EmptyStoredReplicas;

////////////////////////////////////////////////////////////////////////////////

TChunk::TChunk(const TChunkId& id)
    : TChunkTree(id)
{
    LocalProperties_.SetVital(true);
    for (auto& mediumProps : LocalProperties_) {
        mediumProps.Clear();
    }

    ChunkMeta_.set_type(static_cast<int>(EChunkType::Unknown));
    ChunkMeta_.set_version(-1);
    ChunkMeta_.mutable_extensions();
}

TChunkTreeStatistics TChunk::GetStatistics() const
{
    TChunkTreeStatistics result;
    if (IsSealed()) {
        result.RowCount = MiscExt_.row_count();
        result.LogicalRowCount = MiscExt_.row_count();
        result.UncompressedDataSize = MiscExt_.uncompressed_data_size();
        result.CompressedDataSize = MiscExt_.compressed_data_size();
        result.DataWeight = MiscExt_.data_weight();
        if (IsErasure()) {
            result.ErasureDiskSpace = ChunkInfo_.disk_space();
        } else {
            result.RegularDiskSpace = ChunkInfo_.disk_space();
        }
        result.ChunkCount = 1;
        result.LogicalChunkCount = 1;
        result.Rank = 0;
        result.Sealed = IsSealed();
    } else {
        result.Sealed = false;
    }
    return result;
}

TClusterResources TChunk::GetResourceUsage() const
{
    TClusterResources result(0, 1);
    if (!IsConfirmed()) {
        return result;
    }

    for (int i = 0; i < MaxMediumCount; ++i) {
        // NB: Use just the local RF as this only makes sense for staged chunks.
        i64 diskSpace = ChunkInfo_.disk_space() * GetLocalReplicationFactor(i);
        result.DiskSpace[i] = diskSpace;
    }

    return result;
}

void TChunk::Save(NCellMaster::TSaveContext& context) const
{
    TChunkTree::Save(context);

    using NYT::Save;
    Save(context, ChunkInfo_);
    Save(context, ChunkMeta_);
    Save(context, LocalProperties_);
    Save(context, ReadQuorum_);
    Save(context, WriteQuorum_);
    Save(context, GetErasureCodec());
    Save(context, GetMovable());
    Save(context, Parents_);
    // NB: RemoveReplica calls do not commute and their order is not
    // deterministic (i.e. when unregistering a node we traverse certain hashtables).
    TNullableVectorSerializer<TDefaultSerializer, TSortedTag>::Save(context, StoredReplicas_);
    Save(context, CachedReplicas_);
    Save(context, ExportCounter_);
    if (ExportCounter_ > 0) {
        TRangeSerializer::Save(context, TRef::FromPod(ExportDataList_));
    }
}

// Compatibility stuff; used by Load().
struct TOldChunkExportData
{
    ui32 RefCounter : 24;
    bool Vital : 1;
    ui8 ReplicationFactor : 7;
};
static_assert(sizeof(TOldChunkExportData) == 4, "sizeof(TOldChunkExportData) != 4");
using TOldChunkExportDataList = TOldChunkExportData[NObjectClient::MaxSecondaryMasterCells];

void TChunk::Load(NCellMaster::TLoadContext& context)
{
    TChunkTree::Load(context);

    using NYT::Load;
    Load(context, ChunkInfo_);
    Load(context, ChunkMeta_);
    // COMPAT(shakurov)
    if (context.GetVersion() < 400) {
        LocalProperties_[DefaultStoreMediumIndex]
            .SetReplicationFactorOrThrow(Load<i8>(context)); // Never actually throws.
    } else {
        Load(context, LocalProperties_);
    }
    SetReadQuorum(Load<i8>(context));
    SetWriteQuorum(Load<i8>(context));
    SetErasureCodec(Load<NErasure::ECodec>(context));
    SetMovable(Load<bool>(context));
    // COMPAT(shakurov)
    if (context.GetVersion() < 400) {
        SetLocalVital(Load<bool>(context));
    } // Local vital flag is now part of LocalProperties_.
    Load(context, Parents_);
    Load(context, StoredReplicas_);
    Load(context, CachedReplicas_);
    Load(context, ExportCounter_);
    if (ExportCounter_ > 0) {
        // COMPAT(shakurov)
        if (context.GetVersion() < 400) {
            TOldChunkExportDataList oldExportDataList = {};
            TRangeSerializer::Load(context, TMutableRef::FromPod(oldExportDataList));
            for (int i = 0; i < NObjectClient::MaxSecondaryMasterCells; ++i) {
                auto& exportData = ExportDataList_[i];
                auto& properties = exportData.Properties;
                auto& oldExportData = oldExportDataList[i];
                exportData.RefCounter = oldExportData.RefCounter;
                properties[DefaultStoreMediumIndex].SetReplicationFactorOrThrow(oldExportData.ReplicationFactor);
                properties.SetVital(oldExportData.Vital);
            }
        } else {
            TRangeSerializer::Load(context, TMutableRef::FromPod(ExportDataList_));
        }
    }

    if (IsConfirmed()) {
        MiscExt_ = GetProtoExtension<TMiscExt>(ChunkMeta_.extensions());
    }
}

void TChunk::AddParent(TChunkList* parent)
{
    Parents_.push_back(parent);
}

void TChunk::RemoveParent(TChunkList* parent)
{
    auto it = std::find(Parents_.begin(), Parents_.end(), parent);
    Y_ASSERT(it != Parents_.end());
    Parents_.erase(it);
}

const TChunk::TCachedReplicas& TChunk::CachedReplicas() const
{
    return CachedReplicas_ ? *CachedReplicas_ : EmptyCachedReplicas;
}

const TChunk::TStoredReplicas& TChunk::StoredReplicas() const
{
    return StoredReplicas_ ? *StoredReplicas_ : EmptyStoredReplicas;
}

void TChunk::AddReplica(TNodePtrWithIndexes replica, bool cached)
{
    if (cached) {
        Y_ASSERT(!IsJournal());
        if (!CachedReplicas_) {
            CachedReplicas_ = std::make_unique<TCachedReplicas>();
        }
        YCHECK(CachedReplicas_->insert(replica).second);
    } else {
        if (!StoredReplicas_) {
            StoredReplicas_ = std::make_unique<TStoredReplicas>();
        }
        if (IsJournal()) {
            for (auto& existingReplica : *StoredReplicas_) {
                if (existingReplica.GetPtr() == replica.GetPtr()) {
                    existingReplica = replica;
                    return;
                }
            }
        }
        StoredReplicas_->push_back(replica);
    }
}

void TChunk::RemoveReplica(TNodePtrWithIndexes replica, bool cached)
{
    if (cached) {
        Y_ASSERT(CachedReplicas_);
        YCHECK(CachedReplicas_->erase(replica) == 1);
        if (CachedReplicas_->empty()) {
            CachedReplicas_.reset();
        }
    } else {
        // NB: We don't release StoredReplicas_ when it becomes empty since
        // the idea is just to save up some space for foreign chunks.
        for (auto it = StoredReplicas_->begin(); it != StoredReplicas_->end(); ++it) {
            auto& existingReplica = *it;
            if (existingReplica == replica ||
                IsJournal() && existingReplica.GetPtr() == replica.GetPtr())
            {
                std::swap(existingReplica, StoredReplicas_->back());
                StoredReplicas_->pop_back();
                return;
            }
        }
        Y_UNREACHABLE();
    }
}

TNodePtrWithIndexesList TChunk::GetReplicas() const
{
    const auto& storedReplicas = StoredReplicas();
    const auto& cachedReplicas = CachedReplicas();
    TNodePtrWithIndexesList result;
    result.reserve(storedReplicas.size() + cachedReplicas.size());
    result.insert(result.end(), storedReplicas.begin(), storedReplicas.end());
    result.insert(result.end(), cachedReplicas.begin(), cachedReplicas.end());
    return result;
}

void TChunk::ApproveReplica(TNodePtrWithIndexes replica)
{
    if (IsJournal()) {
        YCHECK(StoredReplicas_);
        for (auto& existingReplica : *StoredReplicas_) {
            if (existingReplica.GetPtr() == replica.GetPtr()) {
                existingReplica = replica;
                return;
            }
        }
        Y_UNREACHABLE();
    }
}

void TChunk::Confirm(
    TChunkInfo* chunkInfo,
    TChunkMeta* chunkMeta)
{
    // YT-3251
    if (!HasProtoExtension<TMiscExt>(chunkMeta->extensions())) {
        THROW_ERROR_EXCEPTION("Missing TMiscExt in chunk meta");
    }

    ChunkInfo_.Swap(chunkInfo);
    ChunkMeta_.Swap(chunkMeta);
    MiscExt_ = GetProtoExtension<TMiscExt>(ChunkMeta_.extensions());

    Y_ASSERT(IsConfirmed());
}

bool TChunk::IsConfirmed() const
{
    return EChunkType(ChunkMeta_.type()) != EChunkType::Unknown;
}

bool TChunk::IsAvailable() const
{
    if (!StoredReplicas_) {
        // Actually it makes no sense calling IsAvailable for foreign chunks.
        return false;
    }

    switch (GetType()) {
        case EObjectType::Chunk:
            return !StoredReplicas_->empty();

        case EObjectType::ErasureChunk: {
            auto* codec = NErasure::GetCodec(GetErasureCodec());
            int dataPartCount = codec->GetDataPartCount();
            NErasure::TPartIndexSet missingIndexSet((1 << dataPartCount) - 1);
            for (auto replica : *StoredReplicas_) {
                missingIndexSet.reset(replica.GetReplicaIndex());
            }
            return missingIndexSet.none();
        }

        case EObjectType::JournalChunk:
            if (StoredReplicas_->size() >= GetReadQuorum()) {
                return true;
            }
            for (auto replica : *StoredReplicas_) {
                if (replica.GetReplicaIndex() == SealedChunkReplicaIndex) {
                    return true;
                }
            }
            return false;

        default:
            Y_UNREACHABLE();
    }
}

bool TChunk::IsSealed() const
{
    if (!IsConfirmed()) {
        return false;
    }

    if (!IsJournal()) {
        return true;
    }

    return MiscExt_.sealed();
}

i64 TChunk::GetSealedRowCount() const
{
    YCHECK(MiscExt_.sealed());
    return MiscExt_.row_count();
}

void TChunk::Seal(const TMiscExt& info)
{
    YCHECK(IsConfirmed() && !IsSealed());

    // NB: Just a sanity check.
    YCHECK(!MiscExt_.sealed());
    YCHECK(MiscExt_.row_count() == 0);
    YCHECK(MiscExt_.uncompressed_data_size() == 0);
    YCHECK(MiscExt_.compressed_data_size() == 0);
    YCHECK(ChunkInfo_.disk_space() == 0);

    MiscExt_.set_sealed(true);
    MiscExt_.set_row_count(info.row_count());
    MiscExt_.set_uncompressed_data_size(info.uncompressed_data_size());
    MiscExt_.set_compressed_data_size(info.compressed_data_size());
    SetProtoExtension(ChunkMeta_.mutable_extensions(), MiscExt_);
    ChunkInfo_.set_disk_space(info.uncompressed_data_size());  // an approximation
}

const TChunkProperties& TChunk::GetLocalProperties() const
{
    return LocalProperties_;
}

TMediumChunkProperties TChunk::GetLocalProperties(int mediumIndex) const
{
    return LocalProperties_[mediumIndex];
}

bool TChunk::UpdateLocalProperties(const TChunkProperties& properties)
{
    if (LocalProperties_ != properties) {
        LocalProperties_ = properties;
        return true;
    }

    return false;
}

bool TChunk::UpdateExternalProperties(
    int cellIndex,
    const TChunkProperties& properties)
{
    auto& data = ExportDataList_[cellIndex];
    auto& curProperties = data.Properties;

    if (curProperties != properties) {
        curProperties = properties;
        return true;
    }

    return false;
}

int TChunk::ComputeReplicationFactor(int mediumIndex) const
{
    // NB: Shortcut for non-exported chunk.
    if (ExportCounter_ == 0) {
        return GetLocalReplicationFactor(mediumIndex);
    }

    auto replicationFactor = GetLocalReplicationFactor(mediumIndex);
    for (const auto& data : ExportDataList_) {
        replicationFactor = std::max<int>(
            replicationFactor,
            data.Properties[mediumIndex].GetReplicationFactor());
    }

    return replicationFactor;
}

int TChunk::GetMaxReplicasPerRack(int mediumIndex, TNullable<int> replicationFactorOverride) const
{
    switch (GetType()) {
        case EObjectType::Chunk: {
            int replicationFactor = replicationFactorOverride
            	? *replicationFactorOverride
            	: ComputeReplicationFactor(mediumIndex);
            return std::max(replicationFactor - 1, 1);
        }

        case EObjectType::ErasureChunk:
            return NErasure::GetCodec(GetErasureCodec())->GetGuaranteedRepairablePartCount();

        case EObjectType::JournalChunk: {
            int minQuorum = std::min(ReadQuorum_, WriteQuorum_);
            return std::max(minQuorum - 1, 1);
        }

        default:
            Y_UNREACHABLE();
    }
}

const TChunkExportData& TChunk::GetExportData(int cellIndex) const
{
    return ExportDataList_[cellIndex];
}

void TChunk::Export(int cellIndex)
{
    auto& data = ExportDataList_[cellIndex];
    if (++data.RefCounter == 1) {
        ++ExportCounter_;
    }
}

void TChunk::Unexport(int cellIndex, int importRefCounter)
{
    auto& data = ExportDataList_[cellIndex];
    if ((data.RefCounter -= importRefCounter) == 0) {
        // NB: Reset the entry to the neutral state as ComputeReplicationFactor and
        // ComputeVital always scan the whole array.
        data = {};
        --ExportCounter_;
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT

Y_DECLARE_PODTYPE(NYT::NChunkServer::TOldChunkExportDataList);
