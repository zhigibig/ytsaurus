package ru.yandex.yt.ytclient.rpc.internal;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import ru.yandex.yt.ytclient.rpc.BalancingRpcClient;

/**
 * @author aozeritsky
 */
public final class DataCenter {
    private static final Logger logger = LoggerFactory.getLogger(BalancingRpcClient.class);

    private final String dc;
    private final BalancingDestination[] backends;
    private int aliveCount;

    public DataCenter(String dc, BalancingDestination[] backends) {
        this.dc = dc;
        this.backends = backends;
        this.aliveCount = backends.length;
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
                logger.info("backend `{}` is dead, reason `{}`", dst, reason);
                dst.resetTransaction();
            }
        }
    }

    public boolean isAlive() {
        return aliveCount > 0;
    }

    public void setDead(int index, Throwable reason) {
        setDead(backends[index], reason);
    }

    public void setAlive(int index) {
        setAlive(backends[index]);
    }

    public void close() {
        for (BalancingDestination client : backends) {
            client.close();
        }
    }

    private void swap(int i, int j) {
        BalancingDestination t = backends[i];
        backends[i] = backends[j];
        backends[j] = t;

        backends[i].setIndex(i);
        backends[j].setIndex(j);
    }

    public List<BalancingDestination> selectDestinations(final int maxSelect, Random rnd) {
        final ArrayList<BalancingDestination> result = new ArrayList<>();
        result.ensureCapacity(maxSelect);

        rnd.ints(maxSelect);

        synchronized (backends) {
            int count = aliveCount;

            while (count != 0 && result.size() < maxSelect) {
                int idx = rnd.nextInt(count);
                result.add(backends[idx]);
                swap(idx, count-1);
                --count;
            }
        }

        return result;
    }

    private CompletableFuture<Void> ping(BalancingDestination client, ScheduledExecutorService executorService, Duration pingTimeout) {
        CompletableFuture<Void> f = client.createTransaction().thenCompose(id -> client.pingTransaction(id))
            .thenAccept(unused -> setAlive(client))
            .exceptionally(ex -> {
                setDead(client, ex);
                return null;
            });

        executorService.schedule(
            () -> {
                if (!f.isDone()) {
                    f.cancel(true);
                }
            },
            pingTimeout.toMillis(), TimeUnit.MILLISECONDS
        );

        return f;
    }

    public CompletableFuture<Void> ping(ScheduledExecutorService executorService, Duration pingTimeout) {
        synchronized (backends) {
            CompletableFuture<Void> futures[] = new CompletableFuture[backends.length];
            for (int i = 0; i < backends.length; ++i) {
                futures[i] = ping(backends[i], executorService, pingTimeout);
            }

            return CompletableFuture.allOf(futures);
        }
    }
}
