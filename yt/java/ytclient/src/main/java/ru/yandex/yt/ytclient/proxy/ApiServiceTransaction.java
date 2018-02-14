package ru.yandex.yt.ytclient.proxy;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;

import ru.yandex.yt.ytclient.misc.YtGuid;
import ru.yandex.yt.ytclient.misc.YtTimestamp;
import ru.yandex.yt.ytclient.proxy.request.ConcatenateNodes;
import ru.yandex.yt.ytclient.proxy.request.CopyNode;
import ru.yandex.yt.ytclient.proxy.request.CreateNode;
import ru.yandex.yt.ytclient.proxy.request.ExistsNode;
import ru.yandex.yt.ytclient.proxy.request.GetNode;
import ru.yandex.yt.ytclient.proxy.request.LinkNode;
import ru.yandex.yt.ytclient.proxy.request.ListNode;
import ru.yandex.yt.ytclient.proxy.request.LockMode;
import ru.yandex.yt.ytclient.proxy.request.LockNode;
import ru.yandex.yt.ytclient.proxy.request.LockNodeResult;
import ru.yandex.yt.ytclient.proxy.request.MoveNode;
import ru.yandex.yt.ytclient.proxy.request.ObjectType;
import ru.yandex.yt.ytclient.proxy.request.RemoveNode;
import ru.yandex.yt.ytclient.proxy.request.SetNode;
import ru.yandex.yt.ytclient.proxy.request.TransactionalOptions;
import ru.yandex.yt.ytclient.wire.UnversionedRowset;
import ru.yandex.yt.ytclient.wire.VersionedRowset;
import ru.yandex.yt.ytclient.ytree.YTreeNode;

public class ApiServiceTransaction implements AutoCloseable {
    private final ApiServiceClient client;
    private final YtGuid id;
    private final YtTimestamp startTimestamp;
    private final boolean ping;
    private final boolean sticky;
    private final TransactionalOptions transactionalOptions;
    private final Duration pingPeriod;

    enum State {
        ACTIVE,
        COMMITTING,
        COMMITTED,
        ABORTED
    }

    private final Object stateLock = new Object();
    private State state;

    public ApiServiceClient getClient() {
        return client;
    }

    public YtGuid getId() {
        return id;
    }

    public YtTimestamp getStartTimestamp() {
        return startTimestamp;
    }

    public boolean isPing() {
        return ping;
    }

    public boolean isSticky() {
        return sticky;
    }

    ApiServiceTransaction(
            ApiServiceClient client,
            YtGuid id,
            YtTimestamp startTimestamp,
            boolean ping,
            boolean pingAncestors,
            boolean sticky,
            Duration pingPeriod)
    {
        this.client = Objects.requireNonNull(client);
        this.id = Objects.requireNonNull(id);
        this.startTimestamp = Objects.requireNonNull(startTimestamp);
        this.ping = ping;
        this.sticky = sticky;
        this.transactionalOptions = new TransactionalOptions(id, ping, pingAncestors, sticky);
        this.state = State.ACTIVE;
        this.pingPeriod = pingPeriod;

        if (ping && ! pingPeriod.isZero() && ! pingPeriod.isNegative()) {
            runPeriodicPings();
        }
    }

    private State getState() {
        synchronized (stateLock) {
            return state;
        }
    }

    private boolean isPingableState() {
        State state = getState();
        return state == State.ACTIVE || state == State.COMMITTING;
    }

    private void runPeriodicPings() {
        if (!isPingableState()) {
            return;
        }

        ping().thenAccept((unused) -> {
            if (!isPingableState()) {
                return;
            }

            client.schedule(() -> {
                runPeriodicPings();
                return null;
            }, pingPeriod.toMillis(), TimeUnit.MILLISECONDS);
        }).exceptionally((ex) -> {
            // TODO check timeout here?
            return null;
        });
    }

    public CompletableFuture<Void> ping() {
        return client.pingTransaction(id, sticky);
    }

    private void setCommitted() {
        synchronized (stateLock) {
            if (state != State.COMMITTING) {
                throw new IllegalStateException(String.format("Transaction '%s' is already being committed", id));
            }

            state = State.COMMITTED;
        }
    }

    private void setAborted() {
        synchronized (stateLock) {
            state = State.ABORTED;
        }
    }

