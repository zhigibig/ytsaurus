#include <yt/core/test_framework/framework.h>

#include <yt/ytlib/chunk_client/chunk_reader_memory_manager.h>
#include <yt/ytlib/chunk_client/parallel_reader_memory_manager.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/scheduler.h>

#include <random>

namespace NYT::NChunkClient {
namespace {

constexpr auto WaitIterationCount = 50;
constexpr auto WaitIterationDuration = TDuration::MilliSeconds(5);

////////////////////////////////////////////////////////////////////////////////

void WaitTestPredicate(std::function<bool()> predicate)
{
    WaitForPredicate(predicate, WaitIterationCount, WaitIterationDuration);
}

TEST(TestParallelReaderMemoryManager, TestMemoryManagerAllocatesDesiredMemorySizeIfPossible)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100'000, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();

    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 0; });

    reader1->SetRequiredMemorySize(123);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 123; });

    reader1->SetPrefetchMemorySize(234);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 357; });
}

TEST(TestParallelReaderMemoryManager, TestChunkReaderMemoryManagerGetsMemory)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100'000, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();
    reader1->SetRequiredMemorySize(100);
    reader1->SetPrefetchMemorySize(100);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 200; });
    EXPECT_EQ(reader1->GetAvailableSize(), 200);

    {
        auto acquire1 = reader1->AsyncAquire(200);
        NConcurrency::WaitFor(acquire1).ValueOrThrow();
    }

    EXPECT_EQ(reader1->GetAvailableSize(), 200);
    auto acquire2 = reader1->AsyncAquire(201);
    Sleep(TDuration::MilliSeconds(100));
    ASSERT_FALSE(acquire2.IsSet());
}

TEST(TestParallelReaderMemoryManager, TestChunkReaderMemoryManagerRevokesMemory)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();
    reader1->SetRequiredMemorySize(50);
    reader1->SetPrefetchMemorySize(50);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 100; });
    EXPECT_EQ(reader1->GetAvailableSize(), 100);

    {
        auto acquire1 = reader1->AsyncAquire(100);
        NConcurrency::WaitFor(acquire1).ValueOrThrow();
    }

    auto reader2 = memoryManager->CreateChunkReaderMemoryManager();
    reader2->SetRequiredMemorySize(50);
    WaitTestPredicate([&] () { return reader2->GetReservedMemorySize() == 50; });
    EXPECT_EQ(reader2->GetReservedMemorySize(), 50);
    EXPECT_EQ(reader1->GetAvailableSize(), 50);
    EXPECT_EQ(reader2->GetAvailableSize(), 50);

    auto acquire2 = reader2->AsyncAquire(51);
    Sleep(TDuration::MilliSeconds(50));
    ASSERT_FALSE(acquire2.IsSet());
}

TEST(TestParallelReaderMemoryManager, TestChunkReaderMemoryManagerUnregister)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();
    reader1->SetPrefetchMemorySize(100);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 100; });

    auto reader2 = memoryManager->CreateChunkReaderMemoryManager();
    reader2->SetPrefetchMemorySize(100);
    Sleep(TDuration::MilliSeconds(50));
    EXPECT_EQ(reader2->GetReservedMemorySize(), 0);

    {
        auto allocation = reader1->AsyncAquire(100);
        NConcurrency::WaitFor(allocation).ValueOrThrow();
        reader1->Finalize();
        Sleep(TDuration::MilliSeconds(50));
        EXPECT_EQ(reader1->GetReservedMemorySize(), 100);
        EXPECT_EQ(reader2->GetReservedMemorySize(), 0);
    }

    WaitTestPredicate([&] () { return reader2->GetReservedMemorySize() == 100; });
}

TEST(TestParallelReaderMemoryManager, TestMemoryManagerAllocatesAsMuchAsPossible)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(120, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();

    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 0; });

    reader1->SetRequiredMemorySize(100);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 100; });

    reader1->SetPrefetchMemorySize(234);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 120; });
}

TEST(TestParallelReaderMemoryManager, TestMemoryManagerFreesMemoryAfterUnregister)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();
    reader1->SetRequiredMemorySize(100);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 100; });

    auto reader2 = memoryManager->CreateChunkReaderMemoryManager();
    reader2->SetRequiredMemorySize(80);
    reader2->SetPrefetchMemorySize(80);
    EXPECT_EQ(reader1->GetReservedMemorySize(), 100);
    EXPECT_EQ(reader2->GetReservedMemorySize(), 0);

    reader1->Finalize();
    WaitTestPredicate([&] () { return reader2->GetReservedMemorySize() == 100; });
}

TEST(TestParallelReaderMemoryManager, TestMemoryManagerBalancing1)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();
    reader1->SetRequiredMemorySize(50);
    reader1->SetPrefetchMemorySize(50);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 100; });

    auto reader2 = memoryManager->CreateChunkReaderMemoryManager();
    reader2->SetRequiredMemorySize(50);
    reader2->SetPrefetchMemorySize(50);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 50; });
    WaitTestPredicate([&] () { return reader2->GetReservedMemorySize() == 50; });

    reader1->Finalize();
    WaitTestPredicate([&] () { return reader2->GetReservedMemorySize() == 100; });
}

TEST(TestParallelReaderMemoryManager, TestMemoryManagerBalancing2)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();
    reader1->SetRequiredMemorySize(80);
    reader1->SetPrefetchMemorySize(100'000);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 100; });

    auto reader2 = memoryManager->CreateChunkReaderMemoryManager();
    reader2->SetRequiredMemorySize(50);
    reader2->SetPrefetchMemorySize(100'000);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 80; });
    WaitTestPredicate([&] () { return reader2->GetReservedMemorySize() == 20; });

    auto reader3 = memoryManager->CreateChunkReaderMemoryManager();
    reader3->SetRequiredMemorySize(50);
    reader3->SetPrefetchMemorySize(100'000);
    EXPECT_EQ(reader3->GetReservedMemorySize(), 0);

    reader2->Finalize();
    WaitTestPredicate([&] () { return reader3->GetReservedMemorySize() == 20; });

    auto reader4 = memoryManager->CreateChunkReaderMemoryManager();
    reader4->SetRequiredMemorySize(50);
    reader4->SetPrefetchMemorySize(100'000);
    EXPECT_EQ(reader4->GetReservedMemorySize(), 0);

    reader1->Finalize();
    WaitTestPredicate([&] () { return reader3->GetReservedMemorySize() == 50; });
    WaitTestPredicate([&] () { return reader4->GetReservedMemorySize() == 50; });
}


TEST(TestParallelReaderMemoryManager, TestInitialMemorySize)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100, 60),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager(1);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 1; });

    auto reader2 = memoryManager->CreateChunkReaderMemoryManager(100);
    WaitTestPredicate([&] () { return reader2->GetReservedMemorySize() == 60; });

    auto reader3 = memoryManager->CreateChunkReaderMemoryManager(50);
    WaitTestPredicate([&] () { return reader3->GetReservedMemorySize() == 39; });
    EXPECT_EQ(reader1->GetReservedMemorySize(), 1);
    EXPECT_EQ(reader2->GetReservedMemorySize(), 60);
}

TEST(TestParallelReaderMemoryManager, TestTotalSize)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();
    reader1->SetRequiredMemorySize(100);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 100; });

    auto reader2 = memoryManager->CreateChunkReaderMemoryManager();
    reader2->SetRequiredMemorySize(100);
    reader1->SetTotalSize(70);

    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 70; });
    EXPECT_EQ(reader2->GetReservedMemorySize(), 30);
}

TEST(TestParallelReaderMemoryManager, TestRequiredMemorySizeNeverDecreases)
{
    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(100, 0),
        actionQueue->GetInvoker());

    auto reader1 = memoryManager->CreateChunkReaderMemoryManager();
    reader1->SetRequiredMemorySize(100);
    WaitTestPredicate([&] () { return reader1->GetReservedMemorySize() == 100; });

    reader1->SetRequiredMemorySize(50);
    auto reader2 = memoryManager->CreateChunkReaderMemoryManager();
    reader2->SetRequiredMemorySize(50);

    Sleep(TDuration::MilliSeconds(50));
    ASSERT_EQ(reader1->GetReservedMemorySize(), 100);
    ASSERT_EQ(reader2->GetReservedMemorySize(), 0);
}

TEST(TestParallelReaderMemoryManager, PerformanceAndStressTest)
{
    constexpr auto ReaderCount = 200'000;
    std::mt19937 rng(12345);

    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(10'000'000, 10'000'000),
        actionQueue->GetInvoker());

    std::vector<TChunkReaderMemoryManagerPtr> readers;
    readers.reserve(ReaderCount);

    for (size_t readerIndex = 0; readerIndex < ReaderCount; ++readerIndex) {
        readers.push_back(memoryManager->CreateChunkReaderMemoryManager());
        readers.back()->SetRequiredMemorySize(rng() % 100);
        readers.back()->SetPrefetchMemorySize(rng() % 100);
    }

    while (!readers.empty()) {
        if (rng() % 3 == 0) {
            readers.back()->Finalize();
            readers.pop_back();
        } else {
            auto readerIndex = rng() % readers.size();
            readers[readerIndex]->SetRequiredMemorySize(rng() % 100);
            readers[readerIndex]->SetPrefetchMemorySize(rng() % 100);
        }
    }
}

TEST(TestParallelReaderMemoryManager, TestManyHeavyRebalancings)
{
    constexpr auto ReaderCount = 100'000;
    constexpr auto RebalancingIterations = 800;

    auto actionQueue = New<NConcurrency::TActionQueue>();

    auto memoryManager = CreateParallelReaderMemoryManager(
        TParallelReaderMemoryManagerOptions(200'000, 200'000),
        actionQueue->GetInvoker());

    std::vector<TChunkReaderMemoryManagerPtr> readers;
    readers.resize(ReaderCount + 1);

    for (size_t readerIndex = 0; readerIndex < ReaderCount; ++readerIndex) {
        readers.push_back(memoryManager->CreateChunkReaderMemoryManager());
        readers.back()->SetRequiredMemorySize(1);
        readers.back()->SetPrefetchMemorySize(1);
    }

    // Each rebalancing iteration revokes unit memory from each reader to give
    // new reader required memory size and then returns this memory back to readers,
    // so rebalancing works slow here.
    for (size_t iteration = 0; iteration < RebalancingIterations; ++iteration) {
        readers.push_back(memoryManager->CreateChunkReaderMemoryManager());
        readers.back()->SetRequiredMemorySize(ReaderCount);

        // All rebalancings except the first should be fast.
        if (iteration == 0) {
            WaitForPredicate([&] () { return readers.back()->GetReservedMemorySize() == ReaderCount; }, 1'000'000, WaitIterationDuration);
        } else {
            WaitTestPredicate([&] () { return readers.back()->GetReservedMemorySize() == ReaderCount; });
        }
        readers.back()->Finalize();
        readers.pop_back();
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NChunkClient
