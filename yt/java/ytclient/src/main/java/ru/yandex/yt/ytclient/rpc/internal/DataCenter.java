package ru.yandex.yt.ytclient.rpc.internal;

import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.stream.Collectors;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import ru.yandex.yt.ytclient.rpc.BalancingRpcClient;
import ru.yandex.yt.ytclient.rpc.RpcClient;
import ru.yandex.yt.ytclient.rpc.RpcOptions;
import ru.yandex.yt.ytclient.rpc.internal.metrics.BalancingDestinationMetricsHolder;
import ru.yandex.yt.ytclient.rpc.internal.metrics.BalancingDestinationMetricsHolderImpl;
import ru.yandex.yt.ytclient.rpc.internal.metrics.DataCenterMetricsHolder;
import ru.yandex.yt.ytclient.rpc.internal.metrics.DataCenterMetricsHolderImpl;

/**
 * @author aozeritsky
 *
 * TODO: move to proxy
 */
public final class DataCenter {
    private static final Logger logger = LoggerFactory.getLogger(BalancingRpcClient.class);

    private final DataCenterMetricsHolder metricsHolder;
    private final BalancingDestinationMetricsHolder destMetricsHolder;
    private final RpcOptions options;

    private final String dc;
    private final ArrayList<BalancingDestination> backends;
    private int aliveCount;
    private final double weight;

    public DataCenter(String dc, BalancingDestination[] backends) {
        this(dc, backends, -1.0);
    }

    public DataCenter(
            String dc,
            BalancingDestination[] backends,
            DataCenterMetricsHolder metricsHolder,
            BalancingDestinationMetricsHolder destMetricsHolder)
    {
        this(dc, backends, -1.0, metricsHolder, destMetricsHolder, new RpcOptions());
    }

    public DataCenter(String dc, BalancingDestination[] backends, double weight) {
        this(dc, backends, weight, DataCenterMetricsHolderImpl.instance, BalancingDestinationMetricsHolderImpl.instance, new RpcOptions());
    }

    public DataCenter(
            String dc,
            BalancingDestination[] backends,
            double weight,
            DataCenterMetricsHolder metricsHolder,
            BalancingDestinationMetricsHolder destMetricsHolder,
            RpcOptions options)
    {
        this.dc = dc;
        this.backends = new ArrayList<>(Arrays.asList(backends));
        this.aliveCount = backends.length;
        this.weight = weight;
        this.metricsHolder = metricsHolder;
        this.destMetricsHolder = destMetricsHolder;
        this.options = options;
    }

    public double weight() {
        if (weight > 0) {
            return weight;
        } else {
            return metricsHolder.getDc99thPercentile(dc);
        }
    }

    public String getName() {
        return dc;
    }

    private void setAlive(BalancingDestination dst) {
        synchronized (backends) {
            if (dst.getIndex() >= aliveCount) {
                swap(aliveCount, dst.getIndex());
                aliveCount++;
                logger.info("backend `{}` is alive", dst);
            }
        }
    }

    private void setDead(BalancingDestination dst, Throwable reason) {
        synchronized (backends) {
            if (dst.getIndex() < aliveCount) {
                aliveCount--;
                swap(aliveCount, dst.getIndex());
                logger.info("backend `{}` is dead, reason `{}`", dst, reason.toString());
                dst.resetTransaction();
            }
        }
    }

    public void addProxies(List<RpcClient> proxies) {
        synchronized (backends) {
            backends.ensureCapacity(backends.size() + proxies.size());

            int index = proxies.size();
            for (RpcClient proxy : proxies) {
                backends.add(new BalancingDestination(dc, proxy, index ++, destMetricsHolder, options));
            }
        }
    }

    public void removeProxies(List<RpcClient> proxies) {
        final Map<String, RpcClient> hash = new HashMap<>();
        for (RpcClient client : proxies) {
            hash.put(client.destinationName(), client);
        }

        final ArrayList<BalancingDestination> removeList = new ArrayList<>();
        synchronized (backends) {
            removeList.ensureCapacity(proxies.size());
            for (BalancingDestination dest : backends) {
                if (hash.containsKey(dest.getClient().destinationName())) {
                    removeList.add(dest);
                }
            }
        }

        for (BalancingDestination removed: removeList) {
            setDead(removed, new Exception("proxy was removed from list"));
            synchronized (backends) {
                // TODO: batch remove
                backends.remove(removed.getIndex());
            }
        }
    }

    public boolean isAlive() {
        return aliveCount > 0;
    }

    public void setDead(int index, Throwable reason) {
        setDead(backends.get(index), reason);
    }

    public void setAlive(int index) {
        setAlive(backends.get(index));
    }

    public void close() {
        for (BalancingDestination client : backends) {
            client.close();
        }
    }

    private void swap(int i, int j) {
        Collections.swap(backends, i, j);

        backends.get(i).setIndex(i);
        backends.get(j).setIndex(j);
    }

    public List<RpcClient> getAliveDestinations() {
        synchronized (backends) {
            return backends.subList(0, aliveCount).stream().map(BalancingDestination::getClient).collect(Collectors.toList());
        }
    }

    public List<RpcClient> selectDestinations(final int maxSelect, Random rnd) {
        final ArrayList<RpcClient> result = new ArrayList<>();
        result.ensureCapacity(maxSelect);

        synchronized (backends) {
            int count = aliveCount;

            while (count != 0 && result.size() < maxSelect) {
                int idx = rnd.nextInt(count);
                result.add(backends.get(idx).getClient());
                swap(idx, count-1);
                --count;
            }
        }

        return result;
    }

    private CompletableFuture<Void> ping(BalancingDestination client, ScheduledExecutorService executorService, Duration pingTimeout) {
        CompletableFuture<Void> f = client.createTransaction(pingTimeout).thenCompose(id -> client.pingTransaction(id))
            .thenAccept(unused -> setAlive(client))
            .exceptionally(ex -> {
                setDead(client, ex);
                return null;
            });

        executorService.schedule(
            () -> {
                if (!f.isDone()) {
                    setDead(client, new Exception("ping timeout"));
                    f.cancel(true);
                }
            },
            pingTimeout.toMillis(), TimeUnit.MILLISECONDS
        );

        return f;
    }

    public CompletableFuture<Void> ping(ScheduledExecutorService executorService, Duration pingTimeout) {
        synchronized (backends) {
            CompletableFuture<Void> futures[] = new CompletableFuture[backends.size()];
            for (int i = 0; i < backends.size(); ++i) {
                futures[i] = ping(backends.get(i), executorService, pingTimeout);
            }

            return CompletableFuture.allOf(futures);
        }
    }
}
