#include "cluster_nodes.h"
#include "storage_system_clique.h"

#include <yt/client/misc/discovery.h>

#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataStreams/OneBlockInputStream.h>
#include <Storages/IStorage.h>

#include <algorithm>

namespace NYT::NClickHouseServer {

using namespace DB;

////////////////////////////////////////////////////////////////////////////////

class TStorageSystemClique
    : public DB::IStorage
{
private:
    const std::string TableName_;
    TDiscoveryPtr Discovery_;

public:
    TStorageSystemClique(
        TDiscoveryPtr discovery,
        std::string tableName)
        : TableName_(std::move(tableName))
        , Discovery_(std::move(discovery))
    {
        setColumns(CreateColumnList());
    }

    std::string getName() const override
    {
        return "SystemClique";
    }

    std::string getTableName() const override
    {
        return TableName_;
    }

    BlockInputStreams read(
        const Names& /* columnNames */,
        const SelectQueryInfo& /* queryInfo */,
        const Context& /* context */,
        QueryProcessingStage::Enum /* processedStage */,
        size_t /* maxBlockSize */,
        unsigned /* numStreams */)
    {
        auto nodes = Discovery_->List();

        MutableColumns res_columns = getSampleBlock().cloneEmptyColumns();

        for (const auto& [name, attributes] : nodes) {
            res_columns[0]->insert(std::string(attributes.at("host")->GetValue<TString>()));
            res_columns[1]->insert(attributes.at("rpc_port")->GetValue<ui64>());
            res_columns[2]->insert(attributes.at("monitoring_port")->GetValue<ui64>());
            res_columns[3]->insert(attributes.at("tcp_port")->GetValue<ui64>());
            res_columns[4]->insert(attributes.at("http_port")->GetValue<ui64>());
            res_columns[5]->insert(std::string(name));
            res_columns[6]->insert(attributes.at("pid")->GetValue<i64>());
        }

        return BlockInputStreams(1, std::make_shared<OneBlockInputStream>(getSampleBlock().cloneWithColumns(std::move(res_columns))));
    }

private:
    static ColumnsDescription CreateColumnList()
    {
        return ColumnsDescription({
            {
                "host",
                std::make_shared<DataTypeString>(),
            },
            {
                "rpc_port",
                std::make_shared<DataTypeUInt16>(),
            },
            {
                "monitoring_port",
                std::make_shared<DataTypeUInt16>(),
            },
            {
                "tcp_port",
                std::make_shared<DataTypeUInt16>(),
            },
            {
                "http_port",
                std::make_shared<DataTypeUInt16>(),
            },
            {
                "job_id",
                std::make_shared<DataTypeString>(),
            },
            {
                "pid",
                std::make_shared<DataTypeInt32>(),
            }
        });
    }
};

////////////////////////////////////////////////////////////////////////////////

DB::StoragePtr CreateStorageSystemClique(
    TDiscoveryPtr discovery,
    std::string tableName)
{
    return std::make_shared<TStorageSystemClique>(
        std::move(discovery),
        std::move(tableName));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
