#pragma once

#include "fwd.h"

#include "client_method_options.h"
#include "common.h"
#include "node.h"
#include "mpl.h"

#include <contrib/libs/protobuf/message.h>

#include <util/stream/input.h>
#include <util/stream/output.h>
#include <util/generic/yexception.h>
#include <util/generic/maybe.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

enum EDataStreamFormat
{
    DSF_YSON_TEXT,
    DSF_YSON_BINARY,
    DSF_YAMR_LENVAL,
    DSF_BYTES,
    DSF_PROTO
};

////////////////////////////////////////////////////////////////////////////////

struct INodeReaderImpl;
struct IYaMRReaderImpl;
struct IProtoReaderImpl;
struct INodeWriterImpl;
struct IYaMRWriterImpl;
struct IProtoWriterImpl;

////////////////////////////////////////////////////////////////////////////////

class TIOException
    : public yexception
{ };

///////////////////////////////////////////////////////////////////////////////

class IFileReader
    : public TThrRefBase
    , public TInputStream
{ };

class IFileWriter
    : public TThrRefBase
    , public TOutputStream
{ };

////////////////////////////////////////////////////////////////////////////////

class TRawTableReader
    : public TThrRefBase
    , public TInputStream
{
public:
    // Retries table read starting from the specified rangeIndex and rowIndex.
    // If rowIndex is empty then entire last request will be retried.
    // Otherwise the request will be modified to retrieve table data starting from the specified
    // rangeIndex and rowIndex.
    // The method returns 'true' on successful request retry. If it returns 'false' then
    // the error is fatal and Retry() shouldn't be called any more.
    // After successful retry the user should reset rangeIndex / rowIndex values and read new ones
    // from the stream
    virtual bool Retry(
        const TMaybe<ui32>& rangeIndex,
        const TMaybe<ui64>& rowIndex) = 0;

    // Returns 'true' if the input stream may contain table ranges.
    // The TRawTableReader user is responsible to track active range index in this case
    // in order to pass it to Retry().
    virtual bool HasRangeIndices() const = 0;
};

class TRawTableWriter
    : public TThrRefBase
    , public TOutputStream
{
public:
    // Should be called after complete record is written.
    // When this method is called TRowTableWriter checks its buffer size
    // and if it is full it sends data to YT.
    // NOTE: TRowTableWriter never sends partial records to YT (due to retries).

    virtual void NotifyRowEnd() = 0;
};

////////////////////////////////////////////////////////////////////////////////

template <class T, class>
class TTableReader
    : public TThrRefBase
{
public:
    const T& GetRow() const; // may be a template function
    bool IsValid() const;
    ui32 GetTableIndex() const;
    ui64 GetRowIndex() const;
    void Next();
};

////////////////////////////////////////////////////////////////////////////////

template <class T, class>
class TTableRangesReader
    : public TThrRefBase
{
public:
    TTableReader<T>& GetRange();
    bool IsValid() const;
    void Next();
};

////////////////////////////////////////////////////////////////////////////////

template <class T, class>
class TTableWriter
    : public TThrRefBase
{
public:
    void AddRow(const T& row); // may be a template function
    void Finish();
};

////////////////////////////////////////////////////////////////////////////////

struct TYaMRRow
{
    TStringBuf Key;
    TStringBuf SubKey;
    TStringBuf Value;
};

////////////////////////////////////////////////////////////////////////////////

class IIOClient
{
public:
    virtual IFileReaderPtr CreateFileReader(
        const TRichYPath& path,
        const TFileReaderOptions& options = TFileReaderOptions()) = 0;

    virtual IFileWriterPtr CreateFileWriter(
        const TRichYPath& path,
        const TFileWriterOptions& options = TFileWriterOptions()) = 0;

    template <class T>
    TTableReaderPtr<T> CreateTableReader(
        const TRichYPath& path,
        const TTableReaderOptions& options = TTableReaderOptions());

    template <class T>
    TTableWriterPtr<T> CreateTableWriter(
        const TRichYPath& path,
        const TTableWriterOptions& options = TTableWriterOptions());

    virtual TRawTableReaderPtr CreateRawReader(
        const TRichYPath& path,
        EDataStreamFormat format,
        const TTableReaderOptions& options,
        const TString& formatConfig = TString()) = 0;

    virtual TRawTableWriterPtr CreateRawWriter(
        const TRichYPath& path,
        EDataStreamFormat format,
        const TTableWriterOptions& options,
        const TString& formatConfig = TString()) = 0;

private:
    virtual ::TIntrusivePtr<INodeReaderImpl> CreateNodeReader(
        const TRichYPath& path, const TTableReaderOptions& options) = 0;

    virtual ::TIntrusivePtr<IYaMRReaderImpl> CreateYaMRReader(
        const TRichYPath& path, const TTableReaderOptions& options) = 0;

    virtual ::TIntrusivePtr<IProtoReaderImpl> CreateProtoReader(
        const TRichYPath& path,
        const TTableReaderOptions& options,
        const ::google::protobuf::Message* prototype) = 0;

    virtual ::TIntrusivePtr<INodeWriterImpl> CreateNodeWriter(
        const TRichYPath& path, const TTableWriterOptions& options) = 0;

    virtual ::TIntrusivePtr<IYaMRWriterImpl> CreateYaMRWriter(
        const TRichYPath& path, const TTableWriterOptions& options) = 0;

    virtual ::TIntrusivePtr<IProtoWriterImpl> CreateProtoWriter(
        const TRichYPath& path,
        const TTableWriterOptions& options,
        const ::google::protobuf::Message* prototype) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define IO_INL_H_
#include "io-inl.h"
#undef IO_INL_H_

