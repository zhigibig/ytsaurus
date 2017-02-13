#pragma once

#include "public.h"

#include <yt/ytlib/api/public.h>

#include <yt/ytlib/table_client/schemaful_reader.h>
#include <yt/ytlib/table_client/schemaful_writer.h>

#include <yt/core/misc/enum.h>
#include <yt/core/misc/range.h>
#include <yt/core/misc/ref.h>

#include <yt/core/compression/public.h>

#include <yt/core/logging/log.h>

namespace NYT {
namespace NTabletClient {

///////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EWireProtocolCommand,
    // Read commands:

    ((LookupRows)(1))
    // Finds rows with given keys and fetches their components.
    //
    // Input:
    //   * TReqLookupRows
    //   * Unversioned rowset containing N keys
    //
    // Output:
    //   * N unversioned rows


    // Write commands:

    ((WriteRow)(100))
    // Inserts a new row or completely replaces an existing one with matching key.
    //
    // Input:
    //   * Unversioned row
    // Output:
    //   None

    ((DeleteRow)(101))
    // Deletes a row with a given key, if it exists.
    //
    // Input:
    //   * Key
    // Output:
    //   None
);

////////////////////////////////////////////////////////////////////////////////

//! Builds wire-encoded stream.
class TWireProtocolWriter
    : private TNonCopyable
{
public:
    TWireProtocolWriter();
    ~TWireProtocolWriter();

    size_t GetByteSize() const;

    void WriteCommand(EWireProtocolCommand command);

    void WriteTableSchema(const NTableClient::TTableSchema& schema);

    void WriteMessage(const ::google::protobuf::MessageLite& message);

    void WriteUnversionedRow(
        NTableClient::TUnversionedRow row,
        const NTableClient::TNameTableToSchemaIdMapping* idMapping = nullptr);
    void WriteUnversionedRow(
        const TRange<NTableClient::TUnversionedValue>& row,
        const NTableClient::TNameTableToSchemaIdMapping* idMapping = nullptr);
    void WriteSchemafulRow(
        NTableClient::TUnversionedRow row,
        const NTableClient::TNameTableToSchemaIdMapping* idMapping = nullptr);

    void WriteUnversionedRowset(
        const TRange<NTableClient::TUnversionedRow>& rowset,
        const NTableClient::TNameTableToSchemaIdMapping* idMapping = nullptr);
    void WriteSchemafulRowset(
        const TRange<NTableClient::TUnversionedRow>& rowset,
        const NTableClient::TNameTableToSchemaIdMapping* idMapping = nullptr);

    std::vector<TSharedRef> Finish();

private:
    class TImpl;
    const std::unique_ptr<TImpl> Impl_;

};

///////////////////////////////////////////////////////////////////////////////

//! Reads wire-encoded stream.
/*!
 *  All |ReadXXX| methods obey the following convention.
 *  Rows are captured by the row buffer passed in ctor.
 *  Values are either captured or not depending on |deep| argument.
 */
class TWireProtocolReader
    : private TNonCopyable
{
public:
    using TIterator = const char*;
    using TSchemaData = std::vector<ui32>;

    //! Initializes the instance.
    /*!
     *  If #rowBuffer is null, a default one is created.
     */
    TWireProtocolReader(
        const TSharedRef& data,
        NTableClient::TRowBufferPtr rowBuffer = NTableClient::TRowBufferPtr());
    ~TWireProtocolReader();

    const NTableClient::TRowBufferPtr& GetRowBuffer() const;

    bool IsFinished() const;
    TIterator GetBegin() const;
    TIterator GetEnd() const;

    TIterator GetCurrent() const;
    void SetCurrent(TIterator);

    TSharedRef Slice(TIterator begin, TIterator end);

    EWireProtocolCommand ReadCommand();

    NTableClient::TTableSchema ReadTableSchema();

    void ReadMessage(::google::protobuf::MessageLite* message);

    NTableClient::TUnversionedRow ReadUnversionedRow(bool deep);
    NTableClient::TUnversionedRow ReadSchemafulRow(const TSchemaData& schemaData, bool deep);

    TSharedRange<NTableClient::TUnversionedRow> ReadUnversionedRowset(bool deep);
    TSharedRange<NTableClient::TUnversionedRow> ReadSchemafulRowset(const TSchemaData& schemaData, bool deep);

    static TSchemaData GetSchemaData(
        const NTableClient::TTableSchema& schema,
        const NTableClient::TColumnFilter& filter);
    static TSchemaData GetSchemaData(const NTableClient::TTableSchema& schema);

private:
    class TImpl;
    const std::unique_ptr<TImpl> Impl_;

};

///////////////////////////////////////////////////////////////////////////////

struct IWireProtocolRowsetReader
    : public NTableClient::ISchemafulReader
{ };

DEFINE_REFCOUNTED_TYPE(IWireProtocolRowsetReader)

IWireProtocolRowsetReaderPtr CreateWireProtocolRowsetReader(
    const std::vector<TSharedRef>& compressedBlocks,
    NCompression::ECodec codecId,
    const NTableClient::TTableSchema& schema,
    const NLogging::TLogger& logger);

///////////////////////////////////////////////////////////////////////////////

struct IWireProtocolRowsetWriter
    : public NTableClient::ISchemafulWriter
{
    virtual std::vector<TSharedRef> GetCompressedBlocks() = 0;
};

DEFINE_REFCOUNTED_TYPE(IWireProtocolRowsetWriter)

IWireProtocolRowsetWriterPtr CreateWireProtocolRowsetWriter(
    NCompression::ECodec codecId,
    size_t desiredUncompressedBlockSize,
    const NTableClient::TTableSchema& schema,
    const NLogging::TLogger& logger);

///////////////////////////////////////////////////////////////////////////////

} // namespace NTabletClient
} // namespace NYT

