#include "json_writer.h"

#include <library/json/json_writer.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

static bool IsSpecialJsonKey(const TStringBuf& key)
{
    return key.size() > 0 && key[0] == '$';
}

////////////////////////////////////////////////////////////////////////////////

TJsonWriter::TJsonWriter(
    TOutputStream* output,
    EYsonType type,
    EJsonFormat format,
    EJsonAttributesMode attributesMode)
    : Output(output)
    , Type(type)
    , Format(format)
    , AttributesMode(attributesMode)
    , Depth(0)
{
    if (Type == YT_MAP_FRAGMENT) {
        ythrow TYsonException() << ("Map fragments are not supported by Json");
    }

    UnderlyingJsonWriter.Reset(new NJson::TJsonWriter(
        output,
        Format == JF_PRETTY));
    JsonWriter = UnderlyingJsonWriter.Get();
    HasAttributes = false;
    InAttributesBalance = 0;
}

void TJsonWriter::EnterNode()
{
    if (AttributesMode == JAM_NEVER) {
        HasAttributes = false;
    } else if (AttributesMode == JAM_ON_DEMAND) {
        // Do nothing
    } else if (AttributesMode == JAM_ALWAYS) {
        if (!HasAttributes) {
            JsonWriter->OpenMap();
            JsonWriter->Write("$attributes");
            JsonWriter->OpenMap();
            JsonWriter->CloseMap();
        }
        HasAttributes = true;
    }
    HasUnfoldedStructureStack.push_back(HasAttributes);

    if (HasAttributes) {
        JsonWriter->Write("$value");
        HasAttributes = false;
    }

    Depth += 1;
}

void TJsonWriter::LeaveNode()
{
    YASSERT(!HasUnfoldedStructureStack.empty());
    if (HasUnfoldedStructureStack.back()) {
        // Close map of the {$attributes, $value}
        JsonWriter->CloseMap();
    }
    HasUnfoldedStructureStack.pop_back();

    Depth -= 1;

    if (Depth == 0 && Type == YT_LIST_FRAGMENT && InAttributesBalance == 0) {
        JsonWriter->Flush();
        Output->Write("\n");
    }
}

bool TJsonWriter::IsWriteAllowed()
{
    if (AttributesMode == JAM_NEVER) {
        return InAttributesBalance == 0;
    }
    return true;
}

void TJsonWriter::OnStringScalar(const TStringBuf& value)
{
    if (IsWriteAllowed()) {
        EnterNode();
        WriteStringScalar(value);
        LeaveNode();
    }
}

void TJsonWriter::OnInt64Scalar(i64 value)
{
    if (IsWriteAllowed()) {
        EnterNode();
        JsonWriter->Write(value);
        LeaveNode();
    }
}

void TJsonWriter::OnUint64Scalar(ui64 value)
{
    if (IsWriteAllowed()) {
        EnterNode();
        JsonWriter->Write(value);
        LeaveNode();
    }
}

void TJsonWriter::OnDoubleScalar(double value)
{
    if (IsWriteAllowed()) {
        EnterNode();
        JsonWriter->Write(value);
        LeaveNode();
    }
}

void TJsonWriter::OnBooleanScalar(bool value)
{
    if (IsWriteAllowed()) {
        OnStringScalar(value ? "true" : "false");
    }
}

void TJsonWriter::OnEntity()
{
    if (IsWriteAllowed()) {
        EnterNode();
        JsonWriter->WriteNull();
        LeaveNode();
    }
}

void TJsonWriter::OnBeginList()
{
    if (IsWriteAllowed()) {
        EnterNode();
        JsonWriter->OpenArray();
    }
}

void TJsonWriter::OnListItem()
{ }

void TJsonWriter::OnEndList()
{
    if (IsWriteAllowed()) {
        JsonWriter->CloseArray();
        LeaveNode();
    }
}

void TJsonWriter::OnBeginMap()
{
    if (IsWriteAllowed()) {
        EnterNode();
        JsonWriter->OpenMap();
    }
}

void TJsonWriter::OnKeyedItem(const TStringBuf& name)
{
    if (IsWriteAllowed()) {
        if (IsSpecialJsonKey(name)) {
            WriteStringScalar(Stroka("$") + name);
        } else {
            WriteStringScalar(name);
        }
    }
}

void TJsonWriter::OnEndMap()
{
    if (IsWriteAllowed()) {
        JsonWriter->CloseMap();
        LeaveNode();
    }
}

void TJsonWriter::OnBeginAttributes()
{
    InAttributesBalance += 1;
    if (AttributesMode != JAM_NEVER) {
        JsonWriter->OpenMap();
        JsonWriter->Write("$attributes");
        JsonWriter->OpenMap();
    }
}

void TJsonWriter::OnEndAttributes()
{
    InAttributesBalance -= 1;
    if (AttributesMode != JAM_NEVER) {
        HasAttributes = true;
        JsonWriter->CloseMap();
    }
}

void TJsonWriter::WriteStringScalar(const TStringBuf &value)
{
    JsonWriter->Write(value);
}

void TJsonWriter::Flush()
{
    JsonWriter->Flush();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
