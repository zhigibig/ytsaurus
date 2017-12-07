package ru.yandex.yt.ytclient.rpc.internal;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

import ru.yandex.yt.ytclient.rpc.RpcClient;
import ru.yandex.yt.ytclient.rpc.RpcClientRequest;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestControl;
import ru.yandex.yt.ytclient.rpc.RpcClientResponseHandler;
import ru.yandex.yt.ytclient.rpc.RpcFailoverPolicy;
import ru.yandex.yt.ytclient.rpc.internal.metrics.BalancingResponseHandlerMetricsHolder;
import ru.yandex.yt.ytclient.rpc.internal.metrics.BalancingResponseHandlerMetricsHolderImpl;

/**
 * @author aozeritsky
 */
public class BalancingResponseHandler implements RpcClientResponseHandler {
    // TODO: move somewhere to core
    private final BalancingResponseHandlerMetricsHolder metricsHolder;

    private final CompletableFuture<List<byte[]>> f;
    private List<BalancingDestination> clients;
    private final RpcClientRequest request;
    private final boolean isOneWay;
    private int step;
    private final List<RpcClientRequestControl> cancelation;
    private Future<?> timeoutFuture;
    final private ScheduledExecutorService executorService;
    final private RpcFailoverPolicy failoverPolicy;

    final private long failoverTimeout;
    private long timeout;

    public BalancingResponseHandler(
        ScheduledExecutorService executorService,
        RpcFailoverPolicy failoverPolicy,
        Duration globalTimeout,
        Duration failoverTimeout,
        CompletableFuture<List<byte[]>> f,
        RpcClientRequest request,
        List<BalancingDestination> clients)
    {
        this(executorService, failoverPolicy, globalTimeout, failoverTimeout, f, request, clients,
                new BalancingResponseHandlerMetricsHolderImpl());
    }

    public BalancingResponseHandler(
            ScheduledExecutorService executorService,
            RpcFailoverPolicy failoverPolicy,
            Duration globalTimeout,
            Duration failoverTimeout,
            CompletableFuture<List<byte[]>> f,
            RpcClientRequest request,
            List<BalancingDestination> clients,
            BalancingResponseHandlerMetricsHolder metricsHolder)
    {
        this.executorService = executorService;
        this.failoverPolicy = failoverPolicy;
        this.failoverTimeout = failoverTimeout.toMillis();

        this.f = f;
        this.request = request;
        this.isOneWay = request.isOneWay();
        this.clients = clients;

        this.metricsHolder = metricsHolder;

        timeout = globalTimeout.toMillis();

        cancelation = new ArrayList<>();
        timeoutFuture = CompletableFuture.completedFuture(0);
        step = 0;

        if (clients.isEmpty()) {
            f.completeExceptionally(new RuntimeException("empty destinations list"));
        } else {
            send();
        }
    }

    private void send() {
        if (timeout <= 0) {
            f.completeExceptionally(new RuntimeException("request timeout"));
            return;
        }

        RpcClient client;
        BalancingDestination dst = clients.get(0);
        clients = clients.subList(1, clients.size());
        client = dst.getClient();

        if (step > 0) {
           metricsHolder.failoverInc();
        }

        step ++;

        metricsHolder.inflightInc();
        metricsHolder.totalInc();

        request.header().setTimeout(timeout*1000); // in microseconds
        cancelation.add(client.send(request, this));

        // schedule next step
        executorService.schedule(() ->
                onTimeout(), failoverTimeout, TimeUnit.MILLISECONDS);

        timeout -= failoverTimeout;
    }

    private void onTimeout() {
        synchronized (f) {
            if (!f.isDone()) {
                if (!clients.isEmpty()){
                    if (failoverPolicy.onTimeout()) {
                        send();
                    } else {
                        f.completeExceptionally(new RuntimeException("timeout"));
                    }
                } else {
                    // global timeout
                }
            }
        }
    }

    public void cancel() {
        synchronized (f) {
            for (RpcClientRequestControl control : cancelation) {
                metricsHolder.inflightDec();
                control.cancel();
            }
        }
    }

    @Override
    public void onAcknowledgement() {
        if (isOneWay) {
            synchronized (f) {
                if (!f.isDone()) {
                    f.complete(null);
                }
            }
        }
    }

    @Override
    public void onResponse(List<byte[]> attachments) {
        synchronized (f) {
            if (!f.isDone()) {
                if (isOneWay) {
                    // FATAL error, cannot recover
                    f.completeExceptionally(new IllegalStateException("Server replied to a one-way request"));
                } else {
                    f.complete(attachments);
                }
            }
        }
    }

    @Override
    public void onError(Throwable error) {
        synchronized (f) {
            if (!f.isDone()) {
                // maybe use other proxy here?
                if (failoverPolicy.onError(request, error) && !clients.isEmpty()) {
                    timeoutFuture.cancel(true);
                    send();
                } else {
                    f.completeExceptionally(error);
                }
            }
        }
    }
}
