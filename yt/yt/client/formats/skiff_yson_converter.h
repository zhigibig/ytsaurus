#pragma once

#include <yt/client/table_client/public.h>
#include <yt/client/table_client/row_base.h>

#include <yt/library/decimal/decimal.h>
#include <yt/core/yson/public.h>

#include <library/cpp/skiff/skiff.h>

namespace NYT::NFormats {

////////////////////////////////////////////////////////////////////////////////

NSkiff::EWireType GetSkiffTypeForSimpleLogicalType(NTableClient::ESimpleLogicalValueType logicalType);

////////////////////////////////////////////////////////////////////////////////

using TYsonToSkiffConverter = std::function<void(NYson::TYsonPullParserCursor*, NSkiff::TCheckedInDebugSkiffWriter*)>;
using TSkiffToYsonConverter = std::function<void(NSkiff::TCheckedInDebugSkiffParser*, NYson::TCheckedInDebugYsonTokenWriter*)>;

struct TYsonToSkiffConverterConfig
{
    // Usually skiffSchema MUST match descriptor.LogicalType.
    // But when AllowOmitTopLevelOptional is set to true and descriptor.LogicalType is Optional<SomeInnerType>
    // skiffSchema CAN match SomeInnerType. In that case returned converter will throw error when it gets empty value.
    //
    // Useful for sparse fields.
    bool AllowOmitTopLevelOptional = false;
};

TYsonToSkiffConverter CreateYsonToSkiffConverter(
    const NTableClient::TComplexTypeFieldDescriptor& descriptor,
    const std::shared_ptr<NSkiff::TSkiffSchema>& skiffSchema,
    const TYsonToSkiffConverterConfig& config = {});

struct TSkiffToYsonConverterConfig
{
    // Similar to TYsonToSkiffConverterConfig::AllowOmitTopLevelOptional.
    bool AllowOmitTopLevelOptional = false;
};

TSkiffToYsonConverter CreateSkiffToYsonConverter(
    const NTableClient::TComplexTypeFieldDescriptor& descriptor,
    const std::shared_ptr<NSkiff::TSkiffSchema>& skiffSchema,
    const TSkiffToYsonConverterConfig& config = {});

////////////////////////////////////////////////////////////////////////////////

template <NSkiff::EWireType wireType>
struct TSimpleSkiffParser
{
    Y_FORCE_INLINE auto operator () (NSkiff::TCheckedInDebugSkiffParser* parser) const;
};

template <NSkiff::EWireType SkiffWireType>
class TDecimalSkiffParser
{
public:
    explicit TDecimalSkiffParser(int precision);
    Y_FORCE_INLINE TStringBuf operator() (NSkiff::TCheckedInDebugSkiffParser* parser) const;

private:
    const int Precision_;
    mutable char Buffer_[NDecimal::TDecimal::MaxBinarySize];
};

////////////////////////////////////////////////////////////////////////////////

template <NSkiff::EWireType SkiffWireType>
class TDecimalSkiffWriter
{
public:
    explicit TDecimalSkiffWriter(int precision);

    void operator() (TStringBuf value, NSkiff::TCheckedInDebugSkiffWriter* writer) const;

private:
    const int Precision_;
};

////////////////////////////////////////////////////////////////////////////////

void CheckSkiffWireTypeForDecimal(int precision, NSkiff::EWireType wireType);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::Formats

#define SKIFF_YSON_CONVERTER_INL_H_
#include "skiff_yson_converter-inl.h"
#undef SKIFF_YSON_CONVERTER_INL_H_
