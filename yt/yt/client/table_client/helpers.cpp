#include "helpers.h"
#include "schema.h"
#include "name_table.h"
#include "key_bound.h"

#include <yt/yt_proto/yt/client/table_chunk_format/proto/chunk_meta.pb.h>

#include <yt/yt/core/ytree/convert.h>

#include <yt/yt/core/net/address.h>

#include <yt/yt/core/yson/parser.h>
#include <yt/yt/core/yson/protobuf_interop.h>
#include <yt/yt/core/yson/token_writer.h>

#include <google/protobuf/io/zero_copy_stream_impl_lite.h>

namespace NYT::NTableClient {

using namespace NYTree;
using namespace NYson;
using namespace NNet;

using namespace google::protobuf;
using namespace google::protobuf::io;

////////////////////////////////////////////////////////////////////////////////

namespace {

void YTreeNodeToUnversionedValue(
    TUnversionedOwningRowBuilder* builder,
    const INodePtr& value,
    int id,
    EValueFlags flags)
{
    switch (value->GetType()) {
        case ENodeType::Entity:
            builder->AddValue(MakeUnversionedSentinelValue(EValueType::Null, id, flags));
            break;
        case ENodeType::Int64:
            builder->AddValue(MakeUnversionedInt64Value(value->GetValue<i64>(), id, flags));
            break;
        case ENodeType::Uint64:
            builder->AddValue(MakeUnversionedUint64Value(value->GetValue<ui64>(), id, flags));
            break;
        case ENodeType::Double:
            builder->AddValue(MakeUnversionedDoubleValue(value->GetValue<double>(), id, flags));
            break;
        case ENodeType::String:
            builder->AddValue(MakeUnversionedStringValue(value->GetValue<TString>(), id, flags));
            break;
        default:
            builder->AddValue(MakeUnversionedAnyValue(ConvertToYsonString(value).AsStringBuf(), id, flags));
            break;
    }
}

} // namespace

TUnversionedOwningRow YsonToSchemafulRow(
    const TString& yson,
    const TTableSchema& tableSchema,
    bool treatMissingAsNull)
{
    auto nameTable = TNameTable::FromSchema(tableSchema);

    auto rowParts = ConvertTo<THashMap<TString, INodePtr>>(
        TYsonString(yson, EYsonType::MapFragment));

    TUnversionedOwningRowBuilder rowBuilder;
    auto addValue = [&] (int id, INodePtr value) {
        if (value->GetType() == ENodeType::Entity) {
            rowBuilder.AddValue(MakeUnversionedSentinelValue(
                value->Attributes().Get<EValueType>("type", EValueType::Null), id));
            return;
        }

        switch (tableSchema.Columns()[id].GetWireType()) {
            case EValueType::Boolean:
                rowBuilder.AddValue(MakeUnversionedBooleanValue(value->GetValue<bool>(), id));
                break;
            case EValueType::Int64:
                rowBuilder.AddValue(MakeUnversionedInt64Value(value->GetValue<i64>(), id));
                break;
            case EValueType::Uint64:
                rowBuilder.AddValue(MakeUnversionedUint64Value(value->GetValue<ui64>(), id));
                break;
            case EValueType::Double:
                rowBuilder.AddValue(MakeUnversionedDoubleValue(value->GetValue<double>(), id));
                break;
            case EValueType::String:
                rowBuilder.AddValue(MakeUnversionedStringValue(value->GetValue<TString>(), id));
                break;
            case EValueType::Any:
                rowBuilder.AddValue(MakeUnversionedAnyValue(ConvertToYsonString(value).AsStringBuf(), id));
                break;
            case EValueType::Composite:
                rowBuilder.AddValue(MakeUnversionedCompositeValue(ConvertToYsonString(value).AsStringBuf(), id));
                break;
            case EValueType::Null:

            case EValueType::Min:
            case EValueType::Max:
            case EValueType::TheBottom:
                YT_ABORT();
        }
    };

    const auto& keyColumns = tableSchema.GetKeyColumns();

    // Key
    for (int id = 0; id < std::ssize(keyColumns); ++id) {
        auto it = rowParts.find(nameTable->GetName(id));
        if (it == rowParts.end()) {
            rowBuilder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id));
        } else {
            addValue(id, it->second);
        }
    }

    // Fixed values
    for (int id = std::ssize(keyColumns); id < std::ssize(tableSchema.Columns()); ++id) {
        auto it = rowParts.find(nameTable->GetName(id));
        if (it != rowParts.end()) {
            addValue(id, it->second);
        } else if (treatMissingAsNull) {
            rowBuilder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, id));
        }
    }

    // Variable values
    for (const auto& [name, value] : rowParts) {
        int id = nameTable->GetIdOrRegisterName(name);
        if (id >= std::ssize(tableSchema.Columns())) {
            YTreeNodeToUnversionedValue(&rowBuilder, value, id, EValueFlags::None);
        }
    }

    return rowBuilder.FinishRow();
}

TUnversionedOwningRow YsonToSchemalessRow(const TString& valueYson)
{
    TUnversionedOwningRowBuilder builder;

    auto values = ConvertTo<std::vector<INodePtr>>(TYsonString(valueYson, EYsonType::ListFragment));
    for (const auto& value : values) {
        int id = value->Attributes().Get<int>("id");
        auto flags = EValueFlags::None;
        if (value->Attributes().Get<bool>("aggregate", false)) {
            flags |= EValueFlags::Aggregate;
        }
        YTreeNodeToUnversionedValue(&builder, value, id, flags);
    }

    return builder.FinishRow();
}

TVersionedRow YsonToVersionedRow(
    const TRowBufferPtr& rowBuffer,
    const TString& keyYson,
    const TString& valueYson,
    const std::vector<TTimestamp>& deleteTimestamps,
    const std::vector<TTimestamp>& extraWriteTimestamps)
{
    TVersionedRowBuilder builder(rowBuffer);

    auto keys = ConvertTo<std::vector<INodePtr>>(TYsonString(keyYson, EYsonType::ListFragment));

    for (auto key : keys) {
        int id = key->Attributes().Get<int>("id");
        switch (key->GetType()) {
            case ENodeType::Int64:
                builder.AddKey(MakeUnversionedInt64Value(key->GetValue<i64>(), id));
                break;
            case ENodeType::Uint64:
                builder.AddKey(MakeUnversionedUint64Value(key->GetValue<ui64>(), id));
                break;
            case ENodeType::Double:
                builder.AddKey(MakeUnversionedDoubleValue(key->GetValue<double>(), id));
                break;
            case ENodeType::String:
                builder.AddKey(MakeUnversionedStringValue(key->GetValue<TString>(), id));
                break;
            case ENodeType::Entity:
                builder.AddKey(MakeUnversionedSentinelValue(EValueType::Null, id));
                break;
            default:
                YT_ABORT();
                break;
        }
    }

    auto values = ConvertTo<std::vector<INodePtr>>(TYsonString(valueYson, EYsonType::ListFragment));
    for (auto value : values) {
        int id = value->Attributes().Get<int>("id");
        auto timestamp = value->Attributes().Get<TTimestamp>("ts");
        auto flags = EValueFlags::None;
        if (value->Attributes().Get<bool>("aggregate", false)) {
            flags |= EValueFlags::Aggregate;
        }
        switch (value->GetType()) {
            case ENodeType::Entity:
                builder.AddValue(MakeVersionedSentinelValue(EValueType::Null, timestamp, id, flags));
                break;
            case ENodeType::Int64:
                builder.AddValue(MakeVersionedInt64Value(value->GetValue<i64>(), timestamp, id, flags));
                break;
            case ENodeType::Uint64:
                builder.AddValue(MakeVersionedUint64Value(value->GetValue<ui64>(), timestamp, id, flags));
                break;
            case ENodeType::Double:
                builder.AddValue(MakeVersionedDoubleValue(value->GetValue<double>(), timestamp, id, flags));
                break;
            case ENodeType::String:
                builder.AddValue(MakeVersionedStringValue(value->GetValue<TString>(), timestamp, id, flags));
                break;
            default:
                builder.AddValue(MakeVersionedAnyValue(ConvertToYsonString(value).AsStringBuf(), timestamp, id, flags));
                break;
        }
    }

    for (auto timestamp : deleteTimestamps) {
        builder.AddDeleteTimestamp(timestamp);
    }

    for (auto timestamp : extraWriteTimestamps) {
        builder.AddWriteTimestamp(timestamp);
    }

    return builder.FinishRow();
}

TVersionedOwningRow YsonToVersionedRow(
    const TString& keyYson,
    const TString& valueYson,
    const std::vector<TTimestamp>& deleteTimestamps,
    const std::vector<TTimestamp>& extraWriteTimestamps)
{
    // NB: this implementation is extra slow, it is intended only for using in tests.
    auto rowBuffer = New<TRowBuffer>();
    auto row = YsonToVersionedRow(rowBuffer, keyYson, valueYson, deleteTimestamps, extraWriteTimestamps);
    return TVersionedOwningRow(row);
}

