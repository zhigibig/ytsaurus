#include "rowset.h"

#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/row_buffer.h>
#include <yt/client/table_client/schema.h>
#include <yt/client/table_client/unversioned_writer.h>
#include <yt/client/table_client/unversioned_row.h>

#include <yt/client/table_client/wire_protocol.h>

#include <yt/core/actions/future.h>

namespace NYT::NApi {

using namespace NTabletClient;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

template <class TRow>
class TRowsetBase
    : public IRowset<TRow>
{
public:
    explicit TRowsetBase(TTableSchema schema)
        : Schema_(std::move(schema))
        , NameTableInitialized_(false)
    { }

    explicit TRowsetBase(TNameTablePtr nameTable)
        : NameTableInitialized_(true)
        , NameTable_(std::move(nameTable))
    { }

    virtual const TTableSchema& GetSchema() const override
    {
        return Schema_;
    }

    virtual const TNameTablePtr& GetNameTable() const override
    {
        // Fast path.
        if (NameTableInitialized_.load()) {
            return NameTable_;
        }

        // Slow path.
        auto guard = Guard(NameTableLock_);
        if (!NameTable_) {
            NameTable_ = TNameTable::FromSchema(Schema_);
        }
        NameTableInitialized_ = true;
        return NameTable_;
    }

private:
    const TTableSchema Schema_;
    
    mutable std::atomic<bool> NameTableInitialized_;
    mutable TSpinLock NameTableLock_;
    mutable TNameTablePtr NameTable_;
};

////////////////////////////////////////////////////////////////////////////////

template <class TRow>
class TRowset
    : public TRowsetBase<TRow>
{
public:
    TRowset(
        TTableSchema schema,
        TSharedRange<TRow> rows)
        : TRowsetBase<TRow>(std::move(schema))
        , Rows_(std::move(rows))
    { }

    TRowset(
        TNameTablePtr nameTable,
        TSharedRange<TRow> rows)
        : TRowsetBase<TRow>(std::move(nameTable))
        , Rows_(std::move(rows))
    { }

    virtual TRange<TRow> GetRows() const override
    {
        return Rows_;
    }

private:
    const TSharedRange<TRow> Rows_;
};

DEFINE_REFCOUNTED_TYPE(TRowset<TUnversionedRow>)
DEFINE_REFCOUNTED_TYPE(TRowset<TVersionedRow>)

template <class TRow>
IRowsetPtr<TRow> CreateRowset(
    NTableClient::TTableSchema schema,
    TSharedRange<TRow> rows)
{
    return New<TRowset<TRow>>(std::move(schema), std::move(rows));
}

template
IUnversionedRowsetPtr CreateRowset<TUnversionedRow>(
    NTableClient::TTableSchema schema,
    TSharedRange<TUnversionedRow> rows);
template
IVersionedRowsetPtr CreateRowset<TVersionedRow>(
    NTableClient::TTableSchema schema,
    TSharedRange<TVersionedRow> rows);

template <class TRow>
IRowsetPtr<TRow> CreateRowset(
    TNameTablePtr nameTable,
    TSharedRange<TRow> rows)
{
    return New<TRowset<TRow>>(std::move(nameTable), std::move(rows));
}

template
IUnversionedRowsetPtr CreateRowset<TUnversionedRow>(
    TNameTablePtr nameTable,
    TSharedRange<TUnversionedRow> rows);
template
IVersionedRowsetPtr CreateRowset<TVersionedRow>(
    TNameTablePtr nameTable,
    TSharedRange<TVersionedRow> rows);

////////////////////////////////////////////////////////////////////////////////

class TSchemafulRowsetWriter
    : public TRowsetBase<TUnversionedRow>
    , public IUnversionedRowsetWriter
{
public:
    using TRowsetBase::TRowsetBase;

    virtual TRange<TUnversionedRow> GetRows() const override
    {
        return MakeRange(Rows_);
    }

    TFuture<IUnversionedRowsetPtr> GetResult() const
    {
        return Result_.ToFuture();
    }

    virtual TFuture<void> Close() override
    {
        Result_.Set(IUnversionedRowsetPtr(this));
        Result_.Reset();
        return VoidFuture;
    }

    virtual bool Write(TRange<TUnversionedRow> rows) override
    {
        for (auto row : rows) {
            Rows_.push_back(RowBuffer_->Capture(row));
        }
        return true;
    }

    virtual TFuture<void> GetReadyEvent() override
    {
        return VoidFuture;
    }

private:
    const TTableSchema Schema_;
    const TNameTablePtr NameTable_;

    TPromise<IUnversionedRowsetPtr> Result_ = NewPromise<IUnversionedRowsetPtr>();

    struct TSchemafulRowsetWriterBufferTag
    { };

    const TRowBufferPtr RowBuffer_ = New<TRowBuffer>(TSchemafulRowsetWriterBufferTag());
    std::vector<TUnversionedRow> Rows_;

};

std::tuple<IUnversionedRowsetWriterPtr, TFuture<IUnversionedRowsetPtr>> CreateSchemafulRowsetWriter(const TTableSchema& schema)
{
    auto writer = New<TSchemafulRowsetWriter>(schema);
    return std::make_tuple(writer, writer->GetResult());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi

