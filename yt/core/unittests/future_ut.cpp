#include <yt/core/test_framework/framework.h>

#include <yt/core/actions/cancelable_context.h>
#include <yt/core/actions/future.h>
#include <yt/core/actions/invoker_util.h>

#include <yt/core/misc/ref_counted_tracker.h>

#include <util/system/thread.h>

namespace NYT {
namespace {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto SleepQuantum = TDuration::MilliSeconds(50);

class TFutureTest
    : public ::testing::Test
{ };

TEST_F(TFutureTest, IsNull)
{
    TFuture<int> empty;
    TFuture<int> nonEmpty = MakeFuture(42);

    EXPECT_FALSE(empty);
    EXPECT_TRUE(nonEmpty);

    empty = std::move(nonEmpty);

    EXPECT_TRUE(empty);
    EXPECT_FALSE(nonEmpty);

    swap(empty, nonEmpty);

    EXPECT_FALSE(empty);
    EXPECT_TRUE(nonEmpty);
}

TEST_F(TFutureTest, IsNullVoid)
{
    TFuture<void> empty;
    TFuture<void> nonEmpty = VoidFuture;

    EXPECT_FALSE(empty);
    EXPECT_TRUE(nonEmpty);

    empty = std::move(nonEmpty);

    EXPECT_TRUE(empty);
    EXPECT_FALSE(nonEmpty);

    swap(empty, nonEmpty);

    EXPECT_FALSE(empty);
    EXPECT_TRUE(nonEmpty);
}

TEST_F(TFutureTest, Reset)
{
    auto foo = MakeFuture(42);

    EXPECT_TRUE(foo);
    foo.Reset();
    EXPECT_FALSE(foo);
}

TEST_F(TFutureTest, IsSet)
{
    auto promise = NewPromise<int>();
    auto future = promise.ToFuture();

    EXPECT_FALSE(future.IsSet());
    EXPECT_FALSE(promise.IsSet());
    promise.Set(42);
    EXPECT_TRUE(future.IsSet());
    EXPECT_TRUE(promise.IsSet());
}

TEST_F(TFutureTest, SetAndGet)
{
    auto promise = NewPromise<int>();
    auto future = promise.ToFuture();

    promise.Set(57);
    EXPECT_EQ(57, future.Get().Value());
    EXPECT_EQ(57, future.Get().Value()); // Second Get() should also work.
}

TEST_F(TFutureTest, SetAndTryGet)
{
    auto promise = NewPromise<int>();
    auto future = promise.ToFuture();

    {
        auto result = future.TryGet();
        EXPECT_FALSE(result);
    }

    promise.Set(42);

    {
        auto result = future.TryGet();
        EXPECT_TRUE(result);
        EXPECT_EQ(42, result->Value());
    }
}

class TMock
{
public:
    MOCK_METHOD1(Tacke, void(int));
};

TEST_F(TFutureTest, Subscribe)
{
    TMock firstMock;
    TMock secondMock;

    EXPECT_CALL(firstMock, Tacke(42)).Times(1);
    EXPECT_CALL(secondMock, Tacke(42)).Times(1);

    auto firstSubscriber = BIND([&] (const TErrorOr<int>& x) { firstMock.Tacke(x.Value()); });
    auto secondSubscriber = BIND([&] (const TErrorOr<int>& x) { secondMock.Tacke(x.Value()); });

    auto promise = NewPromise<int>();
    auto future = promise.ToFuture();

    future.Subscribe(firstSubscriber);
    promise.Set(42);
    future.Subscribe(secondSubscriber);
}

TEST_F(TFutureTest, GetUnique)
{
    auto promise = NewPromise<std::vector<int>>();
    auto future = promise.ToFuture();

    EXPECT_FALSE(future.IsSet());

    std::vector v{1, 2, 3};
    promise.Set(v);

    EXPECT_TRUE(future.IsSet());
    auto w = future.GetUnique();
    EXPECT_TRUE(w.IsOK());
    EXPECT_EQ(v, w.Value());
    EXPECT_TRUE(future.IsSet());
}

TEST_F(TFutureTest, TryGetUnique)
{
    auto promise = NewPromise<std::vector<int>>();
    auto future = promise.ToFuture();

    EXPECT_FALSE(future.IsSet());
    EXPECT_FALSE(future.TryGetUnique());

    std::vector v{1, 2, 3};
    promise.Set(v);

    EXPECT_TRUE(future.IsSet());
    auto w = future.TryGetUnique();
    EXPECT_TRUE(w);
    EXPECT_TRUE(w->IsOK());
    EXPECT_EQ(v, w->Value());
    EXPECT_TRUE(future.IsSet());
}

TEST_F(TFutureTest, SubscribeUniqueBeforeSet)
{
    std::vector v{1, 2, 3};

    auto promise = NewPromise<std::vector<int>>();
    auto future = promise.ToFuture();

    std::vector<int> vv;
    future.SubscribeUnique(BIND([&] (TErrorOr<std::vector<int>>&& arg) {
        EXPECT_TRUE(arg.IsOK());
        vv = std::move(arg.Value());
    }));

    EXPECT_FALSE(future.IsSet());
    promise.Set(v);
    EXPECT_TRUE(future.IsSet());
    EXPECT_EQ(v, vv);
}

TEST_F(TFutureTest, SubscribeUniqueAfterSet)
{
    std::vector v{1, 2, 3};

    auto promise = NewPromise<std::vector<int>>();
    auto future = promise.ToFuture();

    EXPECT_FALSE(future.IsSet());
    promise.Set(v);
    EXPECT_TRUE(future.IsSet());

    std::vector<int> vv;
    future.SubscribeUnique(BIND([&] (TErrorOr<std::vector<int>>&& arg) {
        EXPECT_TRUE(arg.IsOK());
        vv = std::move(arg.Value());
    }));

    EXPECT_EQ(v, vv);
    EXPECT_TRUE(future.IsSet());
}

static void* AsynchronousIntSetter(void* param)
{
    Sleep(SleepQuantum);

    auto* promise = reinterpret_cast<TPromise<int>*>(param);
    promise->Set(42);

    return nullptr;
}

static void* AsynchronousVoidSetter(void* param)
{
    Sleep(SleepQuantum);

    auto* promise = reinterpret_cast<TPromise<void>*>(param);
    promise->Set();

    return nullptr;
}

TEST_F(TFutureTest, SubscribeWithAsynchronousSet)
{
    TMock firstMock;
    TMock secondMock;

    EXPECT_CALL(firstMock, Tacke(42)).Times(1);
    EXPECT_CALL(secondMock, Tacke(42)).Times(1);

    auto firstSubscriber = BIND([&] (const TErrorOr<int>& x) { firstMock.Tacke(x.Value()); });
    auto secondSubscriber = BIND([&] (const TErrorOr<int>& x) { secondMock.Tacke(x.Value()); });

    auto promise = NewPromise<int>();
    auto future = promise.ToFuture();

    future.Subscribe(firstSubscriber);

    TThread thread(&AsynchronousIntSetter, &promise);
    thread.Start();
    thread.Join();

    future.Subscribe(secondSubscriber);
}

TEST_F(TFutureTest, CascadedApply)
{
    auto kicker = NewPromise<bool>();

    auto left   = NewPromise<int>();
    auto right  = NewPromise<int>();

    TThread thread(&AsynchronousIntSetter, &left);

    auto leftPrime =
        kicker.ToFuture()
        .Apply(BIND([=, &thread] (bool f) -> TFuture<int> {
            thread.Start();
            return left.ToFuture();
        }))
        .Apply(BIND([=] (int xv) -> int {
            return xv + 8;
        }));
    auto rightPrime =
        right.ToFuture()
        .Apply(BIND([=] (int xv) -> TFuture<int> {
            return MakeFuture(xv + 4);
        }));

    int accumulator = 0;
    auto accumulate = BIND([&] (const TErrorOr<int>& x) { accumulator += x.Value(); });

    leftPrime.Subscribe(accumulate);
    rightPrime.Subscribe(accumulate);

    // Ensure that thread was not started.
    Sleep(SleepQuantum * 2);

    // Initial computation condition.
    EXPECT_FALSE(left.IsSet());  EXPECT_FALSE(leftPrime.IsSet());
    EXPECT_FALSE(right.IsSet()); EXPECT_FALSE(rightPrime.IsSet());
    EXPECT_EQ(0, accumulator);

    // Kick off!
    kicker.Set(true);
    EXPECT_FALSE(left.IsSet());  EXPECT_FALSE(leftPrime.IsSet());
    EXPECT_FALSE(right.IsSet()); EXPECT_FALSE(rightPrime.IsSet());
    EXPECT_EQ(0, accumulator);

    // Kick off!
    right.Set(1);

    EXPECT_FALSE(left.IsSet());  EXPECT_FALSE(leftPrime.IsSet());
    EXPECT_TRUE(right.IsSet());  EXPECT_TRUE(rightPrime.IsSet());
    EXPECT_EQ( 5, accumulator);
    EXPECT_EQ( 1, right.Get().Value());
    EXPECT_EQ( 5, rightPrime.Get().Value());

    // This will sleep for a while until left branch will be evaluated.
    thread.Join();

    EXPECT_TRUE(left.IsSet());   EXPECT_TRUE(leftPrime.IsSet());
    EXPECT_TRUE(right.IsSet());  EXPECT_TRUE(rightPrime.IsSet());
    EXPECT_EQ(55, accumulator);
    EXPECT_EQ(42, left.Get().Value());
    EXPECT_EQ(50, leftPrime.Get().Value());
}

TEST_F(TFutureTest, ApplyVoidToVoid)
{
    int state = 0;

    auto kicker = NewPromise<void>();

    auto source = kicker.ToFuture();
    auto target = source
        .Apply(BIND([&] () -> void { ++state; }));

    EXPECT_EQ(0, state);
    EXPECT_FALSE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    kicker.Set();

    EXPECT_EQ(1, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_TRUE(target.IsSet());
}

TEST_F(TFutureTest, ApplyVoidToFutureVoid)
{
    int state = 0;

    auto kicker = NewPromise<void>();
    auto setter = NewPromise<void>();

    TThread thread(&AsynchronousVoidSetter, &setter);

    auto source = kicker.ToFuture();
    auto target = source
        .Apply(BIND([&] () -> TFuture<void> {
            ++state;
            thread.Start();
            return setter.ToFuture();
        }));

    // Ensure that thread was not started.
    Sleep(SleepQuantum * 2);

    // Initial computation condition.
    EXPECT_EQ(0, state);
    EXPECT_FALSE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    // Kick off!
    kicker.Set();

    EXPECT_EQ(1, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    // This will sleep for a while until evaluation completion.
    thread.Join();

    EXPECT_EQ(1, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_TRUE(target.IsSet());
}

TEST_F(TFutureTest, ApplyVoidToInt)
{
    int state = 0;

    auto kicker = NewPromise<void>();

    auto source = kicker.ToFuture();
    auto target = source
        .Apply(BIND([&] () -> int {
            ++state;
            return 17;
        }));

    // Initial computation condition.
    EXPECT_EQ(0, state);
    EXPECT_FALSE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    // Kick off!
    kicker.Set();

    EXPECT_EQ(1, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_TRUE(target.IsSet());

    EXPECT_EQ(17, target.Get().Value());
}

TEST_F(TFutureTest, ApplyVoidToFutureInt)
{
    int state = 0;

    auto kicker = NewPromise<void>();
    auto setter = NewPromise<int>();

    TThread thread(&AsynchronousIntSetter, &setter);

    auto source = kicker.ToFuture();
    auto  target = source
        .Apply(BIND([&] () -> TFuture<int> {
            ++state;
            thread.Start();
            return setter.ToFuture();
        }));

    // Ensure that thread was not started.
    Sleep(SleepQuantum * 2);

    // Initial computation condition.
    EXPECT_EQ(0, state);
    EXPECT_FALSE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    // Kick off!
    kicker.Set();

    EXPECT_EQ(1, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    // This will sleep for a while until evaluation completion.
    thread.Join();

    EXPECT_EQ(1, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_TRUE(target.IsSet());

    EXPECT_EQ(42, target.Get().Value());
}

TEST_F(TFutureTest, ApplyIntToVoid)
{
    int state = 0;

    auto kicker = NewPromise<int>();

    auto  source = kicker.ToFuture();
    auto target = source
        .Apply(BIND([&] (int x) -> void { state += x; }));

    EXPECT_EQ(0, state);
    EXPECT_FALSE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    kicker.Set(21);

    EXPECT_EQ(21, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_TRUE(target.IsSet());

    EXPECT_EQ(21, source.Get().Value());
}

TEST_F(TFutureTest, ApplyIntToFutureVoid)
{
    int state = 0;

    auto kicker = NewPromise<int>();
    auto setter = NewPromise<void>();

    TThread thread(&AsynchronousVoidSetter, &setter);

    auto source = kicker.ToFuture();
    auto target = source
        .Apply(BIND([&] (int x) -> TFuture<void> {
            state += x;
            thread.Start();
            return setter.ToFuture();
        }));

    // Ensure that thread was not started.
    Sleep(SleepQuantum * 2);

    // Initial computation condition.
    EXPECT_EQ(0, state);
    EXPECT_FALSE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    // Kick off!
    kicker.Set(21);

    EXPECT_EQ(21, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    EXPECT_EQ(21, source.Get().Value());

    // This will sleep for a while until evaluation completion.
    thread.Join();

    EXPECT_EQ(21, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_TRUE(target.IsSet());
}

TEST_F(TFutureTest, ApplyIntToInt)
{
    int state = 0;

    auto kicker = NewPromise<int>();

    auto source = kicker.ToFuture();
    auto target = source
        .Apply(BIND([&] (int x) -> int {
            state += x;
            return x * 2;
        }));

    EXPECT_EQ(0, state);
    EXPECT_FALSE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    kicker.Set(21);

    EXPECT_EQ(21, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_TRUE(target.IsSet());

    EXPECT_EQ(21, source.Get().Value());
    EXPECT_EQ(42, target.Get().Value());
}

TEST_F(TFutureTest, ApplyIntToFutureInt)
{
    int state = 0;

    auto kicker = NewPromise<int>();
    auto setter = NewPromise<int>();

    TThread thread(&AsynchronousIntSetter, &setter);

    auto source = kicker.ToFuture();
    auto target = source
        .Apply(BIND([&] (int x) -> TFuture<int> {
            state += x;
            thread.Start();
            return setter.ToFuture();
        }));

    // Ensure that thread was not started.
    Sleep(SleepQuantum * 2);

    // Initial computation condition.
    EXPECT_EQ(0, state);
    EXPECT_FALSE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    // Kick off!
    kicker.Set(21);

    EXPECT_EQ(21, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_FALSE(target.IsSet());

    EXPECT_EQ(21, source.Get().Value());

    // This will sleep for a while until evaluation completion.
    thread.Join();

    EXPECT_EQ(21, state);
    EXPECT_TRUE(source.IsSet());
    EXPECT_TRUE(target.IsSet());

    EXPECT_EQ(21, source.Get().Value());
    EXPECT_EQ(42, target.Get().Value());
}

static TFuture<int> AsyncDivide(int a, int b, TDuration delay)
{
    auto promise = NewPromise<int>();
    TDelayedExecutor::Submit(
        BIND([=] () mutable {
            if (b == 0) {
                promise.Set(TError("Division by zero"));
            } else {
                promise.Set(a / b);
            }
        }),
        delay);
    return promise;
}

TEST_F(TFutureTest, TestCancelDelayed)
{
    auto future = NConcurrency::TDelayedExecutor::MakeDelayed(TDuration::Seconds(10));
    future.Cancel(TError("Canceled"));
    EXPECT_TRUE(future.IsSet());
    EXPECT_FALSE(future.Get().IsOK());
}

TEST_F(TFutureTest, AnyOf)
{
    std::vector<TFuture<int>> futures{
        AsyncDivide(5, 2, TDuration::Seconds(0.1)),
        AsyncDivide(30, 3, TDuration::Seconds(0.2))
    };
    auto resultOrError = AnyOf(futures).Get();
    EXPECT_TRUE(resultOrError.IsOK());
    auto result = resultOrError.Value();
    EXPECT_EQ(2, result);
}

TEST_F(TFutureTest, AnyOfEmpty)
{
    std::vector<TFuture<int>> futures;
    auto error = AnyOf(futures).Get();
    EXPECT_EQ(NYT::EErrorCode::FutureCombinerFailure, error.GetCode());
}

TEST_F(TFutureTest, AnyOfSkipError)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    auto p3 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture(),
        p3.ToFuture()
    };
    auto f = AnyOf(futures);
    EXPECT_FALSE(f.IsSet());
    EXPECT_FALSE(p2.IsCanceled());
    p1.Set(TError("oops"));
    EXPECT_FALSE(f.IsSet());
    p2.Set(123);
    EXPECT_TRUE(f.IsSet());
    auto result = f.Get();
    EXPECT_TRUE(result.IsOK());
    EXPECT_EQ(123, result.Value());
    EXPECT_TRUE(p3.IsCanceled());
}

TEST_F(TFutureTest, AnyOfSuccessShortcut)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AnyOf(futures);
    EXPECT_FALSE(f.IsSet());
    EXPECT_FALSE(p2.IsCanceled());
    p1.Set(1);
    EXPECT_TRUE(f.IsSet());
    auto result = f.Get();
    EXPECT_TRUE(result.IsOK());
    EXPECT_EQ(1, result.Value());
    EXPECT_TRUE(p2.IsCanceled());
}

TEST_F(TFutureTest, AnyOfDontCancelOnShortcut)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AnyOf(
        futures,
        TSkipErrorPolicy{},
        TFutureCombinerOptions{.CancelInputOnShortcut = false});
    p1.Set(1);
    EXPECT_FALSE(p2.IsCanceled());
}

TEST_F(TFutureTest, AnyOfPropagateCancelation)
{
    auto p1 = NewPromise<void>();
    auto p2 = NewPromise<void>();
    std::vector<TFuture<void>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AnyOf(futures);
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
    f.Cancel(TError("oops"));
    EXPECT_TRUE(p1.IsCanceled());
    EXPECT_TRUE(p2.IsCanceled());
}

TEST_F(TFutureTest, AnyOfDontPropagateCancelation)
{
    auto p1 = NewPromise<void>();
    auto p2 = NewPromise<void>();
    std::vector<TFuture<void>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AnyOf(
        futures,
        TSkipErrorPolicy{},
        TFutureCombinerOptions{.PropagateCancelationToInput = false});
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
    f.Cancel(TError("oops"));
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
}

TEST_F(TFutureTest, AnyOf1)
{
    auto promise = NewPromise<int>();
    auto future = promise.ToFuture();
    std::vector<TFuture<int>> futures{
        future
    };
    EXPECT_EQ(future, AnyOf(futures));
}

TEST_F(TFutureTest, AllOfEmpty)
{
    std::vector<TFuture<int>> futures{};
    auto resultOrError = AllOf(futures).Get();
    EXPECT_TRUE(resultOrError.IsOK());
    const auto& result = resultOrError.Value();
    EXPECT_TRUE(result.empty());
}

TEST_F(TFutureTest, AllOf)
{
    std::vector<TFuture<int>> futures{
        AsyncDivide(5, 2, TDuration::Seconds(0.1)),
        AsyncDivide(30, 3, TDuration::Seconds(0.2))
    };
    auto resultOrError = AllOf(futures).Get();
    EXPECT_TRUE(resultOrError.IsOK());
    const auto& result = resultOrError.Value();
    EXPECT_EQ(2, result.size());
    EXPECT_EQ(2, result[0]);
    EXPECT_EQ(10, result[1]);
}

TEST_F(TFutureTest, AllOfError)
{
    std::vector<TFuture<int>> futures{
        AsyncDivide(5, 2, TDuration::Seconds(0.1)),
        AsyncDivide(30, 0, TDuration::Seconds(0.2))
    };
    auto resultOrError = AllOf(futures).Get();
    EXPECT_FALSE(resultOrError.IsOK());
}

TEST_F(TFutureTest, AllOfFailureShortcut)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AllOf(futures);
    EXPECT_FALSE(f.IsSet());
    EXPECT_FALSE(p2.IsCanceled());
    p1.Set(TError("oops"));
    EXPECT_TRUE(f.IsSet());
    auto result = f.Get();
    EXPECT_FALSE(result.IsOK());
    EXPECT_TRUE(p2.IsCanceled());
}

TEST_F(TFutureTest, AllOfDontCancelOnShortcut)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AllOf(
        futures,
        TPropagateErrorPolicy{},
        TFutureCombinerOptions{.CancelInputOnShortcut = false});
    p1.Set(TError("oops"));
    EXPECT_FALSE(p2.IsCanceled());
}

TEST_F(TFutureTest, AllOfCancel)
{
    std::vector<TFuture<void>> futures{
        TDelayedExecutor::MakeDelayed(TDuration::Seconds(5)),
        TDelayedExecutor::MakeDelayed(TDuration::Seconds(5)),
        TDelayedExecutor::MakeDelayed(TDuration::Seconds(5))
    };
    auto f = AllOf(futures);
    f.Cancel(TError("Error"));
    EXPECT_TRUE(f.IsSet());
    const auto& result = f.Get();
    EXPECT_EQ(NYT::EErrorCode::Canceled, result.GetCode());
}

TEST_F(TFutureTest, AllOfVoid0)
{
    std::vector<TFuture<void>> futures;
    EXPECT_EQ(VoidFuture, AllOf(futures));
}

TEST_F(TFutureTest, AllOfVoid1)
{
    auto promise = NewPromise<void>();
    auto future = promise.ToFuture();
    std::vector<TFuture<void>> futures{
        future
    };
    EXPECT_EQ(future, AllOf(futures));
}

TEST_F(TFutureTest, AllOfRetainError)
{
    std::vector<TFuture<int>> futures{
        AsyncDivide(5, 2, TDuration::Seconds(0.1)),
        AsyncDivide(30, 0, TDuration::Seconds(0.2))
    };
    auto resultOrError = AllOf(futures, TRetainErrorPolicy{}).Get();
    EXPECT_TRUE(resultOrError.IsOK());
    const auto& result = resultOrError.Value();
    EXPECT_EQ(2, result.size());
    EXPECT_TRUE(result[0].IsOK());
    EXPECT_EQ(2, result[0].Value());
    EXPECT_FALSE(result[1].IsOK());
}

TEST_F(TFutureTest, AllOfPropagateCancelation)
{
    auto p1 = NewPromise<void>();
    auto p2 = NewPromise<void>();
    std::vector<TFuture<void>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AllOf(futures);
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
    f.Cancel(TError("oops"));
    EXPECT_TRUE(p1.IsCanceled());
    EXPECT_TRUE(p2.IsCanceled());
}

TEST_F(TFutureTest, AllOfDontPropagateCancelation)
{
    auto p1 = NewPromise<void>();
    auto p2 = NewPromise<void>();
    std::vector<TFuture<void>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AllOf(
        futures,
        TPropagateErrorPolicy{},
        TFutureCombinerOptions{.PropagateCancelationToInput = false});
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
    f.Cancel(TError("oops"));
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfEmpty)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AnyNOf(futures, 0);
    EXPECT_TRUE(f.IsSet());
    const auto& resultOrError = f.Get();
    EXPECT_TRUE(resultOrError.IsOK());
    const auto& result = resultOrError.Value();
    EXPECT_TRUE(result.empty());
    EXPECT_TRUE(p1.IsCanceled());
    EXPECT_TRUE(p2.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfDontCancelOnEmptyShortcut)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AnyNOf(
        futures,
        0,
        TSkipErrorPolicy{},
        TFutureCombinerOptions{.CancelInputOnShortcut = false});
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfInsufficientInputs)
{
    auto p1 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture()
    };
    auto f = AnyNOf(futures, 2);
    EXPECT_TRUE(f.IsSet());
    const auto& resultOrError = f.Get();
    EXPECT_EQ(NYT::EErrorCode::FutureCombinerFailure, resultOrError.GetCode());
    EXPECT_TRUE(p1.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfDontCancelOnInsufficientInputsShortcut)
{
    auto p1 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture()
    };
    auto f = AnyNOf(
        futures,
        2,
        TSkipErrorPolicy{},
        TFutureCombinerOptions{.CancelInputOnShortcut = false});
    EXPECT_FALSE(p1.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfTooManyFailures)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    auto p3 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture(),
        p3.ToFuture()
    };
    auto f = AnyNOf(
        futures,
        2,
        TSkipErrorPolicy{});
    EXPECT_FALSE(f.IsSet());
    EXPECT_FALSE(p3.IsCanceled());
    p1.Set(TError("oops1"));
    p2.Set(TError("oops2"));
    EXPECT_TRUE(f.IsSet());
    const auto& resultOrError = f.Get();
    EXPECT_EQ(NYT::EErrorCode::FutureCombinerFailure, resultOrError.GetCode());
    EXPECT_TRUE(p3.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfDontCancelOnTooManyFailuresShortcut)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    auto p3 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture(),
        p3.ToFuture()
    };
    auto f = AnyNOf(
        futures,
        2,
        TSkipErrorPolicy{},
        TFutureCombinerOptions{.CancelInputOnShortcut = false});
    p1.Set(TError("oops1"));
    p2.Set(TError("oops2"));
    EXPECT_FALSE(p3.IsCanceled());
}

TEST_F(TFutureTest, AnyNOf)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    auto p3 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture(),
        p3.ToFuture()
    };
    auto f = AnyNOf(futures, 2);
    EXPECT_FALSE(f.IsSet());
    EXPECT_FALSE(p1.IsCanceled());
    p2.Set(1);
    p3.Set(2);
    EXPECT_TRUE(f.IsSet());
    const auto& resultOrError = f.Get();
    EXPECT_TRUE(resultOrError.IsOK());
    auto result = resultOrError.Value();
    std::sort(result.begin(), result.end());
    EXPECT_EQ(2, result.size());
    EXPECT_EQ(1, result[0]);
    EXPECT_EQ(2, result[1]);
    EXPECT_TRUE(p1.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfDontCancelOnShortcut)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    auto p3 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture(),
        p3.ToFuture()
    };
    auto f = AnyNOf(
        futures,
        2,
        TSkipErrorPolicy{},
        TFutureCombinerOptions{.CancelInputOnShortcut = false});
    p2.Set(1);
    p3.Set(2);
    EXPECT_FALSE(p1.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfDontCancelOnPropagateErrorShortcut)
{
    auto p1 = NewPromise<int>();
    auto p2 = NewPromise<int>();
    auto p3 = NewPromise<int>();
    std::vector<TFuture<int>> futures{
        p1.ToFuture(),
        p2.ToFuture(),
        p3.ToFuture()
    };
    auto f = AnyNOf(
        futures,
        2,
        TSkipErrorPolicy{},
        TFutureCombinerOptions{.CancelInputOnShortcut = false});
    p3.Set(TError("oops"));
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfVoid1)
{
    auto promise = NewPromise<void>();
    auto future = promise.ToFuture();
    std::vector<TFuture<void>> futures{
        future
    };
    EXPECT_EQ(future, AnyNOf(futures, 1));
}

TEST_F(TFutureTest, AnyNOfRetainError)
{
    std::vector<TFuture<int>> futures{
        AsyncDivide(5, 2, TDuration::Seconds(0.1)),
        AsyncDivide(10, 5, TDuration::Seconds(0.3)),
        AsyncDivide(30, 0, TDuration::Seconds(0.2))
    };
    auto resultOrError = AnyNOf(futures, 2, TRetainErrorPolicy{}).Get();
    EXPECT_TRUE(resultOrError.IsOK());
    const auto& result = resultOrError.Value();
    EXPECT_EQ(2, result.size());
    EXPECT_TRUE(result[0].IsOK());
    EXPECT_EQ(2, result[0].Value());
    EXPECT_FALSE(result[1].IsOK());
}

TEST_F(TFutureTest, AnyNOfPropagateCancelation)
{
    auto p1 = NewPromise<void>();
    auto p2 = NewPromise<void>();
    std::vector<TFuture<void>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AnyNOf(futures, 1);
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
    f.Cancel(TError("oops"));
    EXPECT_TRUE(p1.IsCanceled());
    EXPECT_TRUE(p2.IsCanceled());
}

TEST_F(TFutureTest, AnyNOfDontPropagateCancelation)
{
    auto p1 = NewPromise<void>();
    auto p2 = NewPromise<void>();
    std::vector<TFuture<void>> futures{
        p1.ToFuture(),
        p2.ToFuture()
    };
    auto f = AnyNOf(
        futures,
        1,
        TSkipErrorPolicy{},
        TFutureCombinerOptions{.PropagateCancelationToInput = false});
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
    f.Cancel(TError("oops"));
    EXPECT_FALSE(p1.IsCanceled());
    EXPECT_FALSE(p2.IsCanceled());
}

TEST_F(TFutureTest, AsyncViaCanceledInvoker)
{
    auto context = New<TCancelableContext>();
    auto invoker = context->CreateInvoker(GetSyncInvoker());
    auto generator = BIND([] () {}).AsyncVia(invoker);
    context->Cancel(TError("Error"));
    auto future = generator.Run();
    auto error = future.Get();
    ASSERT_EQ(NYT::EErrorCode::Canceled, error.GetCode());
}

TEST_F(TFutureTest, LastPromiseDied)
{
    TFuture<void> future;
    {
        auto promise = NewPromise<void>();
        future = promise;
        EXPECT_FALSE(future.IsSet());
    }
    Sleep(SleepQuantum);
    EXPECT_TRUE(future.IsSet());
    EXPECT_EQ(NYT::EErrorCode::Canceled, future.Get().GetCode());
}

TEST_F(TFutureTest, PropagateErrorSync)
{
    auto p = NewPromise<int>();
    auto f1 = p.ToFuture();
    auto f2 = f1.Apply(BIND([] (int x) { return x + 1; }));
    p.Set(TError("Oops"));
    EXPECT_TRUE(f2.IsSet());
    EXPECT_FALSE(f2.Get().IsOK());
}

TEST_F(TFutureTest, PropagateErrorAsync)
{
    auto p = NewPromise<int>();
    auto f1 = p.ToFuture();
    auto f2 = f1.Apply(BIND([] (int x) { return MakeFuture(x + 1);}));
    p.Set(TError("Oops"));
    EXPECT_TRUE(f2.IsSet());
    EXPECT_FALSE(f2.Get().IsOK());
}

TEST_F(TFutureTest, WithTimeoutSuccess)
{
    auto p = NewPromise<void>();
    auto f1 = p.ToFuture();
    auto f2 = f1.WithTimeout(TDuration::MilliSeconds(100));
    Sleep(TDuration::MilliSeconds(10));
    p.Set();
    EXPECT_TRUE(f2.Get().IsOK());
}

TEST_F(TFutureTest, WithTimeoutOnSet)
{
    auto p = NewPromise<void>();
    p.Set();
    auto f1 = p.ToFuture();
    auto f2 = f1.WithTimeout(TDuration::MilliSeconds(0));
    EXPECT_TRUE(f1.Get().IsOK());
    EXPECT_TRUE(f2.Get().IsOK());
}

TEST_F(TFutureTest, WithTimeoutFail)
{
    auto p = NewPromise<int>();
    auto f1 = p.ToFuture();
    auto f2 = f1.WithTimeout(SleepQuantum);
    EXPECT_EQ(NYT::EErrorCode::Timeout, f2.Get().GetCode());
}

TEST_W(TFutureTest, Holder)
{
    auto promise = NewPromise<void>();
    auto future = promise.ToFuture();
    {
        TFutureHolder<void> holder(future);
    }
    EXPECT_TRUE(future.IsSet());
    EXPECT_TRUE(promise.IsCanceled());
}

TEST_F(TFutureTest, JustAbanbon)
{
    NewPromise<void>();
}

TEST_F(TFutureTest, AbanbonIsSet)
{
    auto promise = NewPromise<void>();
    auto future = promise.ToFuture();
    promise.Reset();
    EXPECT_TRUE(future.IsSet());
}

TEST_F(TFutureTest, AbanbonTryGet)
{
    auto promise = NewPromise<void>();
    auto future = promise.ToFuture();
    promise.Reset();
    EXPECT_EQ(EErrorCode::Canceled, future.TryGet()->GetCode());
}

TEST_F(TFutureTest, AbanbonGet)
{
    auto promise = NewPromise<void>();
    auto future = promise.ToFuture();
    promise.Reset();
    EXPECT_EQ(EErrorCode::Canceled, future.Get().GetCode());
}

TEST_F(TFutureTest, AbanbonSubscribe)
{
    auto promise = NewPromise<void>();
    auto future = promise.ToFuture();
    promise.Reset();
    bool called = false;
    future.Subscribe(BIND([&] (const TError&) mutable { called = true; }));
    EXPECT_TRUE(called);
}

TEST_F(TFutureTest, SubscribeAbanbon)
{
    bool called = false;
    auto promise = NewPromise<void>();
    auto future = promise.ToFuture();
    future.Subscribe(BIND([&] (const TError&) mutable {
        VERIFY_INVOKER_AFFINITY(GetFinalizerInvoker());
        called = true;
    }));
    promise.Reset();
    Sleep(SleepQuantum);
    EXPECT_TRUE(called);
}

TEST_F(TFutureTest, OnCanceledAbanbon)
{
    bool called = false;
    auto promise = NewPromise<void>();
    auto future = promise.ToFuture();
    promise.OnCanceled(BIND([&] (const TError& /*error*/) {
        called = true;
    }));
    promise.Reset();
    Sleep(SleepQuantum);
    EXPECT_FALSE(called);
}

TString OnCallResult(const TErrorOr<int>& callResult)
{
    THROW_ERROR_EXCEPTION("Call failed");
}

TEST_F(TFutureTest, LtoCrash)
{
    auto future = MakeFuture<int>(0);
    auto nextFuture = future.Apply(BIND(OnCallResult));
}

struct S
{
    static int DestroyedCounter;

    ~S()
    {
        ++DestroyedCounter;
    }
};

int S::DestroyedCounter = 0;

TEST_F(TFutureTest, CancelableDoesNotProhibitDestruction)
{
    auto promise = NewPromise<S>();
    promise.Set(S());

    auto cancelable = promise.ToFuture().AsCancelable();

    auto before = S::DestroyedCounter;
    promise.Reset();
    auto after = S::DestroyedCounter;
    EXPECT_EQ(1, after - before);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT
