#include "file_writer.h"

#include <mapreduce/yt/io/helpers.h>
#include <mapreduce/yt/interface/finish_or_die.h>

#include <mapreduce/yt/common/helpers.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TFileWriter::TFileWriter(
    const TRichYPath& path,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TFileWriterOptions& options)
    : BlockWriter_(
        auth,
        transactionId,
        GetWriteFileCommand(),
        TMaybe<TFormat>(),
        path,
        BUFFER_SIZE,
        options)
{ }

TFileWriter::~TFileWriter()
{
    NDetail::FinishOrDie(this, "TFileWriter");
}

void TFileWriter::DoWrite(const void* buf, size_t len)
{
    BlockWriter_.Write(buf, len);
    BlockWriter_.NotifyRowEnd();
}

void TFileWriter::DoFinish()
{
    BlockWriter_.Finish();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
