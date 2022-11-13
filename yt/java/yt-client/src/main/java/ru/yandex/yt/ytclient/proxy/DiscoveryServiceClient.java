package ru.yandex.yt.ytclient.proxy;

import java.util.List;
import java.util.concurrent.CompletableFuture;

import tech.ytsaurus.client.rpc.RpcClient;
import tech.ytsaurus.client.rpc.RpcClientRequestBuilder;
import tech.ytsaurus.client.rpc.RpcOptions;
import tech.ytsaurus.client.rpc.RpcUtil;

import ru.yandex.yt.TReqDiscoverProxies;
import ru.yandex.yt.TRspDiscoverProxies;

public class DiscoveryServiceClient {
    private final RpcClient client;
    private final RpcOptions options;

    public DiscoveryServiceClient(RpcClient client, RpcOptions options) {
        this.client = client;
        this.options = options;
    }

    public RpcClient getClient() {
        return client;
    }

    public CompletableFuture<List<String>> discoverProxies(String role) {
        RpcClientRequestBuilder<TReqDiscoverProxies.Builder, TRspDiscoverProxies> builder =
                ApiServiceMethodTable.DISCOVER_PROXIES.createRequestBuilder(options);

        if (role != null) {
            builder.body().setRole(role);
        }
        return RpcUtil.apply(builder.invoke(client), response -> response.body().getAddressesList());
    }
}
