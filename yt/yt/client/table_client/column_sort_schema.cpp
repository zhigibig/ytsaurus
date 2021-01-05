#include "column_sort_schema.h"

#include <yt/client/table_client/proto/chunk_meta.pb.h>

#include <yt/core/ytree/fluent.h>

namespace NYT::NTableClient {

using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TColumnSortSchema& schema, IYsonConsumer* consumer)
{
    // COMPAT(gritukan): Serializing columns with ESortOrder::Ascending as map node
    // will end up with a disaster during 21.1 -> 20.3 CA rollback. Remove this code
    // when 21.1 will be stable.
    if (schema.SortOrder == ESortOrder::Ascending) {
        consumer->OnStringScalar(schema.Name);
    } else {
        BuildYsonFluently(consumer).BeginMap()
            .Item("name").Value(schema.Name)
            .Item("sort_order").Value(schema.SortOrder)
        .EndMap();
    }
}

void Deserialize(TColumnSortSchema& schema, INodePtr node)
{
    if (node->GetType() == ENodeType::Map) {
        auto mapNode = node->AsMap();
        Deserialize(schema.Name, mapNode->GetChildOrThrow("name"));
        Deserialize(schema.SortOrder, mapNode->GetChildOrThrow("sort_order"));
    } else if (node->GetType() == ENodeType::String) {
        Deserialize(schema.Name, node);
        schema.SortOrder = ESortOrder::Ascending;
    } else {
        THROW_ERROR_EXCEPTION("Unexpected type of column sort schema node; expected \"string\" or \"map\", %Qv found",
            node->GetType());
    }
}

bool operator == (const TColumnSortSchema& lhs, const TColumnSortSchema& rhs)
{
    return lhs.Name == rhs.Name && lhs.SortOrder == rhs.SortOrder;
}

bool operator != (const TColumnSortSchema& lhs, const TColumnSortSchema& rhs)
{
    return !(lhs == rhs);
}

////////////////////////////////////////////////////////////////////////////////

void ValidateSortColumns(const std::vector<TColumnSortSchema>& columns)
{
    ValidateKeyColumnCount(columns.size());

    THashSet<TString> names;
    for (const auto& column : columns) {
        if (!names.insert(column.Name).second) {
            THROW_ERROR_EXCEPTION("Duplicate sort column name %Qv",
                column.Name);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(
    NProto::TSortColumnsExt* protoSortColumns,
    const TSortColumns& sortColumns)
{
    for (const auto& sortColumn : sortColumns) {
        protoSortColumns->add_names(sortColumn.Name);
        protoSortColumns->add_sort_orders(static_cast<int>(sortColumn.SortOrder));
    }
}

void FromProto(
    TSortColumns* sortColumns,
    const NProto::TSortColumnsExt& protoSortColumns)
{
    YT_VERIFY(protoSortColumns.names_size() == protoSortColumns.sort_orders_size());
    for (int columnIndex = 0; columnIndex < protoSortColumns.names_size(); ++columnIndex) {
        TColumnSortSchema sortColumn{
            .Name = protoSortColumns.names(columnIndex),
            .SortOrder = CheckedEnumCast<ESortOrder>(protoSortColumns.sort_orders(columnIndex))
        };
        sortColumns->push_back(sortColumn);
    }
}

////////////////////////////////////////////////////////////////////////////////

TKeyColumns GetColumnNames(const TSortColumns& sortColumns)
{
    TKeyColumns keyColumns;
    keyColumns.reserve(sortColumns.size());
    for (const auto& sortColumn : sortColumns) {
        keyColumns.push_back(sortColumn.Name);
    }

    return keyColumns;
}

TComparator GetComparator(const TSortColumns& sortColumns)
{
    std::vector<ESortOrder> sortOrders;
    sortOrders.reserve(sortColumns.size());
    for (const auto& sortColumn : sortColumns) {
        sortOrders.push_back(sortColumn.SortOrder);
    }

    return TComparator(sortOrders);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