TUnversionedOwningRow YsonToKey(const TString& yson)
{
    TUnversionedOwningRowBuilder keyBuilder;
    auto keyParts = ConvertTo<std::vector<INodePtr>>(
        TYsonString(yson, EYsonType::ListFragment));

    for (int id = 0; id < std::ssize(keyParts); ++id) {
        const auto& keyPart = keyParts[id];
        switch (keyPart->GetType()) {
            case ENodeType::Int64:
                keyBuilder.AddValue(MakeUnversionedInt64Value(
                    keyPart->GetValue<i64>(),
                    id));
                break;
            case ENodeType::Uint64:
                keyBuilder.AddValue(MakeUnversionedUint64Value(
                    keyPart->GetValue<ui64>(),
                    id));
                break;
            case ENodeType::Double:
                keyBuilder.AddValue(MakeUnversionedDoubleValue(
                    keyPart->GetValue<double>(),
                    id));
                break;
            case ENodeType::String:
                keyBuilder.AddValue(MakeUnversionedStringValue(
                    keyPart->GetValue<TString>(),
                    id));
                break;
            case ENodeType::Entity:
                keyBuilder.AddValue(MakeUnversionedSentinelValue(
                    keyPart->Attributes().Get<EValueType>("type", EValueType::Null),
                    id));
                break;
            default:
                keyBuilder.AddValue(MakeUnversionedAnyValue(
                    ConvertToYsonString(keyPart).AsStringBuf(),
                    id));
                break;
        }
    }

    return keyBuilder.FinishRow();
}

TString KeyToYson(TUnversionedRow row)
{
    return ConvertToYsonString(row, EYsonFormat::Text).ToString();
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    std::nullopt_t,
    const TRowBufferPtr& /*rowBuffer*/,
    int id,
    EValueFlags flags)
{
    *unversionedValue = MakeUnversionedSentinelValue(EValueType::Null, id, flags);
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    TGuid value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    auto strValue = ToString(value);
    *unversionedValue = value
        ? rowBuffer->CaptureValue(MakeUnversionedStringValue(strValue, id, flags))
        : MakeUnversionedSentinelValue(EValueType::Null, id, flags);
}

void FromUnversionedValue(TGuid* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = TGuid();
        return;
    }
    if (unversionedValue.Type != EValueType::String) {
        THROW_ERROR_EXCEPTION("Cannot parse object id value from %Qlv",
            unversionedValue.Type);
    }
    *value = TGuid::FromString(unversionedValue.AsStringBuf());
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const TString& value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    ToUnversionedValue(unversionedValue, static_cast<TStringBuf>(value), rowBuffer, id, flags);
}

void FromUnversionedValue(TString* value, TUnversionedValue unversionedValue)
{
    TStringBuf uncapturedValue;
    FromUnversionedValue(&uncapturedValue, unversionedValue);
    *value = TString(uncapturedValue);
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    TStringBuf value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    *unversionedValue = rowBuffer->CaptureValue(MakeUnversionedStringValue(value, id, flags));
}

void FromUnversionedValue(TStringBuf* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = TStringBuf{};
        return;
    }
    if (unversionedValue.Type != EValueType::String) {
        THROW_ERROR_EXCEPTION("Cannot parse string value from %Qlv",
            unversionedValue.Type);
    }
    *value = unversionedValue.AsStringBuf();
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const char* value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    ToUnversionedValue(unversionedValue, TStringBuf(value), rowBuffer, id, flags);
}

void FromUnversionedValue(const char** value, TUnversionedValue unversionedValue)
{
    *value = FromUnversionedValue<TStringBuf>(unversionedValue).data();
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    bool value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    *unversionedValue = rowBuffer->CaptureValue(MakeUnversionedBooleanValue(value, id, flags));
}

void FromUnversionedValue(bool* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = false;
        return;
    }
    if (unversionedValue.Type != EValueType::Boolean) {
        THROW_ERROR_EXCEPTION("Cannot parse \"boolean\" value from %Qlv",
            unversionedValue.Type);
    }
    *value = unversionedValue.Data.Boolean;
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const TYsonString& value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    YT_ASSERT(value.GetType() == EYsonType::Node);
    *unversionedValue = rowBuffer->CaptureValue(MakeUnversionedAnyValue(value.AsStringBuf(), id, flags));
}

void FromUnversionedValue(TYsonString* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse YSON string from %Qlv",
            unversionedValue.Type);
    }
    *value = TYsonString(unversionedValue.AsString());
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const NYson::TYsonStringBuf& value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    YT_ASSERT(value.GetType() == EYsonType::Node);
    *unversionedValue = rowBuffer->CaptureValue(MakeUnversionedAnyValue(value.AsStringBuf(), id, flags));
}

void FromUnversionedValue(NYson::TYsonStringBuf* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse YSON string from %Qlv",
            unversionedValue.Type);
    }
    *value = TYsonStringBuf(unversionedValue.AsStringBuf());
}

////////////////////////////////////////////////////////////////////////////////

#define XX(cppType, codeType, humanReadableType) \
    void ToUnversionedValue( \
        TUnversionedValue* unversionedValue, \
        cppType value, \
        const TRowBufferPtr& /*rowBuffer*/, \
        int id, \
        EValueFlags flags) \
    { \
        *unversionedValue = MakeUnversioned ## codeType ## Value(value, id, flags); \
    } \
    \
    void FromUnversionedValue(cppType* value, TUnversionedValue unversionedValue) \
    { \
        switch (unversionedValue.Type) { \
            case EValueType::Int64: \
                *value = CheckedIntegralCast<cppType>(unversionedValue.Data.Int64); \
                break; \
            case EValueType::Uint64: \
                *value = CheckedIntegralCast<cppType>(unversionedValue.Data.Uint64); \
                break; \
            default: \
                THROW_ERROR_EXCEPTION("Cannot parse \"" #humanReadableType "\" value from %Qlv", \
                    unversionedValue.Type); \
        } \
    }

XX(i64,  Int64,  int64)
XX(ui64, Uint64, uint64)
XX(i32,  Int64,  int32)
XX(ui32, Uint64, uint32)
XX(i16,  Int64,  int32)
XX(ui16, Uint64, uint16)
XX(i8,   Int64,  int8)
XX(ui8,  Uint64, uint8)

#undef XX

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    double value,
    const TRowBufferPtr& /*rowBuffer*/,
    int id,
    EValueFlags flags)
{
    *unversionedValue = MakeUnversionedDoubleValue(value, id, flags);
}

void FromUnversionedValue(double* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type != EValueType::Double) {
        THROW_ERROR_EXCEPTION("Cannot parse \"double\" value from %Qlv",
            unversionedValue.Type);
    }
    *value = unversionedValue.Data.Double;
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    TInstant value,
    const TRowBufferPtr& /*rowBuffer*/,
    int id,
    EValueFlags flags)
{
    *unversionedValue = MakeUnversionedUint64Value(value.MicroSeconds(), id, flags);
}

void FromUnversionedValue(TInstant* value, TUnversionedValue unversionedValue)
{
    switch (unversionedValue.Type) {
        case EValueType::Int64:
            *value = TInstant::MicroSeconds(CheckedIntegralCast<ui64>(unversionedValue.Data.Int64));
            break;
        case EValueType::Uint64:
            *value = TInstant::MicroSeconds(unversionedValue.Data.Uint64);
            break;
        default:
            THROW_ERROR_EXCEPTION("Cannot parse instant from %Qlv",
                unversionedValue.Type);
    }
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    TDuration value,
    const TRowBufferPtr& /*rowBuffer*/,
    int id,
    EValueFlags flags)
{
    *unversionedValue = MakeUnversionedUint64Value(value.MicroSeconds(), id, flags);
}

void FromUnversionedValue(TDuration* value, TUnversionedValue unversionedValue)
{
    switch (unversionedValue.Type) {
        case EValueType::Int64:
            *value = TDuration::MicroSeconds(CheckedIntegralCast<ui64>(unversionedValue.Data.Int64));
            break;
        case EValueType::Uint64:
            *value = TDuration::MicroSeconds(unversionedValue.Data.Uint64);
            break;
        default:
            THROW_ERROR_EXCEPTION("Cannot parse duration from %Qlv",
                unversionedValue.Type);
    }
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const IMapNodePtr& value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    *unversionedValue = rowBuffer->CaptureValue(MakeUnversionedAnyValue(ConvertToYsonString(value).AsStringBuf(), id, flags));
}

void FromUnversionedValue(IMapNodePtr* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = nullptr;
    }
    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse YSON map from %Qlv",
            unversionedValue.Type);
    }
    *value = ConvertTo<IMapNodePtr>(FromUnversionedValue<TYsonStringBuf>(unversionedValue));
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const TIP6Address& value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    ToUnversionedValue(unversionedValue, ToString(value), rowBuffer, id, flags);
}

void FromUnversionedValue(TIP6Address* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = TIP6Address();
    }
    auto strValue = FromUnversionedValue<TString>(unversionedValue);
    *value = TIP6Address::FromString(strValue);
}

////////////////////////////////////////////////////////////////////////////////

void ToUnversionedValue(
    TUnversionedValue* unversionedValue,
    const TError& value,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    auto errorYson = ConvertToYsonString(value);
    *unversionedValue = rowBuffer->CaptureValue(MakeUnversionedAnyValue(errorYson.AsStringBuf(), id, flags));
}

void FromUnversionedValue(TError* value, TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        *value = {};
    }
    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION(
            "Cannot parse error from value of type %Qlv",
            unversionedValue.Type);
    }
    *value = ConvertTo<TError>(FromUnversionedValue<TYsonStringBuf>(unversionedValue));
}

////////////////////////////////////////////////////////////////////////////////

