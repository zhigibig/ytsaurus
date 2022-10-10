package ru.yandex.yt.ytclient.proxy;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.function.Function;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import ru.yandex.inside.yt.kosher.impl.ytree.object.YTreeRowSerializer;
import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.TRspSelectRows;
import ru.yandex.yt.ytclient.SerializationResolver;
import ru.yandex.yt.ytclient.object.ConsumerSource;
import ru.yandex.yt.ytclient.object.ConsumerSourceRet;
import ru.yandex.yt.ytclient.rpc.RpcClientResponse;
import ru.yandex.yt.ytclient.rpc.RpcUtil;
import ru.yandex.yt.ytclient.wire.UnversionedRowset;


@NonNullApi
@NonNullFields
public class SelectRowsResult {
    private static final Logger logger = LoggerFactory.getLogger(SelectRowsResult.class);

    private final RpcClientResponse<TRspSelectRows> response;
    private final Executor heavyExecutor;

    private final SerializationResolver serializationResolver;

    public SelectRowsResult(
            RpcClientResponse<TRspSelectRows> response,
            Executor heavyExecutor,
            SerializationResolver serializationResolver
    ) {
        this.response = response;
        this.heavyExecutor = heavyExecutor;
        this.serializationResolver = serializationResolver;
    }

    public CompletableFuture<UnversionedRowset> getUnversionedRowset() {
        return handleResponse(response ->
                        ApiServiceUtil.deserializeUnversionedRowset(
                                response.body().getRowsetDescriptor(),
                                response.attachments()));
    }

    public <T> CompletableFuture<List<T>> getRowsList(YTreeRowSerializer<T> serializer) {
        return handleResponse(response -> {
            final ConsumerSourceRet<T> result = ConsumerSource.list();
            ApiServiceUtil.deserializeUnversionedRowset(response.body().getRowsetDescriptor(),
                    response.attachments(), serializer, result, serializationResolver);
            return result.get();
        });
    }

    public <T> CompletableFuture<Void> handleWithConsumer(YTreeRowSerializer<T> serializer,
                                                          ConsumerSource<T> consumer) {
        return handleResponse(response -> {
            ApiServiceUtil.deserializeUnversionedRowset(response.body().getRowsetDescriptor(),
                    response.attachments(), serializer, consumer, serializationResolver);
            return null;
        });
    }

    public boolean isIncompleteOutput() {
        return response.body().getStatistics().getIncompleteOutput();
    }

    public boolean isIncompleteInput() {
        return response.body().getStatistics().getIncompleteInput();
    }

    private <T> CompletableFuture<T> handleResponse(Function<RpcClientResponse<TRspSelectRows>, T> fn) {
        return RpcUtil.applyAsync(
                CompletableFuture.completedFuture(response),
                response -> {
                    logger.trace("SelectRows incoming rowset descriptor: {}", response.body().getRowsetDescriptor());
                    return fn.apply(response);
                },
                heavyExecutor);
    }
}
