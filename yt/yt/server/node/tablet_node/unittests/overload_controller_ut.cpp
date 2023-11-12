#include <yt/yt/server/node/tablet_node/overload_controller.h>

#include <yt/yt/server/lib/tablet_node/config.h>

#include <yt/yt/core/concurrency/action_queue.h>
#include <yt/yt/core/concurrency/new_fair_share_thread_pool.h>
#include <yt/yt/core/concurrency/two_level_fair_share_thread_pool.h>

#include <yt/yt/core/test_framework/framework.h>

namespace NYT::NTabletNode {
namespace {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

class TMockInvoker
    : public IInvoker
{
public:
    TWaitTimeObserver WaitTimeObserver;

    void Invoke(TClosure /*callback*/) override
    { }

    void Invoke(TMutableRange<TClosure> /*callbacks*/) override
    { }

    bool CheckAffinity(const IInvokerPtr& /*invoker*/) const override
    {
        return false;
    }

    bool IsSerialized() const override
    {
        return true;
    }

    NThreading::TThreadId GetThreadId() const override
    {
        return {};
    }

    void RegisterWaitTimeObserver(TWaitTimeObserver waitTimeObserver) override
    {
        WaitTimeObserver = waitTimeObserver;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TMethodInfo
{
    TString Service;
    TString Method;
    double WaitingTimeoutFraction = 0;
};

using TMethodInfoList = std::vector<TMethodInfo>;

static const TDuration MeanWaitTimeThreshold = TDuration::MilliSeconds(20);

TOverloadControllerConfigPtr CreateConfig(const THashMap<TString, TMethodInfoList>& schema)
{
    auto config = New<TOverloadControllerConfig>();
    config->Enabled = true;

    for (const auto& [trackerName, methods] : schema) {
        auto trackerConfig = New<TOverloadTrackerConfig>();

        for (const auto& methodInfo : methods) {
            {
                auto serviceMethod = New<TServiceMethod>();
                serviceMethod->Service = methodInfo.Service;
                serviceMethod->Method = methodInfo.Method;
                trackerConfig->MethodsToThrottle.push_back(std::move(serviceMethod));
            }
            {
                auto serviceMethodConfig = New<TServiceMethodConfig>();
                serviceMethodConfig->Service = methodInfo.Service;
                serviceMethodConfig->Method = methodInfo.Method;
                serviceMethodConfig->WaitingTimeoutFraction = methodInfo.WaitingTimeoutFraction;
                config->Methods.push_back(std::move(serviceMethodConfig));
            }
            trackerConfig->MeanWaitTimeThreshold = MeanWaitTimeThreshold;
        }

        config->Trackers[trackerName] = trackerConfig;
    }

    return config;
}

////////////////////////////////////////////////////////////////////////////////

TEST(TOverloadControllerTest, TestOverloadsRequests)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto mockInvoker = New<TMockInvoker>();
    auto mockInvoker2 = New<TMockInvoker>();

    controller->TrackInvoker("Mock", mockInvoker);
    controller->TrackInvoker("Mock2", mockInvoker2);

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod"}}},
        {"Mock2", {{"MockService", "MockMethod2"}}},
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(1);
    controller->Reconfigure(config);
    controller->Start();

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold * 2);
    }

    // Check overload incoming requests
    int remainsCount = 1000;
    while (remainsCount > 0) {
        EXPECT_FALSE(ShouldThrottleCall(controller->GetCongestionState("MockService", "MockMethod2")));

        auto overloaded = ShouldThrottleCall(controller->GetCongestionState("MockService", "MockMethod"));
        if (overloaded) {
            --remainsCount;
        } else {
            Sleep(TDuration::MicroSeconds(10));
        }
    }

    // Check recovering even if no calls
    while (remainsCount < 1000) {
        auto overloaded = ShouldThrottleCall(controller->GetCongestionState("MockService", "MockMethod"));
        if (!overloaded) {
            ++remainsCount;
        } else {
            Sleep(TDuration::MicroSeconds(1));
        }
    }
}

TEST(TOverloadControllerTest, TestNoOverloads)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto mockInvoker = New<TMockInvoker>();

    controller->TrackInvoker("Mock", mockInvoker);

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod"}}}
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(1);

    controller->Reconfigure(config);
    controller->Start();

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold / 2);
    }

    for (int i = 0; i < 10000; ++i) {
        EXPECT_FALSE(ShouldThrottleCall(controller->GetCongestionState("MockService", "MockMethod")));
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold / 2);

        Sleep(TDuration::MicroSeconds(10));
    }
}

TEST(TOverloadControllerTest, TestTwoInvokersSameMethod)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto mockInvoker = New<TMockInvoker>();
    auto mockInvoker2 = New<TMockInvoker>();

    controller->TrackInvoker("Mock", mockInvoker);
    controller->TrackInvoker("Mock2", mockInvoker2);

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod"}}},
        {"Mock2", {{"MockService", "MockMethod"}}},
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(1);

    controller->Reconfigure(config);
    controller->Start();

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold * 2);
        mockInvoker2->WaitTimeObserver(MeanWaitTimeThreshold / 2);
    }

    // Check overloading incoming requests
    int remainsCount = 1000;
    while (remainsCount > 0) {
        auto overloaded = ShouldThrottleCall(controller->GetCongestionState("MockService", "MockMethod"));
        if (overloaded) {
            --remainsCount;
        } else {
            Sleep(TDuration::MicroSeconds(10));
        }
    }

    // Check recovering even if no calls
    while (remainsCount < 1000) {
        auto overloaded = ShouldThrottleCall(controller->GetCongestionState("MockService", "MockMethod"));
        if (!overloaded) {
            ++remainsCount;
        } else {
            Sleep(TDuration::MicroSeconds(1));
        }
    }
}

TEST(TOverloadControllerTest, TestCongestionWindow)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto mockInvoker = New<TMockInvoker>();
    auto mockInvoker2 = New<TMockInvoker>();

    controller->TrackInvoker("Mock", mockInvoker);
    controller->TrackInvoker("Mock2", mockInvoker2);

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod", 0.3}}},
        {"Mock2", {{"MockService", "MockMethod2", 0.3}}},
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(1);
    controller->Reconfigure(config);
    controller->Start();

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold * 2);
    }

    // Check overload incoming requests
    int remainsCount = 1000;
    while (remainsCount > 0) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold * 2);
        {
            auto window2 = controller->GetCongestionState("MockService", "MockMethod2");
            EXPECT_EQ(window2.MaxWindow, window2.CurrentWindow);
        }

        auto congestionState = controller->GetCongestionState("MockService", "MockMethod");
        bool overloaded = congestionState.MaxWindow != congestionState.CurrentWindow;
        if (overloaded) {
            --remainsCount;
            EXPECT_EQ(0.3, congestionState.WaitingTimeoutFraction);
            EXPECT_EQ(congestionState.OverloadedTrackers, TCongestionState::TTrackersList{"Mock"});
        } else {
            Sleep(TDuration::MicroSeconds(10));
        }
    }

    // Check recovering even if no calls
    while (remainsCount < 1000) {
        auto congestionState = controller->GetCongestionState("MockService", "MockMethod");
        bool overloaded = congestionState.MaxWindow != congestionState.CurrentWindow;

        if (!overloaded) {
            ++remainsCount;
        } else {
            Sleep(TDuration::MicroSeconds(1));
        }
    }
}

TEST(TOverloadControllerTest, TestCongestionWindowTwoTrackers)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto mockInvoker = New<TMockInvoker>();
    auto mockInvoker2 = New<TMockInvoker>();

    controller->TrackInvoker("Mock", mockInvoker);
    controller->TrackInvoker("Mock2", mockInvoker2);

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod", 0.3}}},
        {"Mock2", {{"MockService", "MockMethod", 0.3}}},
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(1);
    controller->Reconfigure(config);
    controller->Start();

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        mockInvoker->WaitTimeObserver(MeanWaitTimeThreshold * 2);
        mockInvoker2->WaitTimeObserver(MeanWaitTimeThreshold * 2);
    }

    // Check overload incoming requests
    int remainsCount = 10;
    while (remainsCount > 0) {
        auto congestionState = controller->GetCongestionState("MockService", "MockMethod");
        bool overloaded = congestionState.MaxWindow != congestionState.CurrentWindow;
        if (overloaded) {
            --remainsCount;
            auto trackers = controller->GetCongestionState("MockService", "MockMethod").OverloadedTrackers;
            std::sort(trackers.begin(), trackers.end());
            EXPECT_EQ(trackers, TCongestionState::TTrackersList({"Mock", "Mock2"}));
        } else {
            Sleep(TDuration::MicroSeconds(10));
        }
    }
}

