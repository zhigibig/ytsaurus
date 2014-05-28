#include "stdafx.h"

#include "job_detail.h"

#include "private.h"

#include <ytlib/job_tracker_client/job.pb.h>

#include <ytlib/new_table_client/schemaless_chunk_reader.h>
#include <ytlib/new_table_client/schemaless_chunk_writer.h>

#include <core/concurrency/scheduler.h>

namespace NYT {
namespace NJobProxy {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NConcurrency;
using namespace NJobTrackerClient::NProto;
using namespace NScheduler::NProto;
using namespace NVersionedTableClient;

////////////////////////////////////////////////////////////////////////////////

static auto& Profiler = JobProxyProfiler;
static auto& Logger = JobProxyLogger;

////////////////////////////////////////////////////////////////////////////////

TJob::TJob(IJobHost* host)
    : Host(MakeWeak(host))
    , StartTime(TInstant::Now())
{
    YCHECK(host);
}

TDuration TJob::GetElapsedTime() const
{
    return TInstant::Now() - StartTime;
}

////////////////////////////////////////////////////////////////////////////////

TSimpleJobBase::TSimpleJobBase(IJobHost* host)
    : TJob(host)
    , JobSpec_(host->GetJobSpec())
    , SchedulerJobSpecExt_(JobSpec_.GetExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext))
    , TotalRowCount_(0)
{ }

TJobResult TSimpleJobBase::Run()
{
    PROFILE_TIMING ("/job_time") {
        LOG_INFO("Initializing");

        {
            auto error = WaitFor(Reader_->Open());
            THROW_ERROR_EXCEPTION_IF_FAILED(error);
        } {
            auto error = WaitFor(Writer_->Open());
            THROW_ERROR_EXCEPTION_IF_FAILED(error);
        }

        PROFILE_TIMING_CHECKPOINT("init");

        LOG_INFO("Reading and writing");
        {
            std::vector<TUnversionedRow> rows;
            rows.reserve(10000);

            while (Reader_->Read(&rows)) {
                if (rows.empty()) {
                    auto error = WaitFor(Reader_->GetReadyEvent());
                    THROW_ERROR_EXCEPTION_IF_FAILED(error);
                    continue;
                }

                if (!Writer_->Write(rows)) {
                    auto error = WaitFor(Writer_->GetReadyEvent());
                    THROW_ERROR_EXCEPTION_IF_FAILED(error);
                }
            }

            YCHECK(rows.empty());
        }

        PROFILE_TIMING_CHECKPOINT("reading_writing");

        LOG_INFO("Finalizing");
        {
            auto error = WaitFor(Writer_->Close());
            THROW_ERROR_EXCEPTION_IF_FAILED(error);

            TJobResult result;
            ToProto(result.mutable_error(), TError());
            return result;
        }
    }
}

double TSimpleJobBase::GetProgress() const
{
    if (TotalRowCount_ == 0) {
        LOG_WARNING("Job progress: empty total");
        return 0;
    } else {
        i64 rowCount = Reader_->GetDataStatistics().row_count();
        double progress = (double) rowCount / TotalRowCount_;
        LOG_DEBUG("Job progress: %lf, read row count: %" PRId64, progress, rowCount);
        return progress;
    }
}

std::vector<TChunkId> TSimpleJobBase::GetFailedChunkIds() const
{
    return Reader_->GetFailedChunkIds();
}

TJobStatistics TSimpleJobBase::GetStatistics() const
{
    TJobStatistics result;
    result.set_time(GetElapsedTime().MilliSeconds());
    ToProto(result.mutable_input(), Reader_->GetDataStatistics());
    ToProto(result.mutable_output(), Writer_->GetDataStatistics());
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT

