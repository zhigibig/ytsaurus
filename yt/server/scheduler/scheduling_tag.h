#pragma once

#include "public.h"

#include <yt/core/misc/boolean_formula.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

class TSchedulingTagFilter
{
public:
    TSchedulingTagFilter();
    explicit TSchedulingTagFilter(const TBooleanFormula& dnf);

    bool CanSchedule(const yhash_set<Stroka>& nodeTags) const;

    bool IsEmpty() const;

    size_t GetHash() const;
    const TBooleanFormula& GetBooleanFormula() const;

private:
    TBooleanFormula BooleanFormula_;
    size_t Hash_;
};

bool operator==(const TSchedulingTagFilter& lhs, const TSchedulingTagFilter& rhs);

extern const TSchedulingTagFilter EmptySchedulingTagFilter;

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

////////////////////////////////////////////////////////////////////

template <>
struct hash<NYT::NScheduler::TSchedulingTagFilter>
{
    size_t operator()(const NYT::NScheduler::TSchedulingTagFilter& filter) const;
};

////////////////////////////////////////////////////////////////////