TEST(TOverloadControllerTest, TestCongestionWindowTwoInstancies)
{
    auto controller = New<TOverloadController>(New<TOverloadControllerConfig>());
    auto tracker1 = controller->CreateGenericTracker("Mock", "Mock.1");
    auto tracker2 = controller->CreateGenericTracker("Mock", "Mock.2");

    auto config = CreateConfig({
        {"Mock", {{"MockService", "MockMethod", 0.3}}},
    });
    config->LoadAdjustingPeriod = TDuration::MilliSeconds(1);
    controller->Reconfigure(config);
    controller->Start();

    // Simulate overload
    for (int i = 0; i < 5000; ++i) {
        tracker1(MeanWaitTimeThreshold * 2);
    }

    // Check overload incoming requests
    int remainsCount = 10;
    while (remainsCount > 0) {
        auto congestionState = controller->GetCongestionState("MockService", "MockMethod");
        bool overloaded = congestionState.MaxWindow != congestionState.CurrentWindow;
        if (overloaded) {
            --remainsCount;
            auto trackers = controller->GetCongestionState("MockService", "MockMethod").OverloadedTrackers;
            EXPECT_EQ(trackers, TCongestionState::TTrackersList({"Mock"}));
        } else {
            Sleep(TDuration::MicroSeconds(10));
        }
    }

    Sleep(TDuration::MicroSeconds(10));

    for (int i = 0; i < 5000; ++i) {
        tracker1(MeanWaitTimeThreshold * 2);
        tracker2(MeanWaitTimeThreshold * 2);
    }

    remainsCount = 10;
    while (remainsCount > 0) {
        auto congestionState = controller->GetCongestionState("MockService", "MockMethod");
        bool overloaded = congestionState.MaxWindow != congestionState.CurrentWindow;
        if (overloaded) {
            --remainsCount;
            auto trackers = controller->GetCongestionState("MockService", "MockMethod").OverloadedTrackers;
            EXPECT_EQ(trackers, TCongestionState::TTrackersList({"Mock"}));
        } else {
            Sleep(TDuration::MicroSeconds(10));
        }
    }

    Sleep(TDuration::MicroSeconds(10));
    for (int i = 0; i < 5000; ++i) {
        tracker1(MeanWaitTimeThreshold / 2);
        tracker2(MeanWaitTimeThreshold * 2);
    }

    remainsCount = 10;
    while (remainsCount > 0) {
        auto congestionState = controller->GetCongestionState("MockService", "MockMethod");
        bool overloaded = congestionState.MaxWindow != congestionState.CurrentWindow;
        if (overloaded) {
            --remainsCount;
            auto trackers = controller->GetCongestionState("MockService", "MockMethod").OverloadedTrackers;
            EXPECT_EQ(trackers, TCongestionState::TTrackersList({"Mock"}));
        } else {
            Sleep(TDuration::MicroSeconds(10));
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

template <class TExecutorPtr>
void ExecuteWaitTimeTest(const TExecutorPtr& executor, const IInvokerPtr& invoker)
{
    static constexpr int DesiredActionsCount = 27;

    TDuration totalWaitTime;
    int actionsCount = 0;

    executor->RegisterWaitTimeObserver([&] (TDuration waitTime) {
        totalWaitTime += waitTime;
        ++actionsCount;
    });

    std::vector<TFuture<void>> futures;
    for (int i = 0; i < DesiredActionsCount; ++i) {
        auto future = BIND([] {
            Sleep(TDuration::MilliSeconds(1));
        }).AsyncVia(invoker)
        .Run();

        futures.push_back(std::move(future));
    }

    WaitFor(AllSucceeded(std::move(futures)))
        .ThrowOnError();

    EXPECT_EQ(DesiredActionsCount, actionsCount);
    EXPECT_GE(totalWaitTime, TDuration::MilliSeconds(DesiredActionsCount - 1));
}

TEST(TOverloadControllerTest, WaitTimeObserver)
{
    {
        auto actionQueue = New<TActionQueue>("TestActionQueue");
        ExecuteWaitTimeTest(actionQueue->GetInvoker(), actionQueue->GetInvoker());
    }

    {
        auto fshThreadPool = CreateTwoLevelFairShareThreadPool(1, "TestFsh");
        ExecuteWaitTimeTest(fshThreadPool, fshThreadPool->GetInvoker("test-pool", "fsh-tag"));
    }

    {
        auto fshThreadPool = CreateNewTwoLevelFairShareThreadPool(1, "TestNewFsh");
        ExecuteWaitTimeTest(fshThreadPool, fshThreadPool->GetInvoker("test-pool", "fsh-tag"));
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTabletNode
