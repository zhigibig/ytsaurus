#include "query_executor.h"
#include "private.h"
#include "config.h"

#include <yt/server/cell_node/bootstrap.h>
#include <yt/server/cell_node/config.h>

#include <yt/server/data_node/chunk_block_manager.h>
#include <yt/server/data_node/chunk.h>
#include <yt/server/data_node/chunk_registry.h>
#include <yt/server/data_node/local_chunk_reader.h>
#include <yt/server/data_node/master_connector.h>

#include <yt/server/hydra/hydra_manager.h>

#include <yt/server/tablet_node/config.h>
#include <yt/server/tablet_node/security_manager.h>
#include <yt/server/tablet_node/slot_manager.h>
#include <yt/server/tablet_node/tablet.h>
#include <yt/server/tablet_node/tablet_manager.h>
#include <yt/server/tablet_node/tablet_reader.h>
#include <yt/server/tablet_node/tablet_slot.h>

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/connection.h>

#include <yt/ytlib/chunk_client/block_cache.h>
#include <yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/ytlib/chunk_client/chunk_spec.pb.h>
#include <yt/ytlib/chunk_client/replication_reader.h>

#include <yt/ytlib/node_tracker_client/node_directory.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/query_client/callbacks.h>
#include <yt/ytlib/query_client/column_evaluator.h>
#include <yt/ytlib/query_client/coordinator.h>
#include <yt/ytlib/query_client/evaluator.h>
#include <yt/ytlib/query_client/function_registry.h>
#include <yt/ytlib/query_client/helpers.h>
#include <yt/ytlib/query_client/plan_fragment.h>
#include <yt/ytlib/query_client/plan_helpers.h>
#include <yt/ytlib/query_client/private.h>
#include <yt/ytlib/query_client/query_statistics.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/config.h>
#include <yt/ytlib/table_client/pipe.h>
#include <yt/ytlib/table_client/schemaful_chunk_reader.h>
#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/schemaful_writer.h>
#include <yt/ytlib/table_client/unordered_schemaful_reader.h>

#include <yt/ytlib/tablet_client/public.h>

#include <yt/core/concurrency/scheduler.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/string.h>

namespace NYT {
namespace NQueryAgent {

using namespace NConcurrency;
using namespace NObjectClient;
using namespace NQueryClient;
using namespace NChunkClient;
using namespace NTabletClient;
using namespace NTableClient;
using namespace NTableClient::NProto;
using namespace NNodeTrackerClient;
using namespace NTabletNode;
using namespace NDataNode;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = QueryAgentLogger;

////////////////////////////////////////////////////////////////////////////////

namespace {

Stroka RowRangeFormatter(const NQueryClient::TRowRange& range)
{
    return Format("[%v .. %v]", range.first, range.second);
}

Stroka DataSourceFormatter(const NQueryClient::TDataRange& source)
{
    return Format("[%v .. %v]", source.Range.first, source.Range.second);
}

} // namespace NYT

////////////////////////////////////////////////////////////////////////////////

class TQueryExecutor
    : public ISubExecutor
{
public:
    explicit TQueryExecutor(
        TQueryAgentConfigPtr config,
        TBootstrap* bootstrap)
        : Config_(config)
        , Bootstrap_(bootstrap)
        , Evaluator_(New<TEvaluator>(Config_))
        , FunctionRegistry_(Bootstrap_->GetMasterClient()->GetFunctionRegistry())
        , ColumnEvaluatorCache_(Bootstrap_->GetMasterClient()->GetConnection()->GetColumnEvaluatorCache())
    { }

    // IExecutor implementation.
    virtual TFuture<TQueryStatistics> Execute(
        TConstQueryPtr query,
        std::vector<TDataRanges> dataSources,
        ISchemafulWriterPtr writer,
        TQueryOptions options) override
    {
        auto securityManager = Bootstrap_->GetSecurityManager();
        auto maybeUser = securityManager->GetAuthenticatedUser();

        auto execute = query->IsOrdered()
            ? &TQueryExecutor::DoExecuteOrdered
            : &TQueryExecutor::DoExecute;

        return BIND(execute, MakeStrong(this))
            .AsyncVia(Bootstrap_->GetQueryPoolInvoker())
            .Run(std::move(query), std::move(dataSources), std::move(options), std::move(writer), maybeUser);
    }

private:
    const TQueryAgentConfigPtr Config_;
    TBootstrap* const Bootstrap_;
    const TEvaluatorPtr Evaluator_;
    const IFunctionRegistryPtr FunctionRegistry_;
    const TColumnEvaluatorCachePtr ColumnEvaluatorCache_;

    typedef std::function<ISchemafulReaderPtr()> TSubreaderCreator;

    TQueryStatistics DoCoordinateAndExecute(
        TConstQueryPtr query,
        TQueryOptions options,
        ISchemafulWriterPtr writer,
        const std::vector<TRefiner>& refiners,
        const std::vector<TSubreaderCreator>& subreaderCreators)
    {
        auto Logger = BuildLogger(query);

        auto securityManager = Bootstrap_->GetSecurityManager();
        auto maybeUser = securityManager->GetAuthenticatedUser();

        NApi::TClientOptions clientOptions;
        if (maybeUser) {
            clientOptions.User = maybeUser.Get();
        }

        auto remoteExecutor = Bootstrap_->GetMasterClient()->GetConnection()
            ->CreateClient(clientOptions)->GetQueryExecutor();

        return CoordinateAndExecute(
            query,
            writer,
            refiners,
            [&] (TConstQueryPtr subquery, int index) {
                auto mergingReader = subreaderCreators[index]();

                auto pipe = New<TSchemafulPipe>();

                LOG_DEBUG("Evaluating subquery (SubqueryId: %v)", subquery->Id);

                auto foreignExecuteCallback = [options, remoteExecutor, Logger] (
                    const TQueryPtr& subquery,
                    TGuid dataId,
                    TRowBufferPtr buffer,
                    TRowRanges ranges,
                    ISchemafulWriterPtr writer) -> TFuture<TQueryStatistics>
                {
                    LOG_DEBUG("Evaluating remote subquery (SubqueryId: %v)", subquery->Id);

                    TQueryOptions subqueryOptions;
                    subqueryOptions.Timestamp = options.Timestamp;
                    subqueryOptions.VerboseLogging = options.VerboseLogging;

                    TDataRanges dataSource{
                        dataId,
                        MakeSharedRange(std::move(ranges), std::move(buffer))
                    };

                    return remoteExecutor->Execute(
                        subquery,
                        std::move(dataSource),
                        writer,
                        subqueryOptions);
                };

                auto asyncStatistics = BIND(&TEvaluator::RunWithExecutor, Evaluator_)
                    .AsyncVia(Bootstrap_->GetQueryPoolInvoker())
                    .Run(
                        subquery,
                        mergingReader,
                        pipe->GetWriter(),
                        foreignExecuteCallback,
                        FunctionRegistry_,
                        options.EnableCodeCache);

                asyncStatistics.Subscribe(BIND([=] (const TErrorOr<TQueryStatistics>& result) {
                    if (!result.IsOK()) {
                        pipe->Fail(result);
                        LOG_DEBUG(result, "Failed evaluating subquery (SubqueryId: %v)", subquery->Id);
                    }
                }));

                return std::make_pair(pipe->GetReader(), asyncStatistics);
            },
            [&] (TConstQueryPtr topQuery, ISchemafulReaderPtr reader, ISchemafulWriterPtr writer) {
                LOG_DEBUG("Evaluating top query (TopQueryId: %v)", topQuery->Id);
                auto result = Evaluator_->Run(
                    topQuery,
                    std::move(reader),
                    std::move(writer),
                    FunctionRegistry_,
                    options.EnableCodeCache);
                LOG_DEBUG("Finished evaluating top query (TopQueryId: %v)", topQuery->Id);
                return result;
            });
    }

    TQueryStatistics DoExecute(
        TConstQueryPtr query,
        std::vector<TDataRanges> dataSources,
        TQueryOptions options,
        ISchemafulWriterPtr writer,
        const TNullable<Stroka>& maybeUser)
    {
        auto securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager, maybeUser);

        auto Logger = BuildLogger(query);

        LOG_DEBUG("Classifying data sources into ranges and lookup keys");

        std::vector<TDataRanges> rangesByTablePart;
        std::vector<TDataKeys> keysByTablePart;

        auto keySize = query->KeyColumnsCount;

        for (const auto& source : dataSources) {
            TRowRanges rowRanges;
            std::vector<TRow> keys;

            for (const auto& range : source.Ranges) {
                auto lowerBound = range.first;
                auto upperBound = range.second;

                if (keySize == lowerBound.GetCount() &&
                    keySize + 1 == upperBound.GetCount() &&
                    upperBound[keySize].Type == EValueType::Max &&
                    CompareRows(lowerBound.Begin(), lowerBound.End(), upperBound.Begin(), upperBound.Begin() + keySize) == 0)
                {
                    keys.push_back(lowerBound);
                } else {
                    rowRanges.push_back(range);
                }
            }

            if (!rowRanges.empty()) {
                rangesByTablePart.push_back({
                    source.Id,
                    MakeSharedRange(std::move(rowRanges), source.Ranges.GetHolder())});
            }
            if (!keys.empty()) {
                keysByTablePart.push_back({
                    source.Id,
                    MakeSharedRange(std::move(keys), source.Ranges.GetHolder())});
            }
        }

        LOG_DEBUG("Splitting sources");

        auto rowBuffer = New<TRowBuffer>();
        auto splits = Split(std::move(rangesByTablePart), rowBuffer, Logger, options.VerboseLogging);
        int splitCount = splits.size();
        int splitOffset = 0;
        std::vector<TSharedRange<TDataRange>> groupedSplits;

        LOG_DEBUG("Grouping %v splits", splitCount);

        auto maxSubqueries = std::min(options.MaxSubqueries, Config_->MaxSubqueries);

        for (int queryIndex = 1; queryIndex <= maxSubqueries; ++queryIndex) {
            int nextSplitOffset = queryIndex * splitCount / maxSubqueries;
            if (splitOffset != nextSplitOffset) {
                std::vector<TDataRange> subsplit(splits.begin() + splitOffset, splits.begin() + nextSplitOffset);
                groupedSplits.emplace_back(MakeSharedRange(std::move(subsplit), rowBuffer));
                splitOffset = nextSplitOffset;
            }
        }

        LOG_DEBUG("Got %v split groups", groupedSplits.size());

        auto columnEvaluator = ColumnEvaluatorCache_->Find(
            query->TableSchema,
            query->KeyColumnsCount);

        auto timestamp = options.Timestamp;
        const auto& workloadDescriptor = options.WorkloadDescriptor;

        std::vector<TRefiner> refiners;
        std::vector<TSubreaderCreator> subreaderCreators;

        for (auto& groupedSplit : groupedSplits) {
            refiners.push_back([&, range = GetRange(groupedSplit)] (
                TConstExpressionPtr expr,
                const TTableSchema& schema,
                const TKeyColumns& keyColumns)
            {
                return RefinePredicate(range, expr, schema, keyColumns, columnEvaluator);
            });
            subreaderCreators.push_back([&, MOVE(groupedSplit)] () {
                if (options.VerboseLogging) {
                    LOG_DEBUG("Generating reader for ranges %v",
                        JoinToString(groupedSplit, DataSourceFormatter));
                } else {
                    LOG_DEBUG("Generating reader for %v ranges", groupedSplit.Size());
                }

                auto bottomSplitReaderGenerator = [
                    Logger,
                    query,
                    MOVE(groupedSplit),
                    timestamp,
                    workloadDescriptor,
                    index = 0,
                    this_ = MakeStrong(this)
                ] () mutable -> ISchemafulReaderPtr {
                    if (index == groupedSplit.Size()) {
                        return nullptr;
                    }

                    const auto& group = groupedSplit[index++];
                    return this_->GetReader(
                        query->TableSchema,
                        group.Id,
                        group.Range,
                        timestamp,
                        workloadDescriptor);
                };

                return CreateUnorderedSchemafulReader(
                    std::move(bottomSplitReaderGenerator),
                    Config_->MaxBottomReaderConcurrency);
            });
        }

        for (auto& keySource : keysByTablePart) {
            const auto& tablePartId = keySource.Id;
            auto& keys = keySource.Keys;

            refiners.push_back([&] (TConstExpressionPtr expr, const TTableSchema& schema, const TKeyColumns& keyColumns) {
                return RefinePredicate(keys, expr, keyColumns);
            });
            subreaderCreators.push_back([&, MOVE(keys)] () {

                // TODO(lukyan): Validate timestamp and read permission
                ValidateReadTimestamp(timestamp);

                std::function<ISchemafulReaderPtr()> bottomSplitReaderGenerator;

                switch (TypeFromId(tablePartId)) {
                    case EObjectType::Chunk:
                    case EObjectType::ErasureChunk: {
                        return GetChunkReader(
                            query->TableSchema,
                            tablePartId,
                            keys,
                            timestamp);
                    }

                    case EObjectType::Tablet: {
                        LOG_DEBUG("Grouping %v lookup keys by parition", keys.Size());
                        auto groupedKeys = GroupKeysByPartition(tablePartId, std::move(keys));
                        LOG_DEBUG("Grouped lookup keys into %v paritions", groupedKeys.size());

                        for (const auto& keys : groupedKeys) {
                            if (options.VerboseLogging) {
                                LOG_DEBUG("Generating lookup reader for keys %v",
                                    JoinToString(keys.second));
                            } else {
                                LOG_DEBUG("Generating lookup reader for %v keys",
                                    keys.second.Size());
                            }
                        }

                        auto slotManager = Bootstrap_->GetTabletSlotManager();
                        auto tabletSnapshot = slotManager->GetTabletSnapshotOrThrow(tablePartId);

                        bottomSplitReaderGenerator = [
                            Logger,
                            MOVE(tabletSnapshot),
                            query,
                            MOVE(groupedKeys),
                            tablePartId,
                            timestamp,
                            workloadDescriptor,
                            index = 0,
                            this_ = MakeStrong(this)
                        ] () mutable -> ISchemafulReaderPtr {
                            if (index == groupedKeys.size()) {
                                return nullptr;
                            } else {
                                const auto& group = groupedKeys[index++];

                                // TODO(lukyan): Validate timestamp and read permission
                                return CreateSchemafulTabletReader(
                                    tabletSnapshot,
                                    query->TableSchema,
                                    group.first,
                                    group.second,
                                    timestamp,
                                    workloadDescriptor);
                            }
                        };

                        return CreateUnorderedSchemafulReader(
                            std::move(bottomSplitReaderGenerator),
                            Config_->MaxBottomReaderConcurrency);
                    }

                    default:
                        THROW_ERROR_EXCEPTION("Unsupported data split type %Qlv",
                            TypeFromId(tablePartId));
                }
            });
        }

        return DoCoordinateAndExecute(
            query,
            options,
            std::move(writer),
            refiners,
            subreaderCreators);
    }

