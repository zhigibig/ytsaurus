#pragma once

#include "public.h"

#include <yt/ytlib/formats/format.h>
#include <yt/ytlib/chunk_client/chunk_owner_ypath_proxy.h>
#include <yt/ytlib/cypress_client/public.h>

#include <yt/core/yson/lexer.h>
#include <yt/core/yson/public.h>
#include <yt/core/misc/phoenix.h>

namespace NYT {
namespace NTableClient {

//////////////////////////////////////////////////////////////////////////////////

class TTableOutput
    : public TOutputStream
{
public:
    TTableOutput(const NFormats::TFormat& format, NYson::IYsonConsumer* consumer);
    ~TTableOutput() throw();

private:
    void DoWrite(const void* buf, size_t len);
    void DoFinish();

    const std::unique_ptr<NFormats::IParser> Parser_;

    bool IsParserValid_ = true;
};

//////////////////////////////////////////////////////////////////////////////////

void PipeReaderToWriter(
    ISchemalessReaderPtr reader,
    ISchemalessWriterPtr writer,
    int bufferRowCount,
    bool validateValues = false);

void PipeInputToOutput(
    TInputStream* input,
    TOutputStream* output,
    i64 bufferBlockSize);

void PipeInputToOutput(
    NConcurrency::IAsyncInputStreamPtr input,
    TOutputStream* output,
    i64 bufferBlockSize);


//////////////////////////////////////////////////////////////////////////////////

// NB: not using TYsonString here to avoid copying.
TUnversionedValue MakeUnversionedValue(
    const TStringBuf& ysonString, int id, 
    NYson::TStatelessLexer& lexer);

//////////////////////////////////////////////////////////////////////////////////

void ValidateKeyColumns(const TKeyColumns& keyColumns, const TKeyColumns& chunkKeyColumns, bool requireUniqueKeys);
TColumnFilter CreateColumnFilter(const NChunkClient::TChannel& protoChannel, TNameTablePtr nameTable);
int GetSystemColumnCount(TChunkReaderOptionsPtr options);

//////////////////////////////////////////////////////////////////////////////////

struct TTableUploadOptions
{
    NChunkClient::EUpdateMode UpdateMode;
    NCypressClient::ELockMode LockMode;
    TTableSchema TableSchema;
    ETableSchemaMode SchemaMode;

    void Persist(NPhoenix::TPersistenceContext& context);
};

TTableUploadOptions GetTableUploadOptions(
    const NYPath::TRichYPath& path,
    const TTableSchema& schema,
    ETableSchemaMode schemaMode,
    i64 rowCount);

//////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
} // namespace NTableClient
