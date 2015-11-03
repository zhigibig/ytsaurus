#pragma once

#include "public.h"

#include <core/yson/public.h>

#include <core/ytree/public.h>

#include <core/rpc/public.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

struct TWorkloadDescriptor
{
    explicit TWorkloadDescriptor(
        EWorkloadCategory category = EWorkloadCategory::Idle,
        int band = 0,
        TInstant instant = TInstant::Now())
        : Category(category)
        , Band(band)
        , Instant(instant)
    { }

    //! The type of the workload.
    EWorkloadCategory Category;

    //! The relative importance of the workload (among others within the category).
    //! Zero is the default value.
    //! Smaller is better.
    int Band;

    //! The time instant when this workload has been initiated.
    //! Certain categories rely on this value for FIFO ordering.
    TInstant Instant;

    //! Updates the instant field with the current time.
    TWorkloadDescriptor SetCurrentInstant() const;

    //! Computes the aggregated priority.
    //! Smaller is better.
    i64 GetPriority() const;
};

i64 GetBasicPriority(EWorkloadCategory category);

void FormatValue(
    TStringBuilder* builder,
    const TWorkloadDescriptor& descriptor,
    const TStringBuf& format);
Stroka ToString(const TWorkloadDescriptor& descriptor);

void ToProto(NYT::NProto::TWorkloadDescriptor* protoDescriptor, const TWorkloadDescriptor& descriptor);
void FromProto(TWorkloadDescriptor* descriptor, const NYT::NProto::TWorkloadDescriptor& protoDescriptor);

void Serialize(const TWorkloadDescriptor& descriptor, NYson::IYsonConsumer* consumer);
void Deserialize(TWorkloadDescriptor& descriptor, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