    TQueryStatistics DoExecuteOrdered(
        TConstQueryPtr query,
        std::vector<TDataRanges> dataSources,
        TQueryOptions options,
        ISchemafulWriterPtr writer,
        const TNullable<Stroka>& maybeUser)
    {
        auto securityManager = Bootstrap_->GetSecurityManager();
        TAuthenticatedUserGuard userGuard(securityManager, maybeUser);

        auto Logger = BuildLogger(query);

        auto rowBuffer = New<TRowBuffer>();
        auto splits = Split(dataSources, rowBuffer, Logger, options.VerboseLogging);

        LOG_DEBUG("Sorting %v splits", splits.size());

        std::sort(splits.begin(), splits.end(), [] (const TDataRange & lhs, const TDataRange & rhs) {
            return lhs.Range.first < rhs.Range.first;
        });

        if (options.VerboseLogging) {
            LOG_DEBUG("Got ranges for groups %v",
                JoinToString(splits, DataSourceFormatter));
        } else {
            LOG_DEBUG("Got ranges for %v groups", splits.size());
        }

        auto columnEvaluator = ColumnEvaluatorCache_->Find(
            query->TableSchema,
            query->KeyColumnsCount);

        auto timestamp = options.Timestamp;
        const auto& workloadDescriptor = options.WorkloadDescriptor;

        std::vector<TRefiner> refiners;
        std::vector<TSubreaderCreator> subreaderCreators;

        for (const auto& dataSplit : splits) {
            refiners.push_back([&] (TConstExpressionPtr expr, const TTableSchema& schema, const TKeyColumns& keyColumns) {
                return RefinePredicate(dataSplit.Range, expr, schema, keyColumns, columnEvaluator);
            });
            subreaderCreators.push_back([&] () {
                return GetReader(query->TableSchema, dataSplit.Id, dataSplit.Range, timestamp, workloadDescriptor);
            });
        }

        return DoCoordinateAndExecute(
            query,
            options,
            std::move(writer),
            refiners,
            subreaderCreators);
    }

    // TODO(lukyan): Use mutable shared range
    std::vector<TDataRange> Split(
        std::vector<TDataRanges> rangesByTablePart,
        TRowBufferPtr rowBuffer,
        const NLogging::TLogger& Logger,
        bool verboseLogging)
    {
        std::vector<TDataRange> allSplits;

        for (auto& tablePartIdRange : rangesByTablePart) {
            auto tablePartId = tablePartIdRange.Id;
            auto& keyRanges = tablePartIdRange.Ranges;

            if (TypeFromId(tablePartId) != EObjectType::Tablet) {
                for (const auto& range : keyRanges) {
                    allSplits.push_back(TDataRange{tablePartId, TRowRange(
                        rowBuffer->Capture(range.first),
                        rowBuffer->Capture(range.second)
                    )});
                }
                continue;
            }

            YCHECK(!keyRanges.Empty());

            YCHECK(std::is_sorted(
                keyRanges.Begin(),
                keyRanges.End(),
                [] (const TRowRange& lhs, const TRowRange& rhs) {
                    return lhs.first < rhs.first;
                }));

            auto slotManager = Bootstrap_->GetTabletSlotManager();
            auto tabletSnapshot = slotManager->GetTabletSnapshotOrThrow(tablePartId);

            std::vector<TRowRange> resultRanges;
            int lastIndex = 0;

            auto addRange = [&] (int count, TUnversionedRow lowerBound, TUnversionedRow upperBound) {
                LOG_DEBUG_IF(verboseLogging, "Merging %v ranges into [%v .. %v]",
                    count,
                    lowerBound,
                    upperBound);
                resultRanges.emplace_back(lowerBound, upperBound);
            };

            for (int index = 1; index < keyRanges.Size(); ++index) {
                auto lowerBound = keyRanges[index].first;
                auto upperBound = keyRanges[index - 1].second;

                int totalSampleCount, partitionCount;
                std::tie(totalSampleCount, partitionCount) = GetBoundSampleKeys(tabletSnapshot, upperBound, lowerBound);
                YCHECK(partitionCount > 0);

                if (totalSampleCount != 0 || partitionCount != 1) {
                    addRange(index - lastIndex, keyRanges[lastIndex].first, upperBound);
                    lastIndex = index;
                }
            }

            addRange(
                keyRanges.Size() - lastIndex,
                keyRanges[lastIndex].first,
                keyRanges[keyRanges.Size() - 1].second);

            int totalSampleCount = 0;
            int totalPartitionCount = 0;
            for (const auto& range : resultRanges) {
                int sampleCount, partitionCount;
                std::tie(sampleCount, partitionCount) = GetBoundSampleKeys(tabletSnapshot, range.first, range.second);
                totalSampleCount += sampleCount;
                totalPartitionCount += partitionCount;
            }

            int freeSlotCount = std::max(0, Config_->MaxSubsplitsPerTablet - totalPartitionCount);
            int cappedSampleCount = std::min(freeSlotCount, totalSampleCount);

            int nextSampleIndex = 1;
            int currentSampleCount = 1;
            for (const auto& range : resultRanges) {
                auto splitKeys = BuildSplitKeys(
                    tabletSnapshot,
                    range.first,
                    range.second,
                    nextSampleIndex,
                    currentSampleCount,
                    totalSampleCount,
                    cappedSampleCount);

                for (int splitKeyIndex = 0; splitKeyIndex < splitKeys.size(); ++splitKeyIndex) {
                    const auto& thisKey = splitKeys[splitKeyIndex];
                    const auto& nextKey = (splitKeyIndex == splitKeys.size() - 1)
                        ? MaxKey()
                        : splitKeys[splitKeyIndex + 1];
                    allSplits.push_back({tablePartId, TRowRange(
                        rowBuffer->Capture(std::max(range.first, thisKey.Get())),
                        rowBuffer->Capture(std::min(range.second, nextKey.Get()))
                    )});
                }
            }
        }

        return allSplits;
    }