void ProtobufToUnversionedValueImpl(
    TUnversionedValue* unversionedValue,
    const Message& value,
    const TProtobufMessageType* type,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    auto byteSize = value.ByteSizeLong();
    auto* pool = rowBuffer->GetPool();
    auto* wireBuffer = pool->AllocateUnaligned(byteSize);
    YT_VERIFY(value.SerializePartialToArray(wireBuffer, byteSize));
    ArrayInputStream inputStream(wireBuffer, byteSize);
    TString ysonBytes;
    TStringOutput outputStream(ysonBytes);
    TYsonWriter ysonWriter(&outputStream);
    ParseProtobuf(&ysonWriter, &inputStream, type);
    *unversionedValue = rowBuffer->CaptureValue(MakeUnversionedAnyValue(ysonBytes, id, flags));
}

////////////////////////////////////////////////////////////////////////////////

void UnversionedValueToProtobufImpl(
    Message* value,
    const TProtobufMessageType* type,
    TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        value->Clear();
        return;
    }
    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse a protobuf message from %Qlv",
            unversionedValue.Type);
    }
    TString wireBytes;
    StringOutputStream outputStream(&wireBytes);
    TProtobufWriterOptions options;
    options.UnknownYsonFieldModeResolver = TProtobufWriterOptions::CreateConstantUnknownYsonFieldModeResolver(EUnknownYsonFieldsMode::Keep);
    auto protobufWriter = CreateProtobufWriter(&outputStream, type, options);
    ParseYsonStringBuffer(
        unversionedValue.AsStringBuf(),
        EYsonType::Node,
        protobufWriter.get());
    if (!value->ParseFromArray(wireBytes.data(), wireBytes.size())) {
        THROW_ERROR_EXCEPTION("Error parsing %v from wire bytes",
            value->GetTypeName());
    }
}

////////////////////////////////////////////////////////////////////////////////

void ListToUnversionedValueImpl(
    TUnversionedValue* unversionedValue,
    const std::function<bool(TUnversionedValue*)> producer,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    TString ysonBytes;
    TStringOutput outputStream(ysonBytes);
    NYT::NYson::TYsonWriter writer(&outputStream);
    writer.OnBeginList();

    TUnversionedValue itemValue;
    while (true) {
        writer.OnListItem();
        if (!producer(&itemValue)) {
            break;
        }
        UnversionedValueToYson(itemValue, &writer);
    }
    writer.OnEndList();

    *unversionedValue = rowBuffer->CaptureValue(MakeUnversionedAnyValue(ysonBytes, id, flags));
}

void UnversionedValueToListImpl(
    std::function<google::protobuf::Message*()> appender,
    const TProtobufMessageType* type,
    TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        return;
    }

    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse vector from %Qlv",
            unversionedValue.Type);
    }

    class TConsumer
        : public IYsonConsumer
    {
    public:
        TConsumer(
            std::function<google::protobuf::Message*()> appender,
            const TProtobufMessageType* type)
            : Appender_(std::move(appender))
              , Type_(type)
              , OutputStream_(&WireBytes_)
        { }

        void OnStringScalar(TStringBuf value) override
        {
            GetUnderlying()->OnStringScalar(value);
        }

        void OnInt64Scalar(i64 value) override
        {
            GetUnderlying()->OnInt64Scalar(value);
        }

        void OnUint64Scalar(ui64 value) override
        {
            GetUnderlying()->OnUint64Scalar(value);
        }

        void OnDoubleScalar(double value) override
        {
            GetUnderlying()->OnDoubleScalar(value);
        }

        void OnBooleanScalar(bool value) override
        {
            GetUnderlying()->OnBooleanScalar(value);
        }

        void OnEntity() override
        {
            GetUnderlying()->OnEntity();
        }

        void OnBeginList() override
        {
            if (Depth_ > 0) {
                GetUnderlying()->OnBeginList();
            }
            ++Depth_;
        }

        void OnListItem() override
        {
            if (Depth_ == 1) {
                NextElement();
            } else {
                GetUnderlying()->OnListItem();
            }
        }

        void OnEndList() override
        {
            --Depth_;
            if (Depth_ == 0) {
                FlushElement();
            } else {
                GetUnderlying()->OnEndList();
            }
        }

        void OnBeginMap() override
        {
            ++Depth_;
            GetUnderlying()->OnBeginMap();
        }

        void OnKeyedItem(TStringBuf key) override
        {
            GetUnderlying()->OnKeyedItem(key);
        }

        void OnEndMap() override
        {
            --Depth_;
            GetUnderlying()->OnEndMap();
        }

        void OnBeginAttributes() override
        {
            GetUnderlying()->OnBeginAttributes();
        }

        void OnEndAttributes() override
        {
            GetUnderlying()->OnEndAttributes();
        }

        void OnRaw(TStringBuf yson, EYsonType type) override
        {
            GetUnderlying()->OnRaw(yson, type);
        }

    private:
        const std::function<google::protobuf::Message*()> Appender_;
        const TProtobufMessageType* const Type_;

        std::unique_ptr<IYsonConsumer> Underlying_;
        int Depth_ = 0;

        TString WireBytes_;
        StringOutputStream OutputStream_;


        IYsonConsumer* GetUnderlying()
        {
            if (!Underlying_) {
                THROW_ERROR_EXCEPTION("YSON value must be a list without attributes");
            }
            return Underlying_.get();
        }

        void NextElement()
        {
            FlushElement();
            WireBytes_.clear();
            Underlying_ = CreateProtobufWriter(&OutputStream_, Type_);
        }

        void FlushElement()
        {
            if (!Underlying_) {
                return;
            }
            auto* value = Appender_();
            if (!value->ParseFromArray(WireBytes_.data(), WireBytes_.size())) {
                THROW_ERROR_EXCEPTION("Error parsing %v from wire bytes",
                    value->GetTypeName());
            }
            Underlying_.reset();
        }
    } consumer(std::move(appender), type);

    ParseYsonStringBuffer(
        unversionedValue.AsStringBuf(),
        EYsonType::Node,
        &consumer);
}

