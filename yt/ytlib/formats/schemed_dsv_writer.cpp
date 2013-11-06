#include "stdafx.h"
#include "schemed_dsv_writer.h"

#include <ytlib/misc/error.h>

#include <ytlib/yson/format.h>

namespace NYT {
namespace NFormats {

using namespace NYTree;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

TSchemedDsvWriter::TSchemedDsvWriter(
        TOutputStream* stream,
        TSchemedDsvFormatConfigPtr config)
    : Stream_(stream)
    , Config_(config)
    , Table_(Config_)
    , Keys_(Config_->Columns.begin(), Config_->Columns.end())
    , ValueCount_(0)
    , TableIndex_(0)
    , State_(EState::None)
{ }

void TSchemedDsvWriter::OnDoubleScalar(double value)
{
    THROW_ERROR_EXCEPTION("Double values are not supported by schemed DSV");
}

void TSchemedDsvWriter::OnBeginList()
{
    THROW_ERROR_EXCEPTION("Lists are not supported by schemed DSV");
}

void TSchemedDsvWriter::OnListItem()
{
    YASSERT(State_ == EState::None);
}

void TSchemedDsvWriter::OnEndList()
{
    YUNREACHABLE();
}

void TSchemedDsvWriter::OnBeginAttributes()
{
    if (State_ == EState::ExpectValue) {
        THROW_ERROR_EXCEPTION("Attributes are not supported by schemed DSV");
    }

    YASSERT(State_ == EState::None);
    State_ = EState::ExpectAttributeName;
}

void TSchemedDsvWriter::OnEndAttributes()
{
    YASSERT(State_ == EState::ExpectEndAttributes);
    State_ = EState::ExpectEntity;
}

void TSchemedDsvWriter::OnBeginMap()
{
    if (State_ == EState::ExpectValue) {
        THROW_ERROR_EXCEPTION("Embedded maps are not supported by schemed DSV");
    }
    YASSERT(State_ == EState::None);
}

void TSchemedDsvWriter::OnEntity()
{
    if (State_ == EState::ExpectValue) {
        THROW_ERROR_EXCEPTION("Entities are not supported by schemed DSV");
    }

    YASSERT(State_ == EState::ExpectEntity);
    State_ = EState::None;
}

void TSchemedDsvWriter::OnIntegerScalar(i64 value)
{
    if (State_ == EState::ExpectValue) {
        THROW_ERROR_EXCEPTION("Integer values are not supported by schemed DSV");
    }
    YASSERT(State_ == EState::ExpectAttributeValue);

    switch (ControlAttribute_) {
    case EControlAttribute::TableIndex:
        TableIndex_ = value;
        break;

    default:
        YUNREACHABLE();
    }

    State_ = EState::ExpectEndAttributes;
}

void TSchemedDsvWriter::OnStringScalar(const TStringBuf& value)
{
    if (State_ == EState::ExpectValue) {
        Values_[CurrentKey_] = value;
        State_ = EState::None;
        ValueCount_ += 1;
    } else {
        YCHECK(State_ == EState::None);
    }
}

void TSchemedDsvWriter::OnKeyedItem(const TStringBuf& key)
{
    if (State_ ==  EState::ExpectAttributeName) {
        ControlAttribute_ = ParseEnum<EControlAttribute>(ToString(key));
        State_ = EState::ExpectAttributeValue;
    } else {
        YASSERT(State_ == EState::None);
        if (Keys_.find(key) != Keys_.end()) {
            CurrentKey_ = key;
            State_ = EState::ExpectValue;
        }
    }
}

void TSchemedDsvWriter::OnEndMap()
{
    YASSERT(State_ == EState::None);

    WriteRow();
}

void TSchemedDsvWriter::WriteRow()
{
    if (ValueCount_ == Keys_.size()) {
        if (Config_->EnableTableIndex) {
            Stream_->Write(ToString(TableIndex_));
            Stream_->Write(Config_->FieldSeparator);
        }
        for (int i = 0; i < Keys_.size(); ++i) {
            auto key = Config_->Columns[i];
            EscapeAndWrite(Values_[key]);
            Stream_->Write(
                i + 1 < Keys_.size()
                ? Config_->FieldSeparator
                : Config_->RecordSeparator);
        }
    }
    ValueCount_ = 0;
}

void TSchemedDsvWriter::EscapeAndWrite(const TStringBuf& value) const
{
    if (Config_->EnableEscaping) {
        WriteEscaped(
            Stream_,
            value,
            Table_.Stops,
            Table_.Escapes,
            Config_->EscapingSymbol);
    } else {
        Stream_->Write(value);
    }
}

TSchemedDsvWriter::~TSchemedDsvWriter()
{ }

////////////////////////////////////////////////////////////////////////////////

} // namespace NFormats
} // namespace NYT
