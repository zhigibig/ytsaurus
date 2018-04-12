#include "private.h"

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

NLogging::TLogger JobProxyClientLogger("JobProxyClient");

TString GetDefaultJobsMetaContainerName()
{
    return "yt_jobs_meta";
}

TString GetSlotMetaContainerName(int slotIndex)
{
    return Format("slot_meta_%v", slotIndex);
}

TString GetFullSlotMetaContainerName(const TString& jobsMetaName, int slotIndex)
{
    return Format(
        "%v/%v",
        jobsMetaName,
        GetSlotMetaContainerName(slotIndex));
}

TString GetUserJobMetaContainerName()
{
    return "user_job_meta";
}

TString GetFullUserJobMetaContainerName(const TString& jobsMetaName, int slotIndex)
{
    return Format(
        "%v/%v/%v",
        jobsMetaName,
        GetSlotMetaContainerName(slotIndex),
        GetUserJobMetaContainerName());
}

TString GetJobProxyMetaContainerName()
{
    return "job_proxy_meta";
}

TString GetFullJobProxyMetaContainerName(const TString& jobsMetaName, int slotIndex)
{
    return Format(
        "%v/%v/%v",
        jobsMetaName,
        GetSlotMetaContainerName(slotIndex),
        GetJobProxyMetaContainerName());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