    public CompletableFuture<Void> commit() {
        synchronized (stateLock) {
            switch (state) {
                case COMMITTED:
                    throw new IllegalStateException(String.format("Transaction '%s' is already committed", id));
                case COMMITTING:
                    throw new IllegalStateException(String.format("Transaction '%s' is already being committed", id));
                case ABORTED:
                    throw new IllegalStateException(String.format("Transaction '%s' is already aborted", id));
                default:
                    state = State.COMMITTING;
                    break;
            }
        }

        return client.commitTransaction(id, sticky).thenAccept((unused) ->
            setCommitted()
        ).exceptionally((ex) -> {
            setAborted();
            return null;
        });
    }

    public CompletableFuture<Void> abort() {
        State state = getState();
        if (state != State.ACTIVE) {
            throw new IllegalStateException(String.format("Transaction '%s' is closed", id));
        }

        synchronized (stateLock) {
            this.state = State.ABORTED;
        }

        // dont wait for answer
        return client.abortTransaction(id, sticky);
    }

    @Override
    public void close() {
        CompletableFuture<Void> future;
        try {
            future = abort();
        } catch (IllegalStateException ignored) {
            // транзакция уже закрыта
            return;
        }
        try {
            future.join();
        } catch (CancellationException | CompletionException ignored) {
            // игнорируем ошибки abort'а
        } finally {
            synchronized (stateLock) {
                this.state = State.ABORTED;
            }
        }
    }

    public CompletableFuture<UnversionedRowset> lookupRows(LookupRowsRequest request) {
        return client.lookupRows(request, startTimestamp);
    }

    public CompletableFuture<VersionedRowset> versionedLookupRows(LookupRowsRequest request) {
        return client.versionedLookupRows(request, startTimestamp);
    }

    public CompletableFuture<Void> modifyRows(ModifyRowsRequest request) {
        return client.modifyRows(id, request);
    }

    /* nodes */

    public CompletableFuture<YtGuid> createNode(CreateNode req) {
        return client.createNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<YtGuid> createNode(String path, ObjectType type) {
        return createNode(new CreateNode(path, type));
    }

    public CompletableFuture<Boolean> existsNode(ExistsNode req) {
        return client.existsNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<Boolean> existsNode(String path) {
        return existsNode(new ExistsNode(path));
    }

    public CompletableFuture<YTreeNode> getNode(GetNode req) {
        return client.getNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<YTreeNode> getNode(String path) {
        return getNode(new GetNode(path));
    }

    public CompletableFuture<YTreeNode> listNode(ListNode req) {
        return client.listNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<YTreeNode> listNode(String path) {
        return listNode(new ListNode(path));
    }

    public CompletableFuture<Void> removeNode(RemoveNode req) {
        return client.removeNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<Void> removeNode(String path) {
        return removeNode(new RemoveNode(path));
    }

    public CompletableFuture<Void> setNode(SetNode req) {
        return client.setNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<Void> setNode(String path, byte[] data) {
        return setNode(new SetNode(path, data));
    }

    public CompletableFuture<Void> setNode(String path, YTreeNode data) {
        return setNode(path, data.toBinary());
    }

    public CompletableFuture<LockNodeResult> lockNode(LockNode req) {
        return client.lockNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<LockNodeResult> lockNode(String path, LockMode mode) {
        return lockNode(new LockNode(path, mode));
    }

    public CompletableFuture<YtGuid> copyNode(CopyNode req) {
        return client.copyNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<YtGuid> copyNode(String src, String dst) {
        return copyNode(new CopyNode(src, dst));
    }

    public CompletableFuture<YtGuid> moveNode(MoveNode req) {
        return client.moveNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<YtGuid> moveNode(String from, String to) {
        return moveNode(new MoveNode(from, to));
    }

    public CompletableFuture<YtGuid> linkNode(LinkNode req) {
        return client.linkNode(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<YtGuid> linkNode(String src, String dst) {
        return linkNode(new LinkNode(src, dst));
    }

    public CompletableFuture<Void> concatenateNodes(ConcatenateNodes req) {
        return client.concatenateNodes(req.setTransactionalOptions(transactionalOptions));
    }

    public CompletableFuture<Void> concatenateNodes(String [] from, String to) {
        return concatenateNodes(new ConcatenateNodes(from, to));
    }
}
