#pragma once

#include "scheduling_context.h"
#include "job_resources.h"

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

class TSchedulingContextBase
    : public ISchedulingContext
{
public:
    DEFINE_BYREF_RW_PROPERTY(TJobResources, ResourceUsageDiscount);
    DEFINE_BYREF_RW_PROPERTY(TJobResources, ResourceUsage);
    DEFINE_BYREF_RO_PROPERTY(TJobResources, ResourceLimits);

    DEFINE_BYREF_RO_PROPERTY(std::vector<TJobPtr>, StartedJobs);
    DEFINE_BYREF_RO_PROPERTY(std::vector<TJobPtr>, PreemptedJobs);
    DEFINE_BYREF_RO_PROPERTY(std::vector<TJobPtr>, RunningJobs);

public:
    TSchedulingContextBase(
        TSchedulerConfigPtr config,
        TExecNodePtr node,
        const std::vector<TJobPtr>& runningJobs,
        NObjectClient::TCellTag cellTag);

    virtual const TExecNodeDescriptor& GetNodeDescriptor() const override;

    virtual TJobPtr GetStartedJob(const TJobId& jobId) const override;

    virtual bool CanStartMoreJobs() const override;
    virtual bool CanSchedule(const TNullable<Stroka>& tag) const override;

    virtual void StartJob(TOperationPtr operation, TJobStartRequestPtr jobStartRequest) override;

    virtual void PreemptJob(TJobPtr job) override;

    virtual TJobId GenerateJobId() override;

private:
    const TSchedulerConfigPtr Config_;
    const NObjectClient::TCellTag CellTag_;
    const TExecNodePtr Node_;
    const TExecNodeDescriptor NodeDescriptor_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
