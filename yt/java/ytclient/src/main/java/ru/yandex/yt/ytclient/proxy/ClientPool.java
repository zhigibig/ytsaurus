package ru.yandex.yt.ytclient.proxy;

import java.util.ArrayList;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Random;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.stream.Collectors;

import javax.annotation.Nullable;

import io.netty.channel.EventLoopGroup;
import org.asynchttpclient.AsyncHttpClient;
import org.asynchttpclient.DefaultAsyncHttpClientConfig;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.ytclient.misc.SerializedExecutorService;
import ru.yandex.yt.ytclient.proxy.internal.FailureDetectingRpcClient;
import ru.yandex.yt.ytclient.proxy.internal.HostPort;
import ru.yandex.yt.ytclient.proxy.internal.RpcClientFactory;
import ru.yandex.yt.ytclient.rpc.RpcClient;
import ru.yandex.yt.ytclient.rpc.RpcClientPool;
import ru.yandex.yt.ytclient.rpc.RpcError;
import ru.yandex.yt.ytclient.rpc.RpcOptions;
import ru.yandex.yt.ytclient.rpc.internal.metrics.DataCenterMetricsHolder;

import static org.asynchttpclient.Dsl.asyncHttpClient;

interface DataCenterRpcClientPool extends RpcClientPool {
    String getDataCenterName();
}

/**
 * This client pool tracks several data center pools.
 * If everything is ok it peeks clients from the local data center (or from data center with the lowest ping).
 *
 * When this data center goes down, pool switches to others.
 */
@NonNullFields
@NonNullApi
class MultiDcClientPool implements RpcClientPool {
    static final Logger logger = LoggerFactory.getLogger(MultiDcClientPool.class);

    final DataCenterRpcClientPool[] clientPools;
    @Nullable final DataCenterRpcClientPool localDcPool;
    final DataCenterMetricsHolder dcMetricHolder;

    static Builder builder() {
        return new Builder();
    }

    private MultiDcClientPool(Builder builder) {
        clientPools = builder.clientPools.toArray(new DataCenterRpcClientPool[0]);
        if (builder.localDc != null) {
            localDcPool = builder.clientPools.stream()
                    .filter((clientPool) -> clientPool.getDataCenterName().equals(builder.localDc))
                    .findFirst().orElse(null);
            if (localDcPool == null) {
                // N.B. actually we should throw exception
                // but by historical reasons we have to work in such conditions
                // At least we can complain.
                logger.error("Cannot find local datacenter: {} among: {}",
                        builder.localDc,
                        builder.clientPools.stream()
                                .map(DataCenterRpcClientPool::getDataCenterName)
                                .collect(Collectors.toList()));
            }
        } else {
            localDcPool = null;
        }
        dcMetricHolder = Objects.requireNonNull(builder.dcMetricHolder);
    }

    @Override
    public CompletableFuture<RpcClient> peekClient(CompletableFuture<?> releaseFuture) {
        // If local dc has immediate candidates return them.
        if (localDcPool != null) {
            CompletableFuture<RpcClient> localClientFuture = localDcPool.peekClient(releaseFuture);
            RpcClient localClient = getImmediateResult(localClientFuture);
            if (localClient != null) {
                return localClientFuture;
            } else {
                localClientFuture.cancel(true);
            }
        }

        // Try to find the best option among all immediate results.
        ArrayList<CompletableFuture<RpcClient>> futures = new ArrayList<>(clientPools.length);
        RpcClient resultClient = null;
        double resultPing = Double.MAX_VALUE;
        for (DataCenterRpcClientPool dc : clientPools) {
            CompletableFuture<RpcClient> f = dc.peekClient(releaseFuture);
            RpcClient client = getImmediateResult(f);
            if (client != null) {
                double currentPing = dcMetricHolder.getDc99thPercentile(dc.getDataCenterName());
                if (currentPing < resultPing) {
                    resultClient = client;
                    resultPing = currentPing;
                }
            } else {
                futures.add(f);
            }
        }

        if (resultClient != null) {
            for (CompletableFuture<RpcClient> future : futures) {
                future.cancel(true);
            }
            return CompletableFuture.completedFuture(resultClient);
        }

        // If all DCs are waiting for a client, then we have to wait first available proxy.
        CompletableFuture<RpcClient> resultFuture = new CompletableFuture<>();
        AtomicInteger errorCount = new AtomicInteger(0);
        final int errorCountLimit = futures.size();
        for (CompletableFuture<RpcClient> future : futures) {
            future.whenComplete((client, error) -> {
                if (error == null) {
                    resultFuture.complete(client);
                } else if (errorCount.incrementAndGet() == errorCountLimit) {
                    resultFuture.completeExceptionally(error);
                }
            });
            resultFuture.whenComplete((client, error) -> future.cancel(true));
        }
        return resultFuture;
    }

