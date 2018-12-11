#pragma once

#include <yt/core/misc/common.h>
#include <yt/core/logging/log.h>

#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#undef LOG_ERROR

#include <AggregateFunctions/registerAggregateFunctions.h>
#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnVector.h>
#include <Columns/IColumn.h>
#include <Common/config.h>
#include <Common/Exception.h>
#include <Common/getMultipleKeysFromConfig.h>
#include <Common/getNumberOfPhysicalCPUCores.h>
#include <Common/LRUCache.h>
#include <Common/OptimizedRegularExpression.h>
#include <Common/StringUtils/StringUtils.h>
#include <Common/typeid_cast.h>
#include <Core/Block.h>
#include <Core/Field.h>
#include <Core/Names.h>
#include <Core/NamesAndTypes.h>
#include <Core/SortDescription.h>
#include <Databases/DatabaseMemory.h>
#include <Databases/IDatabase.h>
#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/IProfilingBlockInputStream.h>
#include <DataStreams/materializeBlock.h>
#include <DataStreams/MaterializingBlockInputStream.h>
#include <DataStreams/OneBlockInputStream.h>
#include <DataStreams/RemoteBlockInputStream.h>
#include <DataTypes/DataTypeFactory.h>
#include <Dictionaries/DictionarySourceFactory.h>
#include <Dictionaries/Embedded/GeodataProviders/HierarchyFormatReader.h>
#include <Dictionaries/Embedded/GeodataProviders/IHierarchiesProvider.h>
#include <Dictionaries/Embedded/GeodataProviders/INamesProvider.h>
#include <Dictionaries/Embedded/GeodataProviders/NamesFormatReader.h>
#include <Dictionaries/Embedded/IGeoDictionariesLoader.h>
#include <Functions/registerFunctions.h>
#include <Interpreters/AsynchronousMetrics.h>
#include <Interpreters/Cluster.h>
#include <Interpreters/Context.h>
#include <Interpreters/evaluateConstantExpression.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/IExternalLoaderConfigRepository.h>
#include <Interpreters/InterpreterSelectQuery.h>
#include <Interpreters/IRuntimeComponentsFactory.h>
#include <Interpreters/ISecurityManager.h>
#include <Interpreters/ProcessList.h>
#include <Interpreters/Users.h>
#include <IO/HTTPCommon.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteHelpers.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/queryToString.h>
#include <Poco/AutoPtr.h>
#include <Poco/DirectoryIterator.h>
#include <Poco/Exception.h>
#include <Poco/Ext/LevelFilterChannel.h>
#include <Poco/File.h>
#include <Poco/Glob.h>
#include <Poco/Logger.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/IPAddress.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/TCPServer.h>
#include <Poco/Net/TCPServerConnectionFactory.h>
#include <Poco/String.h>
#include <Poco/ThreadPool.h>
#include <Poco/Timestamp.h>
#include <Poco/URI.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Poco/Util/LayeredConfiguration.h>
#include <Poco/Util/XMLConfiguration.h>
#include <Storages/IStorage.h>
#include <Storages/MergeTree/KeyCondition.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageMemory.h>
#include <Storages/StorageNull.h>
#include <Storages/System/attachSystemTables.h>
#include <Storages/System/StorageSystemAsynchronousMetrics.h>
#include <Storages/System/StorageSystemBuildOptions.h>
#include <Storages/System/StorageSystemClusters.h>
#include <Storages/System/StorageSystemColumns.h>
#include <Storages/System/StorageSystemDatabases.h>
#include <Storages/System/StorageSystemDictionaries.h>
#include <Storages/System/StorageSystemEvents.h>
#include <Storages/System/StorageSystemFunctions.h>
#include <Storages/System/StorageSystemGraphite.h>
#include <Storages/System/StorageSystemMerges.h>
#include <Storages/System/StorageSystemMetrics.h>
#include <Storages/System/StorageSystemNumbers.h>
#include <Storages/System/StorageSystemOne.h>
#include <Storages/System/StorageSystemParts.h>
#include <Storages/System/StorageSystemProcesses.h>
#include <Storages/System/StorageSystemReplicas.h>
#include <Storages/System/StorageSystemReplicationQueue.h>
#include <Storages/System/StorageSystemSettings.h>
#include <Storages/System/StorageSystemTables.h>
#include <Storages/System/StorageSystemZooKeeper.h>
#include <TableFunctions/ITableFunction.h>
#include <TableFunctions/registerTableFunctions.h>
#include <TableFunctions/TableFunctionFactory.h>

#include <server/IServer.h>
#include <server/IServer.h>
#include <common/DateLUT.h>
#include <server/HTTPHandler.h>
#include <server/NotFoundHandler.h>
#include <server/PingRequestHandler.h>
#include <server/RootRequestHandler.h>
#include <server/IServer.h>
#include <server/TCPHandler.h>

#include <common/logger_useful.h>

// ClickHouse uses the same macro as we do, so we perform a nasty hack to fix this issue.
#undef LOG_TRACE
#undef LOG_DEBUG
#undef LOG_INFO
#undef LOG_WARNING
#undef LOG_ERROR

#define CH_LOG_TRACE(logger, message) do { \
    if ((logger)->trace()) {\
    std::stringstream oss_internal_rare;    \
    oss_internal_rare << message; \
    (logger)->trace(oss_internal_rare.str());}} while(false)

#define CH_LOG_DEBUG(logger, message) do { \
    if ((logger)->debug()) {\
    std::stringstream oss_internal_rare;    \
    oss_internal_rare << message; \
    (logger)->debug(oss_internal_rare.str());}} while(false)

#define CH_LOG_INFO(logger, message) do { \
    if ((logger)->information()) {\
    std::stringstream oss_internal_rare;    \
    oss_internal_rare << message; \
    (logger)->information(oss_internal_rare.str());}} while(false)

#define CH_LOG_WARNING(logger, message) do { \
    if ((logger)->warning()) {\
    std::stringstream oss_internal_rare;    \
    oss_internal_rare << message; \
    (logger)->warning(oss_internal_rare.str());}} while(false)

#define CH_LOG_ERROR(logger, message) do { \
    if ((logger)->error()) {\
    std::stringstream oss_internal_rare;    \
    oss_internal_rare << message; \
    (logger)->error(oss_internal_rare.str());}} while(false)

#ifdef YT_ENABLE_TRACE_LOGGING
#define LOG_TRACE(...)                      LOG_EVENT(Logger, ::NYT::NLogging::ELogLevel::Trace, __VA_ARGS__)
#else
#define LOG_TRACE(...)                      LOG_UNUSED(__VA_ARGS__)
#endif
#define LOG_DEBUG(...)                      LOG_EVENT(Logger, ::NYT::NLogging::ELogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)                       LOG_EVENT(Logger, ::NYT::NLogging::ELogLevel::Info, __VA_ARGS__)
#define LOG_WARNING(...)                    LOG_EVENT(Logger, ::NYT::NLogging::ELogLevel::Warning, __VA_ARGS__)
#define LOG_ERROR(...)                      LOG_EVENT(Logger, ::NYT::NLogging::ELogLevel::Error, __VA_ARGS__)
