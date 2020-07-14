#include "merge_job.h"
#include "private.h"
#include "job_detail.h"

#include <yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/ytlib/chunk_client/data_source.h>
#include <yt/ytlib/chunk_client/job_spec_extensions.h>
#include <yt/ytlib/chunk_client/parallel_reader_memory_manager.h>

#include <yt/ytlib/job_proxy/helpers.h>

#include <yt/client/object_client/helpers.h>

#include <yt/client/table_client/name_table.h>
#include <yt/ytlib/table_client/schemaless_multi_chunk_reader.h>
#include <yt/ytlib/table_client/schemaless_chunk_writer.h>

namespace NYT::NJobProxy {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NScheduler::NProto;
using namespace NTransactionClient;
using namespace NTableClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;

using NChunkClient::TDataSliceDescriptor;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = JobProxyLogger;

////////////////////////////////////////////////////////////////////////////////

class TMergeJob
    : public TSimpleJobBase
{
public:
    TMergeJob(IJobHostPtr host, bool useParallelReader)
        : TSimpleJobBase(host)
        , UseParallelReader_(useParallelReader)
    {
        YT_VERIFY(SchedulerJobSpecExt_.output_table_specs_size() == 1);
    }

    virtual void Initialize() override
    {
        TSimpleJobBase::Initialize();

        TKeyColumns keyColumns;
        std::optional<int> partitionTag;
        if (JobSpec_.HasExtension(TMergeJobSpecExt::merge_job_spec_ext)) {
            const auto& mergeJobSpec = JobSpec_.GetExtension(TMergeJobSpecExt::merge_job_spec_ext);
            keyColumns = FromProto<TKeyColumns>(mergeJobSpec.key_columns());
            if (SchedulerJobSpecExt_.has_partition_tag()) {
                partitionTag = SchedulerJobSpecExt_.partition_tag();
            } else if (mergeJobSpec.has_partition_tag()) {
                partitionTag = mergeJobSpec.partition_tag();
            }
            YT_LOG_INFO("Ordered merge produces sorted output");
        }

        std::vector<TDataSliceDescriptor> dataSliceDescriptors;
        for (const auto& inputSpec : SchedulerJobSpecExt_.input_table_specs()) {
            auto descriptors = UnpackDataSliceDescriptors(inputSpec);
            dataSliceDescriptors.insert(dataSliceDescriptors.end(), descriptors.begin(), descriptors.end());
        }

        TotalRowCount_ = SchedulerJobSpecExt_.input_row_count();

        auto readerOptions = ConvertTo<TTableReaderOptionsPtr>(TYsonString(
            SchedulerJobSpecExt_.table_reader_options()));
        auto dataSourceDirectoryExt = GetProtoExtension<TDataSourceDirectoryExt>(SchedulerJobSpecExt_.extensions());
        auto dataSourceDirectory = FromProto<TDataSourceDirectoryPtr>(dataSourceDirectoryExt);

        NameTable_ = TNameTable::FromKeyColumns(keyColumns);

        auto readerFactory = UseParallelReader_
            ? CreateSchemalessParallelMultiReader
            : CreateSchemalessSequentialMultiReader;

        ReaderFactory_ = [=] (TNameTablePtr nameTable, const TColumnFilter& columnFilter) {
            YT_VERIFY(!Reader_);
            const auto& tableReaderConfig = Host_->GetJobSpecHelper()->GetJobIOConfig()->TableReader;
            Reader_ = readerFactory(
                tableReaderConfig,
                readerOptions,
                Host_->GetClient(),
                Host_->LocalDescriptor(),
                std::nullopt,
                Host_->GetBlockCache(),
                Host_->GetInputNodeDirectory(),
                dataSourceDirectory,
                std::move(dataSliceDescriptors),
                std::move(nameTable),
                BlockReadOptions_,
                columnFilter,
                /* keyColumns */ {},
                partitionTag,
                Host_->GetTrafficMeter(),
                Host_->GetInBandwidthThrottler(),
                Host_->GetOutRpsThrottler(),
                MultiReaderMemoryManager_->CreateMultiReaderMemoryManager(tableReaderConfig->WindowSize));
            return Reader_;
        };

        auto transactionId = FromProto<TTransactionId>(SchedulerJobSpecExt_.output_transaction_id());
        const auto& outputSpec = SchedulerJobSpecExt_.output_table_specs(0);
        auto chunkListId = FromProto<TChunkListId>(outputSpec.chunk_list_id());
        auto options = ConvertTo<TTableWriterOptionsPtr>(TYsonString(outputSpec.table_writer_options()));
        options->CastAnyToComposite = true;
        auto schema = FromProto<TTableSchemaPtr>(outputSpec.table_schema());

        auto writerConfig = GetWriterConfig(outputSpec);
        auto timestamp = static_cast<TTimestamp>(outputSpec.timestamp());

        WriterFactory_ = [=] (TNameTablePtr nameTable, TTableSchemaPtr /*schema*/) {
            YT_VERIFY(!Writer_);
            Writer_ = CreateSchemalessMultiChunkWriter(
                writerConfig,
                options,
                nameTable,
                schema,
                TOwningKey(),
                Host_->GetClient(),
                CellTagFromId(chunkListId),
                transactionId,
                chunkListId,
                TChunkTimestamps{timestamp, timestamp},
                Host_->GetTrafficMeter(),
                Host_->GetOutBandwidthThrottler());
            return Writer_;
        };
    }

private:
    const bool UseParallelReader_;

    TNameTablePtr NameTable_;

    virtual void CreateReader() override
    {
        ReaderFactory_(NameTable_, TColumnFilter());
    }

    virtual void CreateWriter() override
    {
        // NB. WriterFactory_ ignores schema argument and uses schema of output table.
        WriterFactory_(NameTable_, nullptr);
    }

    virtual i64 GetTotalReaderMemoryLimit() const
    {
        return Host_->GetJobSpecHelper()->GetJobIOConfig()->TableReader->MaxBufferSize;
    }
};

IJobPtr CreateOrderedMergeJob(IJobHostPtr host)
{
    return New<TMergeJob>(host, false);
}

IJobPtr CreateUnorderedMergeJob(IJobHostPtr host)
{
    return New<TMergeJob>(host, true);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
