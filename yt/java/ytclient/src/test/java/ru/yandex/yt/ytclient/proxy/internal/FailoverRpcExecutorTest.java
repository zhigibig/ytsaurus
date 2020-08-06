package ru.yandex.yt.ytclient.proxy.internal;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

import org.junit.After;
import org.junit.Test;

import ru.yandex.inside.yt.kosher.impl.common.YtException;
import ru.yandex.yt.rpc.TRequestHeader;
import ru.yandex.yt.rpc.TResponseHeader;
import ru.yandex.yt.ytclient.rpc.RpcClient;
import ru.yandex.yt.ytclient.rpc.RpcClientPool;
import ru.yandex.yt.ytclient.rpc.RpcClientRequest;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestControl;
import ru.yandex.yt.ytclient.rpc.RpcClientResponseHandler;
import ru.yandex.yt.ytclient.rpc.RpcClientStreamControl;
import ru.yandex.yt.ytclient.rpc.RpcFailoverPolicy;
import ru.yandex.yt.ytclient.rpc.RpcOptions;

import static org.hamcrest.MatcherAssert.assertThat;
import static org.hamcrest.Matchers.containsString;
import static org.hamcrest.Matchers.instanceOf;
import static org.hamcrest.Matchers.is;
import static org.hamcrest.Matchers.notNullValue;
import static org.junit.Assert.fail;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

public class FailoverRpcExecutorTest {
    ScheduledExecutorService executorService = new ScheduledThreadPoolExecutor(4);

    @After
    public void after() {
        executorService.shutdownNow();
    }

    @Test
    public void testCancel() {
        CompletableFuture<String> result = new CompletableFuture<>();

        RpcClientRequestControl c = execute(responseNever(), defaultOptions(), result, 2, 2);
        c.cancel();
        waitFuture(result, 10);

        assertThat(result.isDone(), is(true));
        assertThat(result.isCancelled(), is(true));
    }

    @Test
    public void testSuccess() throws Exception {
        CompletableFuture<String> result = new CompletableFuture<>();
        execute(responseImmediately(),  defaultOptions(), result, 2, 2);

        waitFuture(result, 1000);

        assertThat(result.isDone(), is(true));
        assertThat(result.isCompletedExceptionally(), is(false));
        assertThat(result.get(), is("response"));
    }

    @Test
    public void testGlobalTimeout() {
        CompletableFuture<String> result = new CompletableFuture<>();
        RpcOptions options = defaultOptions()
                .setGlobalTimeout(Duration.ofMillis(100))
                .setFailoverTimeout(Duration.ofMillis(100));

        execute(responseNever(), options, result, 2, 2);

        waitFuture(result, 1000);

        assertThat(result.isDone(), is(true));
        assertThat(result.isCompletedExceptionally(), is(true));
        assertThat(getError(result), instanceOf(TimeoutException.class));
    }

    @Test
    public void testFailover() throws Exception {
        CompletableFuture<String> result = new CompletableFuture<>();
        RpcOptions options = defaultOptions()
                .setGlobalTimeout(Duration.ofMillis(1000))
                .setFailoverTimeout(Duration.ofMillis(100));
        execute(responseOnSecondRequest(), options, result, 2, 2);

        waitFuture(result, 1000);

        assertThat(result.isDone(), is(true));
        assertThat(result.isCompletedExceptionally(), is(false));
        assertThat(result.get(), is("response"));
    }

    @Test
    public void testNoClientsInPool() {
        CompletableFuture<String> result = new CompletableFuture<>();

        execute(responseNever(), defaultOptions(), result, 0, 2);

        waitFuture(result, 1000);

        assertThat(result.isDone(), is(true));
        assertThat(result.isCompletedExceptionally(), is(true));
        assertThat(getError(result).toString(), containsString("pool is exhausted"));
    }

    @Test
    public void respondWithDelayPoolExhausted() throws ExecutionException, InterruptedException {
        CompletableFuture<String> result = new CompletableFuture<>();

        Consumer<RpcClientResponseHandler> respondWithDelay = (handler) -> executorService.schedule(
                () -> handler.onResponse(null, null, null),
                100, TimeUnit.MILLISECONDS);

        execute(respondWithDelay, defaultOptions(), result, 1, 2);

        waitFuture(result, 1000);

        assertThat(result.isDone(), is(true));
        assertThat(result.isCompletedExceptionally(), is(false));
        assertThat(result.get(), is("response"));
    }

    @Test
    public void errorImmediatelyPoolExhausted() throws ExecutionException, InterruptedException {
        CompletableFuture<String> result = new CompletableFuture<>();

        AtomicInteger attempts = new AtomicInteger(0);
        Consumer<RpcClientResponseHandler> respondWithError = (handler) -> {
            attempts.incrementAndGet();
            handler.onError(new YtException("retriable error"));
        };

        execute(respondWithError, defaultOptions(), result, 2, 3);

        waitFuture(result, 1000);

        assertThat(result.isDone(), is(true));
        assertThat(result.isCompletedExceptionally(), is(true));
        assertThat(getError(result).toString(), containsString("retriable error"));
        assertThat(attempts.get(), is(2));
    }