void UnversionedValueToListImpl(
    std::function<void(TUnversionedValue)> appender,
    TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        return;
    }

    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse a vector from %Qlv",
            unversionedValue.Type);
    }

    class TConsumer
        : public TYsonConsumerBase
    {
    public:
        explicit TConsumer(std::function<void(TUnversionedValue)> appender)
            : Appender_(std::move(appender))
        { }

        void OnStringScalar(TStringBuf value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedStringValue(value));
        }

        void OnInt64Scalar(i64 value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedInt64Value(value));
        }

        void OnUint64Scalar(ui64 value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedUint64Value(value));
        }

        void OnDoubleScalar(double value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedDoubleValue(value));
        }

        void OnBooleanScalar(bool value) override
        {
            EnsureInList();
            Appender_(MakeUnversionedBooleanValue(value));
        }

        void OnEntity() override
        {
            THROW_ERROR_EXCEPTION("YSON entities are not supported");
        }

        void OnBeginList() override
        {
            EnsureNotInList();
            InList_ = true;
        }

        void OnListItem() override
        { }

        void OnEndList() override
        { }

        void OnBeginMap() override
        {
            THROW_ERROR_EXCEPTION("YSON maps are not supported");
        }

        void OnKeyedItem(TStringBuf /*key*/) override
        {
            YT_ABORT();
        }

        void OnEndMap() override
        {
            YT_ABORT();
        }

        void OnBeginAttributes() override
        {
            THROW_ERROR_EXCEPTION("YSON attributes are not supported");
        }

        void OnEndAttributes() override
        {
            YT_ABORT();
        }

    private:
        const std::function<void(TUnversionedValue)> Appender_;

        bool InList_ = false;

        void EnsureInList()
        {
            if (!InList_) {
                THROW_ERROR_EXCEPTION("YSON list expected");
            }
        }

        void EnsureNotInList()
        {
            if (InList_) {
                THROW_ERROR_EXCEPTION("YSON list is unexpected");
            }
        }
    } consumer(std::move(appender));

    ParseYsonStringBuffer(
        unversionedValue.AsStringBuf(),
        EYsonType::Node,
        &consumer);
}

////////////////////////////////////////////////////////////////////////////////

void MapToUnversionedValueImpl(
    TUnversionedValue* unversionedValue,
    const std::function<bool(TString*, TUnversionedValue*)> producer,
    const TRowBufferPtr& rowBuffer,
    int id,
    EValueFlags flags)
{
    TString ysonBytes;
    TStringOutput outputStream(ysonBytes);
    NYT::NYson::TYsonWriter writer(&outputStream);
    writer.OnBeginMap();

    TString itemKey;
    TUnversionedValue itemValue;
    while (true) {
        if (!producer(&itemKey, &itemValue)) {
            break;
        }
        writer.OnKeyedItem(itemKey);
        UnversionedValueToYson(itemValue, &writer);
    }
    writer.OnEndMap();

    *unversionedValue = rowBuffer->CaptureValue(MakeUnversionedAnyValue(ysonBytes, id, flags));
}