    std::vector<std::pair<TPartitionSnapshotPtr, TSharedRange<TRow>>> GroupKeysByPartition(
        const NObjectClient::TObjectId& objectId,
        TSharedRange<TRow> keys)
    {
        std::vector<std::pair<TPartitionSnapshotPtr, TSharedRange<TRow>>> result;
        // TODO(lukyan): YCHECK(sorted)

        YCHECK(TypeFromId(objectId) == EObjectType::Tablet);

        auto slotManager = Bootstrap_->GetTabletSlotManager();
        auto tabletSnapshot = slotManager->GetTabletSnapshotOrThrow(objectId);

        const auto& partitions = tabletSnapshot->Partitions;

        auto currentPartitionsIt = partitions.begin();
        auto endPartitionsIt = partitions.end();
        auto currentKeysIt = begin(keys);
        auto endKeysIt = end(keys);

        while (currentKeysIt != endKeysIt) {
            auto nextPartitionsIt = std::upper_bound(
                currentPartitionsIt,
                endPartitionsIt,
                *currentKeysIt,
                [] (TRow lhs, const TPartitionSnapshotPtr& rhs) {
                    return lhs < rhs->PivotKey.Get();
                });

            auto nextKeysIt = nextPartitionsIt != endPartitionsIt
                ? std::lower_bound(currentKeysIt, endKeysIt, (*nextPartitionsIt)->PivotKey.Get())
                : endKeysIt;

            // TODO(babenko): fixme, data ownership?
            TPartitionSnapshotPtr ptr = *(nextPartitionsIt - 1);
            result.emplace_back(
                ptr,
                MakeSharedRange(MakeRange<TRow>(currentKeysIt, nextKeysIt), keys.GetHolder()));

            currentKeysIt = nextKeysIt;
            currentPartitionsIt = nextPartitionsIt;
        }

        return result;
    }