    @Nullable
    static private RpcClient getImmediateResult(CompletableFuture<RpcClient> future) {
        try {
            return future.getNow(null);
        } catch (Throwable error) {
            return null;
        }
    }

    @NonNullApi
    @NonNullFields
    static class Builder {
        @Nullable String localDc;
        List<DataCenterRpcClientPool> clientPools = new ArrayList<>();
        @Nullable DataCenterMetricsHolder dcMetricHolder = null;

        Builder setLocalDc(@Nullable String localDcName) {
            localDc = localDcName;
            return this;
        }

        Builder addClientPool(DataCenterRpcClientPool clientPool) {
            clientPools.add(clientPool);
            return this;
        }

        <T extends DataCenterRpcClientPool> Builder addClientPools(Collection<T> pools) {
            clientPools.addAll(pools);
            return this;
        }

        Builder setDcMetricHolder(DataCenterMetricsHolder dcMetricHolder) {
            this.dcMetricHolder = dcMetricHolder;
            return this;
        }

        MultiDcClientPool build() {
            return new MultiDcClientPool(this);
        }
    }
}

/**
 * This pool automatically discovers rpc proxy of a given YT cluster.
 * It can use http balancer or small number of known rpc proxies for bootstrap.
 */
@NonNullApi
@NonNullFields
class ClientPoolService extends ClientPool implements AutoCloseable {
    private static final Logger logger = LoggerFactory.getLogger(ClientPoolService.class);

    final ProxyGetter proxyGetter;
    final ScheduledExecutorService executorService;
    final long updatePeriodMs;
    final List<AutoCloseable> toClose = new ArrayList<>();

    boolean running = true;
    Future<?> nextUpdate = new CompletableFuture<>();

    static HttpBuilder httpBuilder() {
        return new HttpBuilder();
    }

    static RpcBuilder rpcBuilder() {
        return new RpcBuilder();
    }

    void start() {
        synchronized (this) {
            if (running) {
                nextUpdate = executorService.submit(this::doUpdate);
            } else {
                throw new IllegalArgumentException("ClientPoolService was already stopped");
            }
        }
    }

    @Override
    public void close() {
        synchronized (this) {
            running = false;
            nextUpdate.cancel(true);
        }

        Throwable error = null;
        for (AutoCloseable closable : toClose) {
            try {
                closable.close();
            } catch (Throwable t) {
                logger.error("Error while closing client pool service", t);
                error = t;
            }
        }
        if (error != null) {
            throw new RuntimeException(error);
        }
    }

    private void doUpdate() {
        CompletableFuture<List<HostPort>> proxiesFuture = proxyGetter.getProxies();
        proxiesFuture.whenCompleteAsync((result, error) -> {
            if (error == null) {
                logger.debug("Successfully discovered {} rpc proxies DataCenter: {}", result.size(), getDataCenterName());
                updateClients(result);
            } else {
                logger.warn("Failed to discover rpc proxies DataCenter: {} Error: ", getDataCenterName(), error);
                updateWithError(error);
            }

            synchronized (this) {
                if (running) {
                    nextUpdate = executorService.schedule(this::doUpdate, updatePeriodMs, TimeUnit.MILLISECONDS);
                }
            }
        }, executorService);
    }

    private ClientPoolService(HttpBuilder httpBuilder) {
        super(
                Objects.requireNonNull(httpBuilder.dataCenterName),
                Objects.requireNonNull(httpBuilder.options).getChannelPoolSize(),
                Objects.requireNonNull(httpBuilder.clientFactory),
                Objects.requireNonNull(httpBuilder.eventLoop),
                Objects.requireNonNull(httpBuilder.random)
        );
        AsyncHttpClient asyncHttpClient = asyncHttpClient(
                new DefaultAsyncHttpClientConfig.Builder()
                        .setThreadPoolName(httpBuilder.dataCenterName + "::periodicDiscovery")
                        .setEventLoopGroup(httpBuilder.eventLoop)
                        .setHttpClientCodecMaxHeaderSize(65536)
                        .build()
        );
        proxyGetter = new HttpProxyGetter(
                asyncHttpClient,
                Objects.requireNonNull(httpBuilder.balancerAddress),
                httpBuilder.role,
                httpBuilder.token
        );
        toClose.add(asyncHttpClient);

        executorService = httpBuilder.eventLoop;
        updatePeriodMs = httpBuilder.options.getProxyUpdateTimeout().toMillis();
    }

    private ClientPoolService(RpcBuilder rpcBuilder) {
        super(
                Objects.requireNonNull(rpcBuilder.dataCenterName),
                Objects.requireNonNull(rpcBuilder.options).getChannelPoolSize(),
                Objects.requireNonNull(rpcBuilder.clientFactory),
                Objects.requireNonNull(rpcBuilder.eventLoop),
                Objects.requireNonNull(rpcBuilder.random)
        );

        proxyGetter = new RpcProxyGetter(
                Objects.requireNonNull(rpcBuilder.initialProxyList),
                this,
                rpcBuilder.role,
                rpcBuilder.dataCenterName,
                rpcBuilder.clientFactory,
                rpcBuilder.options,
                rpcBuilder.random
        );

        executorService = rpcBuilder.eventLoop;
        updatePeriodMs = rpcBuilder.options.getProxyUpdateTimeout().toMillis();

        updateClients(rpcBuilder.initialProxyList);
    }

    static abstract class BaseBuilder<T extends BaseBuilder<T>> {
        @Nullable String role;
        @Nullable String dataCenterName;
        @Nullable RpcOptions options;
        @Nullable RpcClientFactory clientFactory;
        @Nullable EventLoopGroup eventLoop;
        @Nullable Random random;

        T setDataCenterName(String dataCenterName) {
            this.dataCenterName = dataCenterName;
            //noinspection unchecked
            return (T)this;
        }

        T setOptions(RpcOptions options) {
            this.options = options;
            //noinspection unchecked
            return (T)this;
        }

        T setClientFactory(RpcClientFactory clientFactory) {
            this.clientFactory = clientFactory;
            //noinspection unchecked
            return (T)this;
        }

        T setEventLoop(EventLoopGroup eventLoop) {
            this.eventLoop = eventLoop;
            //noinspection unchecked
            return (T)this;
        }

        T setRandom(Random random) {
            this.random = random;
            //noinspection unchecked
            return (T)this;
        }

        T setRole(@Nullable String role) {
            this.role = role;
            //noinspection unchecked
            return (T)this;
        }
    }

    /**
     * All setters with Nullable parameter are optional.
     * Other setters are required.
     */
    static class HttpBuilder extends BaseBuilder<HttpBuilder> {
        @Nullable String balancerAddress;
        @Nullable String token;

        HttpBuilder setBalancerAddress(String host, int port) {
            this.balancerAddress = host + ":" + port;
            return this;
        }

        /**
         * Set token. It might be null but that will work only on local test clusters.
         */
        HttpBuilder setToken(@Nullable String token) {
            this.token = token;
            return this;
        }

        ClientPoolService build() {
            return new ClientPoolService(this);
        }
    }

    static class RpcBuilder extends BaseBuilder<RpcBuilder> {
        @Nullable List<HostPort> initialProxyList;

        RpcBuilder setInitialProxyList(List<HostPort> initialProxyList) {
            this.initialProxyList = initialProxyList;
            return this;
        }

        ClientPoolService build() {
            return new ClientPoolService(this);
        }
    }
}

/**
 * Client pool tracks a list of RpcProxies can ban them or add new proxies.
 * It doesn't have a process that updates them automatically.
 */
@NonNullApi
@NonNullFields
class ClientPool implements DataCenterRpcClientPool {
    static private final Logger logger = LoggerFactory.getLogger(ClientPool.class);

    private final String dataCenterName;
    private final int maxSize;
    private final RpcClientFactory clientFactory;
    private final SerializedExecutorService safeExecutorService;
    private final Random random;

    // Healthy clients.
    private final Map<HostPort, PooledRpcClient> activeClients = new HashMap<>();

    private CompletableFuture<Void> nextUpdate = new CompletableFuture<>();

    // Array of healthy clients that are used for optimization of peekClient.
    private volatile PooledRpcClient[] clientCache = new PooledRpcClient[0];

    ClientPool(
            String dataCenterName,
            int maxSize,
            RpcClientFactory clientFactory,
            ExecutorService executorService,
            Random random)
    {
        this.dataCenterName = dataCenterName;
        this.safeExecutorService = new SerializedExecutorService(executorService);
        this.random = random;
        this.maxSize = maxSize;
        this.clientFactory = clientFactory;
    }

    @Override
    public CompletableFuture<RpcClient> peekClient(CompletableFuture<?> release) {
        PooledRpcClient[] goodClientsRef = clientCache;
        CompletableFuture<RpcClient> result = new CompletableFuture<>();
        if (!peekClientImpl(goodClientsRef, result, release)) {
            safeExecutorService.submit(()-> peekClientUnsafe(result, release));
        }
        return result;
    }

    CompletableFuture<Void> updateWithError(Throwable error) {
        return safeExecutorService.submit(() -> updateWithErrorUnsafe(error));
    }

    CompletableFuture<Void> updateClients(Collection<HostPort> proxies) {
        return safeExecutorService.submit(() -> updateClientsUnsafe(new HashSet<>(proxies)));
    }

    public String getDataCenterName() {
        return dataCenterName;
    }

    RpcClient[] getAliveClients() {
        PooledRpcClient[] tmp = this.clientCache;
        RpcClient[] result = new RpcClient[tmp.length];
        for (int i = 0; i < tmp.length; ++i) {
            result[i] = tmp[i].publicClient;
        }
        return result;
    }

    private void peekClientUnsafe(CompletableFuture<RpcClient> result, CompletableFuture<?> release) {
        if (peekClientImpl(clientCache, result, release)) {
            return;
        }
        nextUpdate.whenComplete((Void v, Throwable t) -> {
            if (peekClientImpl(clientCache, result, release)) {
                return;
            }
            RuntimeException error = new RuntimeException("Cannot get rpc proxies; DataCenter: " + dataCenterName);
            if (t != null) {
                error.initCause(t);
            }
            result.completeExceptionally(error);
        });
    }

    private boolean peekClientImpl(
            PooledRpcClient[] clients,
            CompletableFuture<RpcClient> result,
            CompletableFuture<?> release)
    {
        if (clients.length > 0) {
            PooledRpcClient pooledClient = clients[random.nextInt(clients.length)];
            if (!pooledClient.banned && pooledClient.ref()) {
                if (result.complete(pooledClient.publicClient)) {
                    release.whenComplete((o, throwable) -> pooledClient.unref());
                } else {
                    pooledClient.unref();
                }
            }
            return true;
        }
        return false;
    }

    private void banErrorClient(HostPort hostPort, Throwable error) {
        logger.warn("Client {} is banned due to error", hostPort, error);
        safeExecutorService.submit(() -> banClientUnsafe(hostPort));
    }

    private void updateClientsUnsafe(Set<HostPort> proxies) {
        for (PooledRpcClient pooledClient : activeClients.values()) {
            if (proxies.contains(pooledClient.hostPort)) {
                proxies.remove(pooledClient.hostPort);
            } else {
                banClientUnsafe(pooledClient.hostPort);
            }
        }

        for (HostPort hostPort : proxies) {
            if (activeClients.size() >= maxSize) {
                break;
            }

            RpcClient client = clientFactory.create(hostPort, dataCenterName);
            PooledRpcClient pooledClient = new PooledRpcClient(hostPort, client);
            activeClients.put(hostPort, pooledClient);
        }
        updateGoodClientsCacheUnsafe();
        CompletableFuture<Void> oldNextUpdate = nextUpdate;
        nextUpdate = new CompletableFuture<>();

        oldNextUpdate.complete(null);
    }

    private void updateWithErrorUnsafe(Throwable error) {
        CompletableFuture<Void> oldNextUpdate = nextUpdate;
        nextUpdate = new CompletableFuture<>();

        oldNextUpdate.completeExceptionally(error);
    }

    private void updateGoodClientsCacheUnsafe() {
        PooledRpcClient[] newCache = new PooledRpcClient[activeClients.size()];
        clientCache = activeClients.values().toArray(newCache);
    }

    private void banClientUnsafe(HostPort hostPort) {
        PooledRpcClient pooledClient = activeClients.get(hostPort);
        if (pooledClient.banned) {
            return;
        }
        pooledClient.banned = true;
        pooledClient.unref();

        updateGoodClientsCacheUnsafe();
    }

    @NonNullFields
    @NonNullApi
    class PooledRpcClient {
        final HostPort hostPort;
        final RpcClient internalClient;
        final RpcClient publicClient;

        volatile boolean banned = false;

        private final AtomicInteger referenceCounter = new AtomicInteger(1);

        PooledRpcClient(HostPort hostPort, RpcClient client) {
            this.hostPort = hostPort;
            this.internalClient = client;
            this.publicClient = new FailureDetectingRpcClient(
                    internalClient,
                    RpcError::isUnrecoverable,
                    e -> banErrorClient(hostPort, e)
            );
        }

        boolean ref() {
            int old = referenceCounter.getAndUpdate(x -> x == 0 ? 0 : ++x);
            return old > 0;
        }

        void unref() {
            int ref = referenceCounter.decrementAndGet();
            if (ref == 0) {
                internalClient.close();
            }
        }
    }
}

