#pragma once

#include "retryful_writer.h"

#include <mapreduce/yt/common/fwd.h>

#include <mapreduce/yt/interface/io.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TFileWriter
    : public IFileWriter
{
public:
    TFileWriter(
        const TRichYPath& path,
        IClientRetryPolicyPtr clientRetryPolicy,
        ITransactionPingerPtr transactionPinger,
        const TAuth& auth,
        const TTransactionId& transactionId,
        const TFileWriterOptions& options = TFileWriterOptions());

    ~TFileWriter() override;

protected:
    void DoWrite(const void* buf, size_t len) override;
    void DoFinish() override;

private:
    TRetryfulWriter RetryfulWriter_;
    static const size_t BUFFER_SIZE = 64 << 20;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
