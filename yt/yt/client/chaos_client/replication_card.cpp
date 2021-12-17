#include "replication_card.h"

#include <yt/yt/core/misc/format.h>
#include <yt/yt/core/misc/guid.h>
#include <yt/yt/core/misc/serialize.h>

#include <util/digest/multi.h>

#include <algorithm>

namespace NYT::NChaosClient {

using namespace NTransactionClient;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

bool IsStableReplicaMode(EReplicaMode mode)
{
    return mode == EReplicaMode::Sync || mode == EReplicaMode::Async;
}

bool IsStableReplicaState(EReplicaState state)
{
    return state == EReplicaState::Enabled || state == EReplicaState::Disabled;
}

////////////////////////////////////////////////////////////////////////////////

TReplicationCardToken::TReplicationCardToken(
    NObjectClient::TCellId chaosCellId,
    TReplicationCardId replicationCardId)
    : ChaosCellId(chaosCellId)
    , ReplicationCardId(replicationCardId)
{ }

TReplicationCardToken::operator size_t() const
{
    return MultiHash(
        ChaosCellId,
        ReplicationCardId);
}

TReplicationCardToken::operator bool() const
{
    return static_cast<bool>(ReplicationCardId);
}

bool TReplicationCardToken::operator == (const TReplicationCardToken& other) const
{
    return ChaosCellId == other.ChaosCellId && ReplicationCardId == other.ReplicationCardId;
}

////////////////////////////////////////////////////////////////////////////////

void FormatValue(TStringBuilderBase* builder, const TReplicationProgress& replicationProgress, TStringBuf /*spec*/)
{
    builder->AppendFormat("{Segments: %v, UpperKey: %v}",
        MakeFormattableView(replicationProgress.Segments, [] (auto* builder, const auto& segment) {
            builder->AppendFormat("<%v, %llx>", segment.LowerKey, segment.Timestamp);
        }),
        replicationProgress.UpperKey);
}

TString ToString(const TReplicationProgress& replicationProgress)
{
    return ToStringViaBuilder(replicationProgress);
}

void FormatValue(TStringBuilderBase* builder, const TReplicaHistoryItem& replicaHistoryItem, TStringBuf /*spec*/)
{
    builder->AppendFormat("{Era: %v, Timestamp: %llx, Mode: %v, State: %v}",
        replicaHistoryItem.Era,
        replicaHistoryItem.Timestamp,
        replicaHistoryItem.Mode,
        replicaHistoryItem.State);
}

TString ToString(const TReplicaHistoryItem& replicaHistoryItem)
{
    return ToStringViaBuilder(replicaHistoryItem);
}

void FormatValue(TStringBuilderBase* builder, const TReplicaInfo& replicaInfo, TStringBuf /*spec*/)
{
    builder->AppendFormat("{ReplicaId: %v, Cluster: %v, Path: %v, ContentType: %v, Mode: %v, State: %v, Progress: %v, History: %v}",
        replicaInfo.ReplicaId,
        replicaInfo.Cluster,
        replicaInfo.TablePath,
        replicaInfo.ContentType,
        replicaInfo.Mode,
        replicaInfo.State,
        replicaInfo.ReplicationProgress,
        replicaInfo.History);
}

TString ToString(const TReplicaInfo& replicaInfo)
{
    return ToStringViaBuilder(replicaInfo);
}

void FormatValue(TStringBuilderBase* builder, const TReplicationCard& replicationCard, TStringBuf /*spec*/)
{
    builder->AppendFormat("{Era: %v, Replicas: %v, CoordinatorCellIds: %v}",
        replicationCard.Era,
        replicationCard.Replicas,
        replicationCard.CoordinatorCellIds);
}

TString ToString(const TReplicationCard& replicationCard)
{
    return ToStringViaBuilder(replicationCard);
}

void FormatValue(TStringBuilderBase* builder, const TReplicationCardToken& replicationCardToken, TStringBuf /*spec*/)
{
    builder->AppendFormat("{ChaosCellId: %v, ReplicationCardId: %v}",
        replicationCardToken.ChaosCellId,
        replicationCardToken.ReplicationCardId);
}

TString ToString(const TReplicationCardToken& replicationCardToken)
{
    return ToStringViaBuilder(replicationCardToken);
}

////////////////////////////////////////////////////////////////////////////////

void TReplicationProgress::TSegment::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, LowerKey);
    Persist(context, Timestamp);
}

void TReplicationProgress::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Segments);
    Persist(context, UpperKey);
}

void TReplicaHistoryItem::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Era);
    Persist(context, Timestamp);
    Persist(context, Mode);
    Persist(context, State);
}

void TReplicaInfo::Persist(const TStreamPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Cluster);
    Persist(context, TablePath);
    Persist(context, ContentType);
    Persist(context, Mode);
    Persist(context, State);
    Persist(context, History);
    Persist(context, ReplicationProgress);
}

////////////////////////////////////////////////////////////////////////////////

int TReplicaInfo::FindHistoryItemIndex(TTimestamp timestamp)
{
    auto it = std::upper_bound(
        History.begin(),
        History.end(),
        timestamp,
        [] (const TTimestamp& lhs, const TReplicaHistoryItem& rhs) {
            return lhs < rhs.Timestamp;
        });
    return std::distance(History.begin(), it) - 1;
}

TReplicaInfo* TReplicationCard::FindReplica(TReplicaId replicaId)
{
    for (auto& replica : Replicas) {
        if (replica.ReplicaId == replicaId) {
            return &replica;
        }
    }
    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////

bool IsReplicaReallySync(EReplicaMode mode, EReplicaState state)
{
    return mode == EReplicaMode::Sync && state == EReplicaState::Enabled;
}

void UpdateReplicationProgress(TReplicationProgress* progress, const TReplicationProgress& update)
{
    std::vector<TReplicationProgress::TSegment> segments;
    auto progressIt = progress->Segments.begin();
    auto progressEnd = progress->Segments.end();
    auto updateIt = update.Segments.begin();
    auto updateEnd = update.Segments.end();
    auto progressTimestamp = NullTimestamp;
    auto updateTimestamp = NullTimestamp;

    auto append = [&] (TUnversionedOwningRow key) {
        auto timestamp = std::max(progressTimestamp, updateTimestamp);
        if (segments.empty() || segments.back().Timestamp != timestamp) {
            segments.push_back({std::move(key), timestamp});
        }
    };

    bool upper = false;
    auto processUpperKey = [&] (const TUnversionedOwningRow& key) {
        if (upper || updateIt != updateEnd) {
            return;
        }

        auto cmpResult = CompareRows(key, update.UpperKey);
        if (cmpResult >= 0) {
            updateTimestamp = NullTimestamp;
            upper = true;
        }
        if (cmpResult > 0) {
            append(update.UpperKey);
        }
    };

    while (progressIt < progressEnd || updateIt < updateEnd) {
        int cmpResult;
        if (updateIt == updateEnd) {
            cmpResult = -1;
        } else if (progressIt == progressEnd) {
            cmpResult = 1;
        } else {
            cmpResult = CompareRows(progressIt->LowerKey, updateIt->LowerKey);
        }

        if (cmpResult < 0) {
            processUpperKey(progressIt->LowerKey);
            progressTimestamp = progressIt->Timestamp;
            append(std::move(progressIt->LowerKey));
            ++progressIt;
        } else if (cmpResult > 0) {
            updateTimestamp = updateIt->Timestamp;
            append(updateIt->LowerKey);
            ++updateIt;
        } else {
            updateTimestamp = updateIt->Timestamp;
            progressTimestamp = progressIt->Timestamp;
            append(std::move(progressIt->LowerKey));
            ++progressIt;
            ++updateIt;
        }
    }

    progress->Segments = std::move(segments);
}

bool IsReplicationProgressGreaterOrEqual(const TReplicationProgress& progress, const TReplicationProgress& other)
{
    auto progressIt = progress.Segments.begin();
    auto otherIt = std::upper_bound(
        other.Segments.begin(),
        other.Segments.end(),
        progressIt->LowerKey,
        [] (const auto& lhs, const auto& rhs) {
            return CompareRows(lhs, rhs.LowerKey) < 0;
        });
    YT_VERIFY(otherIt != other.Segments.begin());
    --otherIt;

    auto progressEnd = progress.Segments.end();
    auto otherEnd = other.Segments.end();
    auto progressTimestamp = MaxTimestamp;
    auto otherTimestamp = otherIt->Timestamp;

    while (progressIt < progressEnd && otherIt < otherEnd) {
        int cmpResult = CompareRows(progressIt->LowerKey, otherIt->LowerKey);
        if (cmpResult < 0) {
            progressTimestamp = progressIt->Timestamp;
            ++progressIt;
        } else if (cmpResult > 0) {
            otherTimestamp = otherIt->Timestamp;
            ++otherIt;
        } else {
            progressTimestamp = progressIt->Timestamp;
            otherTimestamp = otherIt->Timestamp;
            ++progressIt;
            ++otherIt;
        }

        if (progressTimestamp < otherTimestamp) {
            return false;
        }
    }

    while (otherIt < otherEnd) {
        int cmpResult = CompareRows(progress.UpperKey, otherIt->LowerKey);
        if (cmpResult > 0) {
            otherTimestamp = otherIt->Timestamp;
            ++otherIt;
        } else {
            break;
        }

        if (progressTimestamp < otherTimestamp) {
            return false;
        }
    }

    return true;
}

bool IsReplicationProgressGreaterOrEqual(const TReplicationProgress& progress, TTimestamp timestamp)
{
    for (const auto& segment : progress.Segments) {
        if (segment.Timestamp < timestamp) {
            return false;
        }
    }
    return true;
}

TReplicationProgress AdvanceReplicationProgress(const TReplicationProgress& progress, TTimestamp timestamp)
{
    TReplicationProgress result;
    result.UpperKey = progress.UpperKey;

    for (const auto& segment : progress.Segments) {
        if (segment.Timestamp > timestamp) {
            result.Segments.push_back(segment);
        } else if (result.Segments.empty() || result.Segments.back().Timestamp > timestamp) {
            result.Segments.push_back({segment.LowerKey, timestamp});
        }
    }

    return result;
}

TReplicationProgress LimitReplicationProgressByTimestamp(const TReplicationProgress& progress, TTimestamp timestamp)
{
    TReplicationProgress result;
    result.UpperKey = progress.UpperKey;

    for (const auto& segment : progress.Segments) {
        if (segment.Timestamp < timestamp) {
            result.Segments.push_back(segment);
        } else if (result.Segments.empty() || result.Segments.back().Timestamp < timestamp) {
            result.Segments.push_back({segment.LowerKey, timestamp});
        }
    }

    return result;
}

NTransactionClient::TTimestamp GetReplicationProgressMinTimestamp(const TReplicationProgress& progress)
{
    auto minTimestamp = MaxTimestamp;
    for (const auto& segment : progress.Segments) {
        minTimestamp = std::min(segment.Timestamp, minTimestamp);
    }
    return minTimestamp;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChaosClient
