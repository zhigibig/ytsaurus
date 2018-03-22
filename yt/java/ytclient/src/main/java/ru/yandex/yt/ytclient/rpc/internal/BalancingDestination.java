package ru.yandex.yt.ytclient.rpc.internal;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledExecutorService;

import ru.yandex.inside.yt.kosher.common.GUID;
import ru.yandex.yt.rpcproxy.ETransactionType;
import ru.yandex.yt.rpcproxy.TReqPingTransaction;
import ru.yandex.yt.rpcproxy.TReqStartTransaction;
import ru.yandex.yt.rpcproxy.TRspPingTransaction;
import ru.yandex.yt.rpcproxy.TRspStartTransaction;
import ru.yandex.yt.ytclient.proxy.ApiService;
import ru.yandex.yt.ytclient.proxy.ApiServiceUtil;
import ru.yandex.yt.ytclient.rpc.RpcClient;
import ru.yandex.yt.ytclient.rpc.RpcClientRequest;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestBuilder;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestControl;
import ru.yandex.yt.ytclient.rpc.RpcClientResponse;
import ru.yandex.yt.ytclient.rpc.RpcClientResponseHandler;
import ru.yandex.yt.ytclient.rpc.RpcOptions;
import ru.yandex.yt.ytclient.rpc.RpcUtil;
import ru.yandex.yt.ytclient.rpc.internal.metrics.BalancingDestinationMetricsHolder;
import ru.yandex.yt.ytclient.rpc.internal.metrics.BalancingDestinationMetricsHolderImpl;

/**
 * @author aozeritsky
 *
 * TODO: move to proxy
 */
public class BalancingDestination {
    private final BalancingDestinationMetricsHolder metricsHolder;

    private final String dc;
    private final RpcClient client;
    private final String id;
    private int index;

    private final ApiService service;
    private GUID transaction = null;

    private final String destinationName;

    public BalancingDestination(String dc, RpcClient client, int index) {
        this(dc, client, index, new BalancingDestinationMetricsHolderImpl());
    }

    public BalancingDestination(String dc, RpcClient client, int index, BalancingDestinationMetricsHolder metricsHolder) {
        this(dc, client, index, metricsHolder, new RpcOptions());
    }

    public BalancingDestination(String dc, RpcClient client, int index, BalancingDestinationMetricsHolder metricsHolder, RpcOptions options) {
        this.dc = dc;
        this.client = Objects.requireNonNull(client);
        this.index = index;
        this.id = String.format("%s/%s", dc, client.toString());

        this.destinationName = client.destinationName();
        this.metricsHolder = metricsHolder;

        service = client.getService(ApiService.class, options);
    }

    /* for testing only */
    public BalancingDestination(String dc, int index) {
        this.dc = dc;
        BalancingDestination parent = this;
        this.client = new RpcClient() {
            @Override
            public void close() { }

            @Override
            public RpcClientRequestControl send(RpcClient unused, RpcClientRequest request, RpcClientResponseHandler handler) {
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

            @Override
            public String toString() {
                return parent.toString();
            }
        };
        this.id = String.format("%s/%d", dc, index);
        this.index = index;

        this.destinationName = "local";
        this.metricsHolder = new BalancingDestinationMetricsHolderImpl();

        service = null;
    }

    public double weight() {
        return metricsHolder.getLocal99thPercentile(destinationName);
    }

    public String dataCenter() {
        return dc;
    }

    public int getIndex() {
        return index;
    }

    public void setIndex(int i) {
        index = i;
    }

    public RpcClient getClient() {
        return client;
    }

    public void close() {
        client.close();
    }

    public CompletableFuture<GUID> createTransaction(Duration timeout) {
        if (transaction == null) {
            RpcClientRequestBuilder<TReqStartTransaction.Builder, RpcClientResponse<TRspStartTransaction>> builder =
                service.startTransaction();
            builder.body().setTimeout(ApiServiceUtil.durationToYtMicros(timeout.multipliedBy(2)));
            builder.body().setType(ETransactionType.TT_TABLET);
            builder.body().setSticky(true);
            return RpcUtil.apply(builder.invoke(), response -> {
                GUID id = RpcUtil.fromProto(response.body().getId());
                return id;
            });
        } else {
            return CompletableFuture.completedFuture(transaction);
        }
    }

    public CompletableFuture<Void> pingTransaction(GUID id) {
        RpcClientRequestBuilder<TReqPingTransaction.Builder, RpcClientResponse<TRspPingTransaction>> builder =
            service.pingTransaction();
        builder.body().setTransactionId(RpcUtil.toProto(id));
        builder.body().setSticky(true);

        long start = System.nanoTime();

        return RpcUtil.apply(builder.invoke(), response -> null).thenAccept(unused -> {
            transaction = id;

            long end = System.nanoTime();
            long interval = (end - start) / 1000000;
            metricsHolder.updateLocal(destinationName, interval);
            metricsHolder.updateDc(dc, interval);
        });
    }

    public void resetTransaction() {
        transaction = null;
    }

    public String toString() {
        return id;
    }
}