    @Test
    public void errorWithDelayPoolExhausted() throws ExecutionException, InterruptedException {
        CompletableFuture<String> result = new CompletableFuture<>();

        RpcOptions options = new RpcOptions()
                .setGlobalTimeout(Duration.ofMillis(1000))
                .setFailoverTimeout(Duration.ofMillis(20));

        Consumer<RpcClientResponseHandler> respondWithDelay = (handler) -> executorService.schedule(
                () -> handler.onError(new YtException("our test error")),
                100, TimeUnit.MILLISECONDS);

        execute(respondWithDelay, new RpcOptions(), result, 1, 2);

        waitFuture(result, 1000);

        assertThat(result.isDone(), is(true));
        assertThat(result.isCompletedExceptionally(), is(true));
        assertThat(getError(result).toString(), containsString("our test error"));
    }

    private RpcClient createClient(Consumer<RpcClientResponseHandler> handlerConsumer) {
        RpcClientRequestControl control = mock(RpcClientRequestControl.class);
        return new RpcClient() {
            @Override
            public void close() {}

            @Override
            public RpcClientRequestControl send(RpcClient sender, RpcClientRequest request, RpcClientResponseHandler handler) {
                handlerConsumer.accept(handler);
                return control;
            }

            @Override
            public RpcClientStreamControl startStream(RpcClient sender, RpcClientRequest request) {
                return null;
            }

            @Override
            public String destinationName() {
                return null;
            }

            @Override
            public ScheduledExecutorService executor() {
                return null;
            }
        };
    }

    private Consumer<RpcClientResponseHandler> responseNever() {
        return handler -> {};
    }

    private Consumer<RpcClientResponseHandler> responseImmediately() {
        return handler -> handler.onResponse(null, null, null);
    }

    private Consumer<RpcClientResponseHandler> responseOnSecondRequest() {
        return new Consumer<>() {
            int count = 1;
            @Override
            public void accept(RpcClientResponseHandler handler) {
                if (count >= 2) {
                    handler.onResponse(null, null, null);
                } else {
                    count += 1;
                }
            }
        };
    }

    private RpcClientRequestControl execute(
            Consumer<RpcClientResponseHandler> handlerConsumer,
            RpcOptions options,
            CompletableFuture<String> result,
            int clientCount,
            int attemptCount)
    {
        TRequestHeader.Builder header = TRequestHeader.newBuilder();
        RpcClientRequest request = mock(RpcClientRequest.class);
        when(request.getOptions()).thenReturn(options);
        when(request.header()).thenReturn(header);

        RpcClientResponseHandler handler = new RpcClientResponseHandler() {
            @Override
            public void onAcknowledgement(RpcClient sender) {
            }

            @Override
            public void onResponse(RpcClient sender, TResponseHeader header, List<byte[]> attachments) {
                result.complete("response");
            }

            @Override
            public void onError(Throwable error) {
                result.completeExceptionally(error);
            }

            @Override
            public void onCancel(CancellationException cancel) {
                result.completeExceptionally(cancel);
            }
        };

        List<RpcClient> clients = new ArrayList<>();
        RpcClient client = createClient(handlerConsumer);
        for (int i = 0; i < clientCount; ++i) {
            clients.add(client);
        }

        return FailoverRpcExecutor.execute(
                executorService,
                RpcClientPool.collectionPool(clients),
                request,
                handler,
                attemptCount);
    }

    private static void waitFuture(Future<?> future, long timeoutMillis) {
        try {
            future.get(timeoutMillis, TimeUnit.MILLISECONDS);
        } catch (InterruptedException | TimeoutException error) {
            fail(error.toString());
        } catch (ExecutionException | CancellationException error) {
            // that's ok
        }
    }

    private static Throwable getError(CompletableFuture<?> future) {
        assertThat(future.isCompletedExceptionally(), is(true));

        Throwable t = null;
        try {
            future.get();
            fail("expected exception to be thrown");
        } catch (ExecutionException error) {
            t = error.getCause();
        } catch (InterruptedException error) {
            fail("unexpected error: " + error.toString());
        }
        assertThat(t, is(notNullValue()));
        return t;
    }

    static RpcOptions defaultOptions() {
        RpcOptions result = new RpcOptions();
        result.setFailoverPolicy(new TestFailoverPolicy());
        return result;
    }

    private static class TestFailoverPolicy implements RpcFailoverPolicy {
        @Override
        public boolean onError(RpcClientRequest request, Throwable error) {
            return error.getMessage().contains("retriable");
        }

        @Override
        public boolean onTimeout() {
            return true;
        }

        @Override
        public boolean randomizeDcs() {
            return false;
        }
    }
}
