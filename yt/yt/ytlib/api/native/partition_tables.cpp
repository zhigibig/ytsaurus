#include "partition_tables.h"

#include <yt/yt/ytlib/chunk_client/combine_data_slices.h>
#include <yt/yt/ytlib/chunk_client/data_slice_descriptor.h>
#include <yt/yt/ytlib/chunk_client/helpers.h>
#include <yt/yt/ytlib/chunk_client/input_chunk.h>
#include <yt/yt/ytlib/chunk_client/input_chunk_slice.h>
#include <yt/yt/ytlib/chunk_client/legacy_data_slice.h>

#include <yt/yt/ytlib/chunk_pools/chunk_pool.h>
#include <yt/yt/ytlib/chunk_pools/chunk_pool_factory.h>
#include <yt/yt/ytlib/chunk_pools/chunk_stripe.h>

#include <yt/yt/ytlib/table_client/helpers.h>

#include <yt/yt/client/node_tracker_client/node_directory.h>

#include <yt/yt/client/object_client/public.h>

#include <yt/yt/client/table_client/row_buffer.h>

#include <yt/yt/core/ytree/permission.h>

#include <library/cpp/iterator/enumerate.h>

namespace NYT::NApi::NNative {

using namespace NChunkClient;
using namespace NChunkPools;
using namespace NObjectClient;
using namespace NTableClient;
using namespace NYPath;

////////////////////////////////////////////////////////////////////////////////

TMultiTablePartitioner::TMultiTablePartitioner(
    const IClientPtr& client,
    const std::vector<TRichYPath>& paths,
    const TPartitionTablesOptions& options,
    const NLogging::TLogger& logger)
    : Client_(client)
    , Paths_(paths)
    , Options_(options)
    , Logger(logger)
{ }

TMultiTablePartitions TMultiTablePartitioner::DoPartitionTables()
{
    YT_LOG_INFO("Partitioning tables (DataWeightPerPartition: %v, MaxPartitionCount: %v)", Options_.DataWeightPerPartition, Options_.MaxPartitionCount);

    InitializeChunkPool();
    CollectInput();
    BuildPartitions();

    return Partitions_;
}

void TMultiTablePartitioner::InitializeChunkPool()
{
    ChunkPool_ = CreateChunkPool(Options_.PartitionMode, Options_.DataWeightPerPartition, Logger);
}

void TMultiTablePartitioner::CollectInput()
{
    YT_LOG_INFO("Collecting input (TableCount: %v)", Paths_.size());

    YT_VERIFY(ChunkPool_);

    size_t totalChunkCount = 0;

    for (const auto& [tableIndex, path] : Enumerate(Paths_)) {
        auto rowBuffer = New<TRowBuffer>();
        auto transactionId = path.GetTransactionId();

        auto [inputChunks, schema, dynamic] = CollectTableInputChunks( // TODO(galtsev): make these requests asynchronously; see https://a.yandex-team.ru/review/2564596/details#comment-3570976
            path,
            Client_,
            /* nodeDirectory */ nullptr,
            Options_.FetchChunkSpecConfig,
            transactionId
                ? *transactionId
                : Options_.TransactionId,
            /* fetchHeavyColumnStatisticsExt */ false, // TODO(galtsev): use columnar statistics; see https://a.yandex-team.ru/review/2564596/details#comment-3570985
            Logger);

        YT_LOG_DEBUG("Input chunks fetched (TableIndex: %v, Path: %v, Schema: %v, ChunkCount: %v)", tableIndex, path, schema, inputChunks.size());

        AddDataSource(tableIndex, schema, dynamic);

        YT_LOG_DEBUG("Fetching chunks (Path: %v)", path);

        for (const auto& inputChunk : inputChunks) {
            TLegacyDataSlice::TChunkSliceList inputChunkSliceList;

            auto inputChunkSlice = CreateInputChunkSlice(inputChunk);
            inputChunkSlice->TransformToNew(rowBuffer, schema->ToComparator().GetLength());

            inputChunkSliceList.emplace_back(std::move(inputChunkSlice));

            auto dataSlice = New<TLegacyDataSlice>(
                dynamic ? EDataSourceType::VersionedTable : EDataSourceType::UnversionedTable,
                std::move(inputChunkSliceList),
                TInputSliceLimit());
            dataSlice->SetInputStreamIndex(tableIndex);
            auto chunkStripe = New<TChunkStripe>(std::move(dataSlice));

            ChunkPool_->Add(std::move(chunkStripe));
        }

        totalChunkCount += inputChunks.size();
    }

    YT_LOG_INFO("Finishing chunk pool (TotalChunkCount: %v)", totalChunkCount);

    ChunkPool_->Finish();

    YT_LOG_INFO("Input collected");
}

void TMultiTablePartitioner::BuildPartitions()
{
    YT_LOG_INFO("Building partitions");

    YT_VERIFY(ChunkPool_);
    YT_VERIFY(IsDataSourcesReady());

    while (true) {
        auto cookie = ChunkPool_->Extract();
        if (cookie == IChunkPoolOutput::NullCookie) {
            break;
        }

        if (Options_.MaxPartitionCount && std::ssize(Partitions_.Partitions) >= *Options_.MaxPartitionCount) {
            THROW_ERROR_EXCEPTION("Maximum partition count exceeded: %v", *Options_.MaxPartitionCount);
        }

        auto chunkStripeList = ChunkPool_->GetStripeList(cookie);
        auto slicesByTable = ConvertChunkStripeListIntoDataSliceDescriptors(chunkStripeList);

        Partitions_.Partitions.emplace_back(TMultiTablePartition{CombineDataSlices(DataSourceDirectory_, slicesByTable)});
    }

    YT_LOG_INFO( "Partitions built (PartitionCount: %v)", Partitions_.Partitions.size());
}

bool TMultiTablePartitioner::IsDataSourcesReady()
{
    YT_VERIFY(DataSourceDirectory_->DataSources().size() <= Paths_.size());

    return DataSourceDirectory_->DataSources().size() == Paths_.size();
}

void TMultiTablePartitioner::AddDataSource(size_t tableIndex, const TTableSchemaPtr& schema, bool dynamic)
{
    YT_VERIFY(!IsDataSourcesReady());
    YT_VERIFY(tableIndex == DataSourceDirectory_->DataSources().size());

    auto& dataSource = DataSourceDirectory_->DataSources().emplace_back();
    auto& path = Paths_[tableIndex];

    if (dynamic) {
        dataSource = MakeVersionedDataSource(
            path.GetPath(),
            schema,
            path.GetColumns(),
            /* omittedInaccessibleColumns */ {},
            path.GetTimestamp().value_or(NTransactionClient::AsyncLastCommittedTimestamp));
    } else {
        dataSource = MakeUnversionedDataSource(
            path.GetPath(),
            schema,
            path.GetColumns(),
            /* omittedInaccessibleColumns */ {});
    }
}

std::vector<std::vector<NChunkClient::TDataSliceDescriptor>> TMultiTablePartitioner::ConvertChunkStripeListIntoDataSliceDescriptors(const TChunkStripeListPtr& chunkStripeList)
{
    YT_VERIFY(IsDataSourcesReady());

    std::vector<std::vector<NChunkClient::TDataSliceDescriptor>> slicesByTable(DataSourceDirectory_->DataSources().size());

    for (auto chunkStripe : chunkStripeList->Stripes) {
        for (auto dataSlice : chunkStripe->DataSlices) {
            auto tableIndex = dataSlice->GetInputStreamIndex();
            for (auto chunkSlice : dataSlice->ChunkSlices) {
                YT_VERIFY(tableIndex < std::ssize(slicesByTable));
                auto& dataSliceDescriptor = slicesByTable[tableIndex].emplace_back();
                auto& chunkSpec = dataSliceDescriptor.ChunkSpecs.emplace_back();

                ToProto(
                    &chunkSpec,
                    chunkSlice,
                    DataSourceDirectory_->DataSources()[tableIndex].Schema()->ToComparator(),
                    dataSlice->Type);
            }
        }
    }

    return slicesByTable;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
