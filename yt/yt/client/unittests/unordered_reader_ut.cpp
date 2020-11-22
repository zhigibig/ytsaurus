#include <yt/core/test_framework/framework.h>

#include <yt/client/table_client/unordered_schemaful_reader.h>
#include <yt/client/table_client/unversioned_reader.h>
#include <yt/client/table_client/unversioned_row.h>
#include <yt/client/table_client/unversioned_row_batch.h>

#include <yt/core/actions/cancelable_context.h>

#include <yt/core/concurrency/spinlock.h>

#include <yt/core/actions/future.h>

namespace NYT {
namespace {

using namespace NConcurrency;
using namespace NTableClient;
using namespace NChunkClient;
using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////////////////

class TUnorderedReaderTest
    : public ::testing::Test
{ };

class TSchemafulReaderMock
    : public ISchemafulUnversionedReader
{
public:
    virtual IUnversionedRowBatchPtr Read(const TRowBatchReadOptions& /*options*/ = {})
    {
        return ReadyEvent_.IsSet() ? nullptr : CreateEmptyUnversionedRowBatch();
    }

    virtual TFuture<void> GetReadyEvent() const
    {
        return ReadyEvent_;
    }

    void SetReadyEvent(const TError& error)
    {
        ReadyEvent_.Set(error);
    }

    virtual TDataStatistics GetDataStatistics() const override
    {
        return {};
    }

    virtual NChunkClient::TCodecStatistics GetDecompressionStatistics() const override
    {
        return {};
    }

    virtual bool IsFetchingCompleted() const override
    {
        return false;
    }

    virtual std::vector<TChunkId> GetFailedChunkIds() const override
    {
        return {};
    }

private:
    const TPromise<void> ReadyEvent_ = NewPromise<void>();
};

TEST_F(TUnorderedReaderTest, Simple)
{
    auto reader1 = New<TSchemafulReaderMock>();
    auto reader2 = New<TSchemafulReaderMock>();

    auto subqueryReaderCreator = [&, index = 0] () mutable -> ISchemafulUnversionedReaderPtr {
        if (index == 0) {
            ++index;
            return reader1;
        } else if (index == 1) {
            ++index;
            return reader2;
        } else {
            return nullptr;
        }
    };

    auto mergingReader = CreateUnorderedSchemafulReader(subqueryReaderCreator, 2);

    EXPECT_TRUE(mergingReader->Read().operator bool());

    reader1->SetReadyEvent(TError());
    reader2->SetReadyEvent(TError("Error"));

    EXPECT_TRUE(mergingReader->GetReadyEvent().IsSet());
    EXPECT_TRUE(mergingReader->GetReadyEvent().Get().IsOK());

    EXPECT_TRUE(mergingReader->Read().operator bool());
    EXPECT_TRUE(mergingReader->GetReadyEvent().IsSet());
    EXPECT_EQ("Error", mergingReader->GetReadyEvent().Get().GetMessage());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT
