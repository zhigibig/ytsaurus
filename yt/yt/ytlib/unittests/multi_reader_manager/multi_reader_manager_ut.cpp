#include "mock/multi_reader_memory_manager.h"
#include "mock/multi_chunk_reader.h"
#include "mock/reader_factory.h"

#include <yt/ytlib/chunk_client/multi_reader_manager.h>

#include <yt/client/table_client/helpers.h>

#include <yt/core/test_framework/framework.h>

#include <util/random/shuffle.h>

namespace NYT::NChunkClient {
namespace {

using namespace NConcurrency;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMultiReaderManagerType,
    ((Parallel)   (0))
    ((Sequential) (1))
);


TMultiChunkReaderMockPtr CreateMultiReader(
    std::vector<IReaderBasePtr> readers,
    EMultiReaderManagerType multiReaderManagerType)
{
    auto config = New<TMultiChunkReaderConfig>();
    auto options = New<TMultiChunkReaderOptions>();

    std::vector<IReaderFactoryPtr> factories;

    for (auto& reader : readers) {
        factories.push_back(New<TReaderFactoryMock>(std::move(reader)));
    }

    auto memoryManager = New<TMultiReaderMemoryManagerMock>();

    switch (multiReaderManagerType) {
        case EMultiReaderManagerType::Parallel:
            return New<TMultiChunkReaderMock>(
                CreateParallelMultiReaderManager(
                    std::move(config),
                    std::move(options),
                    std::move(factories),
                    std::move(memoryManager)));
        case EMultiReaderManagerType::Sequential:
            return New<TMultiChunkReaderMock>(
                CreateSequentialMultiReaderManager(
                    std::move(config),
                    std::move(options),
                    std::move(factories),
                    std::move(memoryManager)));
    };
}

std::vector<IReaderBasePtr> CreateMockReaders(int readerCount, int filledRowCount, TDuration delayStep, int emptyRowCount = 0)
{
    std::vector<IReaderBasePtr> readers;

    int filledRowValue = 0;
    for (int readerIndex = 0; readerIndex < readerCount; ++readerIndex) {
        std::vector<std::vector<TUnversionedOwningRow>> readerData;
        for (int i = 0; i < filledRowCount; ++i) {
            readerData.push_back({MakeUnversionedOwningRow(filledRowValue++)});
        }
        for (int i = 0; i < emptyRowCount; ++i) {
            readerData.push_back({});
        }
        Shuffle(readerData.begin(), readerData.end());
        readers.emplace_back(New<TChunkReaderMock>(std::move(readerData), delayStep * readerIndex));
    }
    return readers;
}

IReaderBasePtr CreateReaderWithError(int filledRowCount)
{
    std::vector<std::vector<TUnversionedOwningRow>> readerData;
    for (int rowIndex = 0; rowIndex < filledRowCount; ++rowIndex) {
        readerData.push_back({MakeUnversionedOwningRow(rowIndex + 100)});
    }
    return New<TChunkReaderWithErrorMock>(std::move(readerData), TDuration::Zero());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace

class TMultiReaderManagerTest
    : public ::testing::Test
    , public ::testing::WithParamInterface<EMultiReaderManagerType>
{
protected:
    virtual void SetUp() override
    { }
};

TEST_P(TMultiReaderManagerTest, DataWithEmptyRows)
{
    auto multiReader = CreateMultiReader(
        CreateMockReaders(
            /*readerCount =*/ 10,
            /*filledRowCount =*/ 10,
            /*delayStep =*/ TDuration::MilliSeconds(1),
            /*emptyRowCount =*/ 10),
        GetParam());

    multiReader->Open();

    std::unordered_set<i64> values;
    for (int i = 0; i < 100; ++i) {
        values.insert(i);
    }

    std::vector<TUnversionedRow> readRows;
    while (multiReader->Read(&readRows)) {
        if (readRows.empty()) {
            WaitFor(multiReader->GetReadyEvent())
                .ThrowOnError();
                continue;
        }
        EXPECT_EQ(1, readRows.size());
        auto value = readRows.front()[0].Data.Int64;
        EXPECT_NE(values.end(), values.find(value));
        values.erase(value);
    }

    EXPECT_EQ(0, values.size());
}

TEST_P(TMultiReaderManagerTest, ReaderWithError)
{
    auto readers = CreateMockReaders(/*readerCount =*/ 2, /*filledRowCount =*/ 5, /*delayStep =*/ TDuration::MilliSeconds(1));
    readers.emplace_back(CreateReaderWithError(/*filledRowCount =*/ 5));

    auto multiReader = CreateMultiReader(std::move(readers), GetParam());

    multiReader->Open();

    std::vector<TUnversionedRow> readRows;
    for (int i = 0; i < 20; ++i) {
        readRows.clear();
        while (readRows.empty()) {
            multiReader->Read(&readRows);
            if (readRows.empty()) {
                auto error = WaitFor(multiReader->GetReadyEvent());
                if (error.IsOK()) {
                    EXPECT_EQ(multiReader->GetFailedChunkIds().size(), 0);
                } else {
                    EXPECT_EQ(multiReader->GetFailedChunkIds().size(), 1);
                    break;
                }
            }
        }
    }

    EXPECT_EQ(multiReader->GetFailedChunkIds().size(), 1);
    EXPECT_FALSE(WaitFor(multiReader->GetReadyEvent()).IsOK());
}

TEST_P(TMultiReaderManagerTest, Interrupt)
{
    auto readers = CreateMockReaders(/*readerCount =*/ 5, /*filledRowCount =*/ 10, /*delayStep =*/ TDuration::MilliSeconds(1));

    auto multiReader = CreateMultiReader(std::move(readers), GetParam());

    multiReader->Open();

    std::vector<TUnversionedRow> readRows;
    for (int i = 0; i < 15; ++i) {
        readRows.clear();
        while (multiReader->Read(&readRows)) {
            if (readRows.empty()) {
                WaitFor(multiReader->GetReadyEvent())
                    .ThrowOnError();
                    continue;
            } else {
                break;
            }
        }
    }

    multiReader->Interrupt();

    WaitFor(multiReader->GetReadyEvent())
        .ThrowOnError();
    int remainingRowCount = 0;
    while (multiReader->Read(&readRows)) {
        if (readRows.empty()) {
            WaitFor(multiReader->GetReadyEvent())
                .ThrowOnError();
                continue;
        }
        ++remainingRowCount;
    }
    EXPECT_EQ(5, remainingRowCount);
}

INSTANTIATE_TEST_SUITE_P(
    TMultiReaderManagerTest,
    TMultiReaderManagerTest,
    ::testing::Values(EMultiReaderManagerType::Parallel, EMultiReaderManagerType::Sequential)
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