void UnversionedValueToMapImpl(
    std::function<google::protobuf::Message*(TString)> appender,
    const TProtobufMessageType* type,
    TUnversionedValue unversionedValue)
{
    if (unversionedValue.Type == EValueType::Null) {
        return;
    }

    if (unversionedValue.Type != EValueType::Any) {
        THROW_ERROR_EXCEPTION("Cannot parse map from %Qlv",
            unversionedValue.Type);
    }

    class TConsumer
        : public IYsonConsumer
    {
    public:
        TConsumer(
            std::function<google::protobuf::Message*(TString)> appender,
            const TProtobufMessageType* type)
            : Appender_(std::move(appender))
            , Type_(type)
            , OutputStream_(&WireBytes_)
        { }

        void OnStringScalar(TStringBuf value) override
        {
            GetUnderlying()->OnStringScalar(value);
        }

        void OnInt64Scalar(i64 value) override
        {
            GetUnderlying()->OnInt64Scalar(value);
        }

        void OnUint64Scalar(ui64 value) override
        {
            GetUnderlying()->OnUint64Scalar(value);
        }

        void OnDoubleScalar(double value) override
        {
            GetUnderlying()->OnDoubleScalar(value);
        }

        void OnBooleanScalar(bool value) override
        {
            GetUnderlying()->OnBooleanScalar(value);
        }

        void OnEntity() override
        {
            GetUnderlying()->OnEntity();
        }

        void OnBeginList() override
        {
            ++Depth_;
            GetUnderlying()->OnBeginList();
        }

        void OnListItem() override
        {
            GetUnderlying()->OnListItem();
        }

        void OnEndList() override
        {
            --Depth_;
            GetUnderlying()->OnEndList();
        }

        void OnBeginMap() override
        {
            if (Depth_ > 0) {
                GetUnderlying()->OnBeginMap();
            }
            ++Depth_;
        }

        void OnKeyedItem(TStringBuf key) override
        {
            if (Depth_ == 1) {
                NextElement(key);
            } else {
                GetUnderlying()->OnKeyedItem(key);
            }
        }

        void OnEndMap() override
        {
            --Depth_;
            if (Depth_ == 0) {
                FlushElement();
            } else {
                GetUnderlying()->OnEndMap();
            }
        }

        void OnBeginAttributes() override
        {
            GetUnderlying()->OnBeginAttributes();
        }

        void OnEndAttributes() override
        {
            GetUnderlying()->OnEndAttributes();
        }

        void OnRaw(TStringBuf yson, EYsonType type) override
        {
            GetUnderlying()->OnRaw(yson, type);
        }

    private:
        const std::function<google::protobuf::Message*(TString)> Appender_;
        const TProtobufMessageType* const Type_;

        std::optional<TString> Key_;
        std::unique_ptr<IYsonConsumer> Underlying_;
        int Depth_ = 0;

        TString WireBytes_;
        StringOutputStream OutputStream_;


        IYsonConsumer* GetUnderlying()
        {
            if (!Underlying_) {
                THROW_ERROR_EXCEPTION("YSON value must be a list without attributes");
            }
            return Underlying_.get();
        }

        void NextElement(TStringBuf key)
        {
            FlushElement();
            WireBytes_.clear();
            Key_ = TString(key);
            Underlying_ = CreateProtobufWriter(&OutputStream_, Type_);
        }

        void FlushElement()
        {
            if (!Underlying_) {
                return;
            }
            auto* value = Appender_(*Key_);
            if (!value->ParseFromArray(WireBytes_.data(), WireBytes_.size())) {
                THROW_ERROR_EXCEPTION("Error parsing %v from wire bytes",
                    value->GetTypeName());
            }
            Underlying_.reset();
            Key_.reset();
        }
    } consumer(std::move(appender), type);

    ParseYsonStringBuffer(
        unversionedValue.AsStringBuf(),
        EYsonType::Node,
        &consumer);
}

////////////////////////////////////////////////////////////////////////////////

void UnversionedValueToYson(TUnversionedValue unversionedValue, TCheckedInDebugYsonTokenWriter* tokenWriter)
{
    switch (unversionedValue.Type) {
        case EValueType::Int64:
            tokenWriter->WriteBinaryInt64(unversionedValue.Data.Int64);
            return;
        case EValueType::Uint64:
            tokenWriter->WriteBinaryUint64(unversionedValue.Data.Uint64);
            return;
        case EValueType::Double:
            tokenWriter->WriteBinaryDouble(unversionedValue.Data.Double);
            return;
        case EValueType::String:
            tokenWriter->WriteBinaryString(unversionedValue.AsStringBuf());
            return;
        case EValueType::Any:
        case EValueType::Composite:
            tokenWriter->WriteRawNodeUnchecked(unversionedValue.AsStringBuf());
            return;
        case EValueType::Boolean:
            tokenWriter->WriteBinaryBoolean(unversionedValue.Data.Boolean);
            return;
        case EValueType::Null:
            tokenWriter->WriteEntity();
            return;
        case EValueType::TheBottom:
        case EValueType::Min:
        case EValueType::Max:
            YT_ABORT();
    }
    ThrowUnexpectedValueType(unversionedValue.Type);
}

void UnversionedValueToYson(TUnversionedValue unversionedValue, IYsonConsumer* consumer)
{
    switch (unversionedValue.Type) {
        case EValueType::Int64:
            consumer->OnInt64Scalar(unversionedValue.Data.Int64);
            return;
        case EValueType::Uint64:
            consumer->OnUint64Scalar(unversionedValue.Data.Uint64);
            return;
        case EValueType::Double:
            consumer->OnDoubleScalar(unversionedValue.Data.Double);
            return;
        case EValueType::String:
            consumer->OnStringScalar(unversionedValue.AsStringBuf());
            return;
        case EValueType::Any:
        case EValueType::Composite:
            consumer->OnRaw(unversionedValue.AsStringBuf(), EYsonType::Node);
            return;
        case EValueType::Boolean:
            consumer->OnBooleanScalar(unversionedValue.Data.Boolean);
            return;
        case EValueType::Null:
            consumer->OnEntity();
            return;
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            YT_ABORT();
    }
    ThrowUnexpectedValueType(unversionedValue.Type);
}

TYsonString UnversionedValueToYson(TUnversionedValue unversionedValue, bool enableRaw)
{
    TString data;
    data.reserve(GetYsonSize(unversionedValue));
    TStringOutput output(data);
    TYsonWriter writer(&output, EYsonFormat::Binary, EYsonType::Node, /* enableRaw */ enableRaw);
    UnversionedValueToYson(unversionedValue, &writer);
    return TYsonString(std::move(data));
}

////////////////////////////////////////////////////////////////////////////////

void ToAny(
    TRowBuffer* rowBuffer,
    TUnversionedValue* result,
    TUnversionedValue* value,
    EYsonFormat format)
{
    TStringStream stream;
    NYson::TYsonWriter writer(&stream, format);

    switch (value->Type) {
        case EValueType::Null:
            *result = MakeUnversionedNullValue();
            return;
        case EValueType::Any:
        case EValueType::Composite: {
            if (format == EYsonFormat::Binary) {
                *result = *value;
                return;
            } else {
                writer.OnRaw(value->AsStringBuf());
            }
            break;
        }
        case EValueType::String: {
            writer.OnStringScalar(value->AsStringBuf());
            break;
        }
        case EValueType::Int64: {
            writer.OnInt64Scalar(value->Data.Int64);
            break;
        }
        case EValueType::Uint64: {
            writer.OnUint64Scalar(value->Data.Uint64);
            break;
        }
        case EValueType::Double: {
            writer.OnDoubleScalar(value->Data.Double);
            break;
        }
        case EValueType::Boolean: {
            writer.OnBooleanScalar(value->Data.Boolean);
            break;
        }
        case EValueType::Min:
        case EValueType::Max:
        case EValueType::TheBottom:
            YT_ABORT();
    }

    writer.Flush();

    *result = rowBuffer->CaptureValue(
        MakeUnversionedAnyValue(TStringBuf(stream.Data(), stream.Size())));
}

////////////////////////////////////////////////////////////////////////////////

struct TDefaultUnversionedRowsBuilderTag
{ };

TUnversionedRowsBuilder::TUnversionedRowsBuilder()
    : TUnversionedRowsBuilder(New<TRowBuffer>(TDefaultUnversionedRowsBuilderTag()))
{ }

TUnversionedRowsBuilder::TUnversionedRowsBuilder(TRowBufferPtr rowBuffer)
    : RowBuffer_(std::move(rowBuffer))
{ }

void TUnversionedRowsBuilder::ReserveRows(int rowCount)
{
    Rows_.reserve(rowCount);
}

void TUnversionedRowsBuilder::AddRow(TUnversionedRow row)
{
    Rows_.push_back(RowBuffer_->CaptureRow(row));
}

void TUnversionedRowsBuilder::AddRow(TMutableUnversionedRow row)
{
    AddRow(TUnversionedRow(row));
}

void TUnversionedRowsBuilder::AddProtoRow(const TString& protoRow)
{
    auto& row = Rows_.emplace_back();
    FromProto(&row, protoRow, RowBuffer_);
}

TSharedRange<TUnversionedRow> TUnversionedRowsBuilder::Build()
{
    return MakeSharedRange(std::move(Rows_), std::move(RowBuffer_));
}

////////////////////////////////////////////////////////////////////////////////

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(
    NProto::TDataBlockMeta,
    /*last_key*/ 9,
    TUnversionedOwningRow)

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(
    NProto::TBoundaryKeysExt,
    /*min*/ 1,
    TUnversionedOwningRow)
REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(
    NProto::TBoundaryKeysExt,
    /*max*/ 2,
    TUnversionedOwningRow)

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(
    NProto::TSamplesExt,
    /*entries*/ 1,
    TUnversionedOwningRow)

REGISTER_INTERMEDIATE_PROTO_INTEROP_BYTES_FIELD_REPRESENTATION(
    NProto::THeavyColumnStatisticsExt,
    /*column_data_weights*/ 5,
    TUnversionedOwningRow)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTableClient
