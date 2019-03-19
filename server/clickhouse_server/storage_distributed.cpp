#include "storage_distributed.h"

#include "format_helpers.h"
#include "type_helpers.h"
#include "helpers.h"
#include "query_context.h"

#include <Common/Exception.h>
#include <DataStreams/materializeBlock.h>
#include <DataStreams/MaterializingBlockInputStream.h>
#include <DataStreams/RemoteBlockInputStream.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Parsers/queryToString.h>
#include <Storages/MergeTree/KeyCondition.h>

namespace NYT::NClickHouseServer {

using namespace DB;

////////////////////////////////////////////////////////////////////////////////

BlockInputStreams TStorageDistributed::read(
    const Names& columnNames,
    const SelectQueryInfo& queryInfo,
    const Context& context,
    QueryProcessingStage::Enum processedStage,
    size_t /* maxBlockSize */,
    unsigned /* numStreams */)
{
    auto* queryContext = GetQueryContext(context);
    const auto& Logger = queryContext->Logger;

    auto clusterNodes = Cluster->GetAvailableNodes();
    auto allocation = AllocateTablePartsToClusterNodes(clusterNodes, queryInfo, context);

    YT_LOG_INFO("Preparing query to YT table storage (ColumnNames: %v, TableName: %v, NodeCount: %v)",
        columnNames,
        getTableName(),
        clusterNodes.size());

    // Prepare settings and context for subqueries.

    const auto& settings = context.getSettingsRef();

    processedStage = settings.distributed_group_by_no_merge
        ? QueryProcessingStage::Complete
        : QueryProcessingStage::WithMergeableState;

    Context newContext(context);
    newContext.setSettings(PrepareLeafJobSettings(settings));

    auto throttler = CreateNetThrottler(settings);

    BlockInputStreams streams;

    for (const auto& partAllocation : allocation) {
        const auto& tablePart = partAllocation.TablePart;
        const auto& clusterNode = partAllocation.TargetClusterNode;

        auto subQueryAst = RewriteSelectQueryForTablePart(
            queryInfo.query,
            ToStdString(tablePart.JobSpec));

        bool isLocal = clusterNode->IsLocal();
        // XXX(max42): weird workaround.
        isLocal = false;
        auto tablePartStream = isLocal
            ? CreateLocalStream(
                subQueryAst,
                newContext,
                processedStage)
            : CreateRemoteStream(
                partAllocation.TargetClusterNode,
                subQueryAst,
                newContext,
                throttler,
                context.getExternalTables(),
                processedStage);

        streams.push_back(std::move(tablePartStream));
    }

    YT_LOG_INFO("Finished query preparation");

    return streams;
}

QueryProcessingStage::Enum TStorageDistributed::getQueryProcessingStage(const Context& context) const
{
    const auto& settings = context.getSettingsRef();

    // Set processing stage

    return settings.distributed_group_by_no_merge
                     ? QueryProcessingStage::Complete
                     : QueryProcessingStage::WithMergeableState;
}

TTableAllocation TStorageDistributed::AllocateTablePartsToClusterNodes(
    const TClusterNodes& clusterNodes,
    const SelectQueryInfo& queryInfo,
    const Context& context)
{
    size_t clusterNodeCount = clusterNodes.size();

    std::unique_ptr<KeyCondition> keyCondition;
    if (Schema.HasPrimaryKey()) {
        keyCondition = std::make_unique<KeyCondition>(CreateKeyCondition(context, queryInfo, Schema));
    }

    auto tableParts = GetTableParts(
        queryInfo.query,
        context,
        keyCondition.get(),
        clusterNodeCount);

    if (tableParts.empty()) {
        // nothing to read
        return {};
    }

    if (tableParts.size() > clusterNodes.size()) {
        throw Exception("Cluster is too small", ErrorCodes::LOGICAL_ERROR);
    }

    TTableAllocation allocation;
    allocation.reserve(tableParts.size());
    for (size_t i = 0; i < tableParts.size(); ++i) {
        allocation.emplace_back(tableParts[i], clusterNodes[i]);
    }
    return allocation;
}

Settings TStorageDistributed::PrepareLeafJobSettings(const Settings& settings)
{
    Settings newSettings = settings;

    newSettings.queue_max_wait_ms = Cluster::saturate(
        newSettings.queue_max_wait_ms,
        settings.max_execution_time);

    // Does not matter on remote servers, because queries are sent under different user.
    newSettings.max_concurrent_queries_for_user = 0;
    newSettings.max_memory_usage_for_user = 0;

    // This setting is really not for user and should not be sent to remote server.
    newSettings.max_memory_usage_for_all_queries = 0;

    // Set as unchanged to avoid sending to remote server.
    newSettings.max_concurrent_queries_for_user.changed = false;
    newSettings.max_memory_usage_for_user.changed = false;
    newSettings.max_memory_usage_for_all_queries.changed = false;

    newSettings.max_query_size = 0;

    return newSettings;
}

ThrottlerPtr TStorageDistributed::CreateNetThrottler(
    const Settings& settings)
{
    ThrottlerPtr throttler;
    if (settings.max_network_bandwidth || settings.max_network_bytes) {
        throttler = std::make_shared<Throttler>(
            settings.max_network_bandwidth,
            settings.max_network_bytes,
            "Limit for bytes to send or receive over network exceeded.");
    }
    return throttler;
}

BlockInputStreamPtr TStorageDistributed::CreateLocalStream(
    const ASTPtr& queryAst,
    const Context& context,
    QueryProcessingStage::Enum processedStage)
{
    InterpreterSelectQuery interpreter(queryAst, context, Names{}, processedStage);
    BlockInputStreamPtr stream = interpreter.execute().in;

    // Materialization is needed, since from remote servers the constants come materialized.
    // If you do not do this, different types (Const and non-Const) columns will be produced in different threads,
    // And this is not allowed, since all code is based on the assumption that in the block stream all types are the same.
    return std::make_shared<MaterializingBlockInputStream>(stream);
}

BlockInputStreamPtr TStorageDistributed::CreateRemoteStream(
    const IClusterNodePtr remoteNode,
    const ASTPtr& queryAst,
    const Context& context,
    const ThrottlerPtr& throttler,
    const Tables& externalTables,
    QueryProcessingStage::Enum processedStage)
{
    std::string query = queryToString(queryAst);

    Block header = materializeBlock(InterpreterSelectQuery(queryAst, context, Names{}, processedStage).getSampleBlock());

    auto stream = std::make_shared<RemoteBlockInputStream>(
        remoteNode->GetConnection(),
        query,
        header,
        context,
        nullptr,    // will use settings from context
        throttler,
        externalTables,
        processedStage);

    stream->setPoolMode(PoolMode::GET_MANY);

    return stream;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
