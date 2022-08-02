#pragma once

#include "client_impl.h"

#include <yt/yt/ytlib/chunk_client/public.h>

#include <yt/yt/client/table_client/row_buffer.h>

namespace NYT::NApi::NNative {

////////////////////////////////////////////////////////////////////////////////

class TMultiTablePartitioner
{
public:
    TMultiTablePartitioner(
        const IClientPtr& client,
        const std::vector<NYPath::TRichYPath>& paths,
        const TPartitionTablesOptions& options,
        const NLogging::TLogger& logger);

    TMultiTablePartitions PartitionTables();

private:
    struct TVersionedSliceFetchState
    {
        std::vector<TFuture<void>> AsyncResults;
        std::vector<NTableClient::IChunkSliceFetcherPtr> TableFetchers;
        std::vector<size_t> TableIndices;
    };

    const IClientPtr Client_;
    const std::vector<NYPath::TRichYPath> Paths_;
    const TPartitionTablesOptions Options_;
    const NLogging::TLogger Logger;

    NChunkPools::IChunkPoolPtr ChunkPool_;
    const NChunkClient::TDataSourceDirectoryPtr DataSourceDirectory_ = New<NChunkClient::TDataSourceDirectory>();
    TMultiTablePartitions Partitions_;
    NTableClient::TRowBufferPtr RowBuffer_ = New<NTableClient::TRowBuffer>();
    TVersionedSliceFetchState FetchState_;

    void InitializeChunkPool();
    void CollectInput();
    void BuildPartitions();

    bool IsDataSourcesReady();
    void AddDataSource(int tableIndex, const NQueryClient::TTableSchemaPtr& schema, bool dynamic);
    std::vector<std::vector<NChunkClient::TDataSliceDescriptor>> ConvertChunkStripeListIntoDataSliceDescriptors(
        const NChunkPools::TChunkStripeListPtr& chunkStripeList);
    void AddDataSlice(int tableIndex, NChunkClient::TLegacyDataSlicePtr dataSlice);
    void RequestVersionedDataSlices(
        const std::vector<NChunkClient::TInputChunkPtr>& inputChunks,
        int tableIndex,
        const NTableClient::TTableSchemaPtr& schema,
        const NChunkClient::TChunkSliceFetcherConfigPtr& chunkSliceFetcherConfig);
    void FetchVersionedDataSlices();
    void AddUnversionedDataSlices(const std::vector<NChunkClient::TInputChunkPtr>& inputChunks,
        int tableIndex,
        const NTableClient::TTableSchemaPtr& schema);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NNative
