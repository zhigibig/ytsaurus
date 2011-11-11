#include "stdafx.h"
#include "json_adapter.h"

#include <library/json/json_writer.h>

namespace NYT {
namespace NYTree {

using NJson::TJsonWriter;

////////////////////////////////////////////////////////////////////////////////

TJsonAdapter::TJsonAdapter(TOutputStream* output)
    : JsonWriter(new TJsonWriter(output, true))
{ }

void TJsonAdapter::OnStringScalar(const Stroka& value, bool hasAttributes)
{
    YASSERT(!hasAttributes);
    JsonWriter->Write(value);
}

void TJsonAdapter::OnInt64Scalar(i64 value, bool hasAttributes)
{
    YASSERT(!hasAttributes);
    JsonWriter->Write(value);
}

void TJsonAdapter::OnDoubleScalar(double value, bool hasAttributes)
{
    YASSERT(!hasAttributes);
    JsonWriter->Write(value);
}

void TJsonAdapter::OnEntity(bool hasAttributes)
{
    UNUSED(hasAttributes);
    YUNREACHABLE();
}

void TJsonAdapter::OnBeginList()
{
    JsonWriter->OpenArray();
}

void TJsonAdapter::OnListItem()
{ }

void TJsonAdapter::OnEndList(bool hasAttributes)
{
    YASSERT(!hasAttributes);
    JsonWriter->CloseArray();
}

void TJsonAdapter::OnBeginMap()
{
    JsonWriter->OpenMap();
}

void TJsonAdapter::OnMapItem(const Stroka& name)
{
    JsonWriter->Write(name);
}

void TJsonAdapter::OnEndMap(bool hasAttributes)
{
    YASSERT(!hasAttributes);
    JsonWriter->CloseMap();
}

void TJsonAdapter::OnBeginAttributes()
{
    YUNREACHABLE();
}

void TJsonAdapter::OnAttributesItem(const Stroka& name)
{
    UNUSED(name);
    YUNREACHABLE();
}

void TJsonAdapter::OnEndAttributes()
{
    YUNREACHABLE();
}

void TJsonAdapter::Flush()
{
    JsonWriter->Flush();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
