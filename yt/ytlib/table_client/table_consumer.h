﻿#pragma once

#include "public.h"

#include <core/misc/blob_output.h>
#include <core/misc/blob_range.h>
#include <core/misc/nullable.h>

#include <core/yson/consumer.h>
#include <core/yson/writer.h>

#include <ytlib/new_table_client/writer.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

/*!
 *  For performance reasons we don't use TForwardingYsonConsumer.
 */
class TTableConsumer
    : public NYson::IYsonConsumer
{
public:
    template <class TWriter>
    explicit TTableConsumer(TWriter writer)
        : ControlState(EControlState::None)
        , CurrentTableIndex(0)
        , Writer(writer)
        , Depth(0)
        , ValueWriter(&RowBuffer)
    {
        Writers.push_back(writer);
    }

    template <class TWriter>
    TTableConsumer(const std::vector<TWriter>& writers, int tableIndex)
        : ControlState(EControlState::None)
        , CurrentTableIndex(tableIndex)
        , Writers(writers.begin(), writers.end())
        , Writer(Writers[CurrentTableIndex])
        , Depth(0)
        , ValueWriter(&RowBuffer)
    { }

    virtual void OnKeyedItem(const TStringBuf& name) override;
    virtual void OnStringScalar(const TStringBuf& value) override;
    virtual void OnIntegerScalar(i64 value) override;
    virtual void OnDoubleScalar(double value) override;
    virtual void OnEntity() override;
    virtual void OnBeginList() override;
    virtual void OnListItem() override;
    virtual void OnEndList() override;
    virtual void OnBeginMap() override;
    virtual void OnEndMap() override;
    virtual void OnBeginAttributes() override;
    virtual void OnEndAttributes() override;
    virtual void OnRaw(const TStringBuf& yson, NYson::EYsonType type) override;

private:
    void ThrowError(const Stroka& message) const;
    void ThrowMapExpected() const;
    void ThrowEntityExpected() const;
    void ThrowInvalidControlAttribute(const Stroka& whatsWrong) const;

    DECLARE_ENUM(EControlState,
        (None)
        (ExpectControlAttributeName)
        (ExpectControlAttributeValue)
        (ExpectEndControlAttributes)
        (ExpectEntity)
    );

    EControlState ControlState;
    EControlAttribute ControlAttribute;

    int CurrentTableIndex;
    std::vector<IWriterBasePtr> Writers;
    IWriterBasePtr Writer;

    int Depth;

    //! Keeps the current row data.
    TBlobOutput RowBuffer;

    //! |(endColumn, endValue)| offsets in #RowBuffer.
    std::vector<size_t> Offsets;

    NYson::TYsonWriter ValueWriter;

};

////////////////////////////////////////////////////////////////////////////////

// TODO(babenko): should eventually merge with the above
/*!
 *  For performance reasons we don't use TForwardingYsonConsumer.
 */
class TVersionedTableConsumer
    : public NYson::IYsonConsumer
{
public:
    TVersionedTableConsumer(
        const NVersionedTableClient::TTableSchema& schema,
        const NVersionedTableClient::TKeyColumns& keyColumns,
        NVersionedTableClient::TNameTablePtr nameTable,
        NVersionedTableClient::IWriterPtr writer);

    TVersionedTableConsumer(
        const NVersionedTableClient::TTableSchema& schema,
        const NVersionedTableClient::TKeyColumns& keyColumns,
        NVersionedTableClient::TNameTablePtr nameTable,
        std::vector<NVersionedTableClient::IWriterPtr> writers,
        int tableIndex);

private:
    void Initialize(
        const NVersionedTableClient::TTableSchema& schema,
        const NVersionedTableClient::TKeyColumns& keyColumns,
        NVersionedTableClient::TNameTablePtr nameTable);

    virtual void OnStringScalar(const TStringBuf& value) override;
    virtual void OnIntegerScalar(i64 value) override;
    virtual void OnDoubleScalar(double value) override;
    virtual void OnEntity() override;
    virtual void OnBeginList() override;
    virtual void OnListItem() override;
    virtual void OnBeginMap() override;
    virtual void OnKeyedItem(const TStringBuf& name) override;
    virtual void OnEndMap() override;

    virtual void OnBeginAttributes() override;

    void ThrowMapExpected();
    void ThrowInvalidSchemaColumnType(int columnId, NVersionedTableClient::EColumnType actualType);
    void ThrowInvalidControlAttribute(const Stroka& whatsWrong);

    virtual void OnEndList() override;
    virtual void OnEndAttributes() override;
    virtual void OnRaw(const TStringBuf& yson, NYson::EYsonType type) override;

    void WriteValue(const NVersionedTableClient::TRowValue& rowValue);

    DECLARE_ENUM(EControlState,
        (None)
        (ExpectName)
        (ExpectValue)
        (ExpectEndAttributes)
        (ExpectEntity)
    );

    NVersionedTableClient::TNameTablePtr NameTable;

    EControlState ControlState;
    EControlAttribute ControlAttribute;

    int CurrentTableIndex;
    std::vector<NVersionedTableClient::IWriterPtr> Writers;
    NVersionedTableClient::IWriterPtr CurrentWriter;

    int Depth;
    int ColumnIndex;

    struct TColumnDescriptor
    {
        TColumnDescriptor()
            : Written(false)
        { }

        bool Written;
        NVersionedTableClient::EColumnType Type;
    };

    std::vector<TColumnDescriptor> SchemaColumnDescriptors;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
