﻿#include "stdafx.h"

#include "partition_job.h"

#include "job_detail.h"
#include "config.h"
#include "private.h"

#include <ytlib/chunk_client/chunk_spec.h>

#include <ytlib/new_table_client/name_table.h>
#include <ytlib/new_table_client/partitioner.h>
#include <ytlib/new_table_client/schemaless_chunk_reader.h>
#include <ytlib/new_table_client/schemaless_chunk_writer.h>

namespace NYT {
namespace NJobProxy {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NScheduler::NProto;
using namespace NTransactionClient;
using namespace NVersionedTableClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

class TPartitionJob
    : public TSimpleJobBase
{
public:
    explicit TPartitionJob(IJobHost* host)
        : TSimpleJobBase(host)
        , PartitionJobSpecExt_(host->GetJobSpec().GetExtension(TPartitionJobSpecExt::partition_job_spec_ext))
    {
        auto config = host->GetConfig();

        YCHECK(SchedulerJobSpecExt_.input_specs_size() == 1);
        const auto& inputSpec = SchedulerJobSpecExt_.input_specs(0);

        std::vector<TChunkSpec> chunkSpecs(inputSpec.chunks().begin(), inputSpec.chunks().end());

        TotalRowCount_ = GetCumulativeRowCount(chunkSpecs);

        auto keyColumns = FromProto<TKeyColumns>(PartitionJobSpecExt_.sort_key_columns());
        NameTable_ = TNameTable::FromKeyColumns(keyColumns);

        ReaderFactory_ = [=] (TNameTablePtr nameTable, TColumnFilter columnFilter) {
            YCHECK(!Reader_);
            Reader_ = CreateSchemalessParallelMultiChunkReader(
                config->JobIO->TableReader,
                New<TMultiChunkReaderOptions>(),
                host->GetMasterChannel(),
                host->GetBlockCache(),
                host->GetNodeDirectory(),
                std::move(chunkSpecs),
                nameTable,
                columnFilter,
                TKeyColumns());
            return Reader_;
        };

        YCHECK(SchedulerJobSpecExt_.output_specs_size() == 1);
        const auto& outputSpec = SchedulerJobSpecExt_.output_specs(0);

        auto transactionId = FromProto<TTransactionId>(SchedulerJobSpecExt_.output_transaction_id());
        auto chunkListId = FromProto<TChunkListId>(outputSpec.chunk_list_id());

        auto options = ConvertTo<TTableWriterOptionsPtr>(TYsonString(outputSpec.table_writer_options()));

        WriterFactory_ = [=] (TNameTablePtr nameTable) mutable {
            YCHECK(!Writer_);
            Writer_ = CreatePartitionMultiChunkWriter(
                config->JobIO->TableWriter,
                options,
                nameTable,
                keyColumns,
                host->GetMasterChannel(),
                transactionId,
                chunkListId,
                GetPartitioner());
            return Writer_;
        };
    }

private:
    const TPartitionJobSpecExt& PartitionJobSpecExt_;
    std::vector<TOwningKey> PartitionKeys_;
    TNameTablePtr NameTable_;

    virtual void CreateReader() override
    {
        ReaderFactory_(NameTable_, TColumnFilter());
    }

    virtual void CreateWriter() override
    {
        WriterFactory_(NameTable_);
    }

    std::unique_ptr<IPartitioner> GetPartitioner()
    {
        std::unique_ptr<IPartitioner> partitioner;
        if (PartitionJobSpecExt_.partition_keys_size() > 0) {
            YCHECK(PartitionJobSpecExt_.partition_keys_size() + 1 == PartitionJobSpecExt_.partition_count());
            for (const auto& protoKey : PartitionJobSpecExt_.partition_keys()) {
                TOwningKey key;
                FromProto(&key, protoKey);
                PartitionKeys_.push_back(key);
            }
            partitioner = CreateOrderedPartitioner(&PartitionKeys_);
        } else {
            partitioner = CreateHashPartitioner(
                PartitionJobSpecExt_.partition_count(),
                PartitionJobSpecExt_.reduce_key_column_count());
        }
        return partitioner;
    }
};

IJobPtr CreatePartitionJob(IJobHost* host)
{
    return New<TPartitionJob>(host);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