    std::pair<int, int> GetBoundSampleKeys(
        TTabletSnapshotPtr tabletSnapshot,
        const TRow& lowerBound,
        const TRow& upperBound)
    {
        YCHECK(lowerBound <= upperBound);

        auto findStartSample = [&] (const std::vector<TOwningKey>& sampleKeys) {
            return std::upper_bound(
                sampleKeys.begin(),
                sampleKeys.end(),
                lowerBound);
        };
        auto findEndSample = [&] (const std::vector<TOwningKey>& sampleKeys) {
            return std::lower_bound(
                sampleKeys.begin(),
                sampleKeys.end(),
                upperBound);
        };

        // Run binary search to find the relevant partitions.
        const auto& partitions = tabletSnapshot->Partitions;
        YCHECK(!partitions.empty());
        YCHECK(lowerBound >= partitions[0]->PivotKey);
        auto startPartitionIt = std::upper_bound(
            partitions.begin(),
            partitions.end(),
            lowerBound,
            [] (const TRow& lhs, const TPartitionSnapshotPtr& rhs) {
                return lhs < rhs->PivotKey.Get();
            }) - 1;
        auto endPartitionIt = std::lower_bound(
            startPartitionIt,
            partitions.end(),
            upperBound,
            [] (const TPartitionSnapshotPtr& lhs, const TRow& rhs) {
                return lhs->PivotKey.Get() < rhs;
            });
        int partitionCount = std::distance(startPartitionIt, endPartitionIt);

        int totalSampleCount = 0;
        for (auto partitionIt = startPartitionIt; partitionIt != endPartitionIt; ++partitionIt) {
            const auto& partition = *partitionIt;
            const auto& sampleKeys = partition->SampleKeys->Keys;
            auto startSampleIt = partitionIt == startPartitionIt && !sampleKeys.empty()
                ? findStartSample(sampleKeys)
                : sampleKeys.begin();
            auto endSampleIt = partitionIt + 1 == endPartitionIt
                ? findEndSample(sampleKeys)
                : sampleKeys.end();

            totalSampleCount += std::distance(startSampleIt, endSampleIt);
        }

        return std::make_pair(totalSampleCount, partitionCount);
    }

    std::vector<TOwningKey> BuildSplitKeys(
        TTabletSnapshotPtr tabletSnapshot,
        const TRow& lowerBound,
        const TRow& upperBound,
        int& nextSampleIndex,
        int& currentSampleCount,
        int totalSampleCount,
        int cappedSampleCount)
    {
        auto findStartSample = [&] (const std::vector<TOwningKey>& sampleKeys) {
            return std::upper_bound(
                sampleKeys.begin(),
                sampleKeys.end(),
                lowerBound);
        };
        auto findEndSample = [&] (const std::vector<TOwningKey>& sampleKeys) {
            return std::lower_bound(
                sampleKeys.begin(),
                sampleKeys.end(),
                upperBound);
        };

        // Run binary search to find the relevant partitions.
        const auto& partitions = tabletSnapshot->Partitions;
        YCHECK(lowerBound >= partitions[0]->PivotKey);
        auto startPartitionIt = std::upper_bound(
            partitions.begin(),
            partitions.end(),
            lowerBound,
            [] (const TRow& lhs, const TPartitionSnapshotPtr& rhs) {
                return lhs < rhs->PivotKey.Get();
            }) - 1;
        auto endPartitionIt = std::lower_bound(
            startPartitionIt,
            partitions.end(),
            upperBound,
            [] (const TPartitionSnapshotPtr& lhs, const TRow& rhs) {
                return lhs->PivotKey.Get() < rhs;
            });
        int partitionCount = std::distance(startPartitionIt, endPartitionIt);

        int nextSampleCount = cappedSampleCount != 0
            ? nextSampleIndex * totalSampleCount / cappedSampleCount
            : 0;

        // Fill results with pivotKeys and up to cappedSampleCount sampleKeys.
        std::vector<TOwningKey> result;
        result.reserve(partitionCount + cappedSampleCount);
        for (auto partitionIt = startPartitionIt; partitionIt != endPartitionIt; ++partitionIt) {
            const auto& partition = *partitionIt;
            const auto& sampleKeys = partition->SampleKeys->Keys;
            auto startSampleIt = partitionIt == startPartitionIt && !sampleKeys.empty()
                ? findStartSample(sampleKeys)
                : sampleKeys.begin();
            auto endSampleIt = partitionIt == endPartitionIt - 1
                ? findEndSample(sampleKeys)
                : sampleKeys.end();

            result.push_back(partition->PivotKey);

            if (cappedSampleCount == 0) {
                continue;
            }

            for (auto sampleIt = startSampleIt; sampleIt < endSampleIt;) {
                if (currentSampleCount == nextSampleCount) {
                    ++nextSampleIndex;
                    nextSampleCount = nextSampleIndex * totalSampleCount / cappedSampleCount;
                    result.push_back(*sampleIt);
                }
                int samplesLeft = static_cast<int>(std::distance(sampleIt, endSampleIt));
                int step = std::min(samplesLeft, nextSampleCount - currentSampleCount);
                YCHECK(step > 0);
                sampleIt += step;
                currentSampleCount += step;
            }
        }
        return result;
    }

    ISchemafulReaderPtr GetReader(
        const TTableSchema& schema,
        const NObjectClient::TObjectId& objectId,
        const TRowRange& range,
        TTimestamp timestamp,
        const TWorkloadDescriptor& workloadDescriptor)
    {
        ValidateReadTimestamp(timestamp);

        switch (TypeFromId(objectId)) {
            case EObjectType::Chunk:
            case EObjectType::ErasureChunk:
                return GetChunkReader(schema, objectId, range, timestamp);

            case EObjectType::Tablet:
                return GetTabletReader(schema, objectId, range, timestamp, workloadDescriptor);

            default:
                THROW_ERROR_EXCEPTION("Unsupported data split type %Qlv",
                    TypeFromId(objectId));
        }
    }

    ISchemafulReaderPtr GetReader(
        const TTableSchema& schema,
        const NObjectClient::TObjectId& objectId,
        const TSharedRange<TRow>& keys,
        TTimestamp timestamp,
        const TWorkloadDescriptor& workloadDescriptor)
    {
        ValidateReadTimestamp(timestamp);

        switch (TypeFromId(objectId)) {
            case EObjectType::Chunk:
            case EObjectType::ErasureChunk:
                return GetChunkReader(schema,  objectId, keys, timestamp);

            case EObjectType::Tablet:
                return GetTabletReader(schema, objectId, keys, timestamp, workloadDescriptor);

            default:
                THROW_ERROR_EXCEPTION("Unsupported data split type %Qlv",
                    TypeFromId(objectId));
        }
    }

    ISchemafulReaderPtr GetChunkReader(
        const TTableSchema& schema,
        const NObjectClient::TObjectId& chunkId,
        const TRowRange& range,
        TTimestamp timestamp)
    {
        // TODO(sandello): Use options for passing workload descriptor.
        std::vector<TReadRange> readRanges;
        TReadLimit lowerReadLimit;
        TReadLimit upperReadLimit;
        lowerReadLimit.SetKey(TOwningKey(range.first));
        upperReadLimit.SetKey(TOwningKey(range.second));
        readRanges.emplace_back(std::move(lowerReadLimit), std::move(upperReadLimit));
        return GetChunkReader(schema, chunkId, std::move(readRanges), timestamp);
    }

    ISchemafulReaderPtr GetChunkReader(
        const TTableSchema& schema,
        const TChunkId& chunkId,
        const TSharedRange<TRow>& keys,
        TTimestamp timestamp)
    {
        // TODO(sandello): Use options for passing workload descriptor.

        std::vector<TReadRange> readRanges;
        TUnversionedOwningRowBuilder builder;
        for (const auto& key : keys) {
            TReadLimit lowerReadLimit;
            lowerReadLimit.SetKey(TOwningKey(key));

            TReadLimit upperReadLimit;
            for (int index = 0; index < key.GetCount(); ++index) {
                builder.AddValue(key[index]);
            }
            builder.AddValue(MakeUnversionedSentinelValue(EValueType::Max));
            upperReadLimit.SetKey(builder.FinishRow());

            readRanges.emplace_back(std::move(lowerReadLimit), std::move(upperReadLimit));
        }

        return GetChunkReader(schema, chunkId, readRanges, timestamp);
    }

    ISchemafulReaderPtr GetChunkReader(
        const TTableSchema& schema,
        const TChunkId& chunkId,
        std::vector<TReadRange> readRanges,
        TTimestamp timestamp)
    {
        auto blockCache = Bootstrap_->GetBlockCache();
        auto chunkRegistry = Bootstrap_->GetChunkRegistry();
        auto chunk = chunkRegistry->FindChunk(chunkId);

        // TODO(sandello): Use options for passing workload descriptor.

        NChunkClient::IChunkReaderPtr chunkReader;
        if (chunk && !chunk->IsRemoveScheduled()) {
            LOG_DEBUG("Creating local reader for chunk split (ChunkId: %v, Timestamp: %v)",
                chunkId,
                timestamp);

            chunkReader = CreateLocalChunkReader(
                Bootstrap_,
                Bootstrap_->GetConfig()->TabletNode->ChunkReader,
                chunk,
                blockCache);
        } else {
            LOG_DEBUG("Creating remote reader for chunk split (ChunkId: %v, Timestamp: %v)",
                chunkId,
                timestamp);

            // TODO(babenko): seed replicas?
            // TODO(babenko): throttler?
            auto options = New<TRemoteReaderOptions>();
            chunkReader = CreateReplicationReader(
                Bootstrap_->GetConfig()->TabletNode->ChunkReader,
                options,
                Bootstrap_->GetMasterClient(),
                New<TNodeDirectory>(),
                Bootstrap_->GetMasterConnector()->GetLocalDescriptor(),
                chunkId,
                TChunkReplicaList(),
                Bootstrap_->GetBlockCache());
        }

        auto chunkMeta = WaitFor(chunkReader->GetMeta()).ValueOrThrow();

        return WaitFor(CreateSchemafulChunkReader(
            Bootstrap_->GetConfig()->TabletNode->ChunkReader,
            std::move(chunkReader),
            Bootstrap_->GetBlockCache(),
            schema,
            chunkMeta,
            std::move(readRanges),
            timestamp))
            .ValueOrThrow();
    }

    ISchemafulReaderPtr GetTabletReader(
        const TTableSchema& schema,
        const NObjectClient::TObjectId& tabletId,
        const TRowRange& range,
        TTimestamp timestamp,
        const TWorkloadDescriptor& workloadDescriptor)
    {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        auto tabletSnapshot = slotManager->GetTabletSnapshotOrThrow(tabletId);

        auto securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(tabletSnapshot, NYTree::EPermission::Read);

        TOwningKey lowerBound(range.first);
        TOwningKey upperBound(range.second);

        return CreateSchemafulTabletReader(
            std::move(tabletSnapshot),
            schema,
            lowerBound,
            upperBound,
            timestamp,
            workloadDescriptor);
    }

    ISchemafulReaderPtr GetTabletReader(
        const TTableSchema& schema,
        const TTabletId& tabletId,
        const TSharedRange<TRow>& keys,
        TTimestamp timestamp,
        const TWorkloadDescriptor& workloadDescriptor)
    {
        auto slotManager = Bootstrap_->GetTabletSlotManager();
        auto tabletSnapshot = slotManager->GetTabletSnapshotOrThrow(tabletId);

        auto securityManager = Bootstrap_->GetSecurityManager();
        securityManager->ValidatePermission(tabletSnapshot, NYTree::EPermission::Read);

        return CreateSchemafulTabletReader(
            std::move(tabletSnapshot),
            schema,
            keys,
            timestamp,
            workloadDescriptor);
    }
};

ISubExecutorPtr CreateQueryExecutor(
    TQueryAgentConfigPtr config,
    TBootstrap* bootstrap)
{
    return New<TQueryExecutor>(config, bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryAgent
} // namespace NYT

