package ru.yandex.yt.ytclient.proxy.request;

import javax.annotation.Nonnull;

import ru.yandex.inside.yt.kosher.cypress.YPath;
import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.TMutatingOptions;
import ru.yandex.yt.rpcproxy.TReqRemoveNode;
import ru.yandex.yt.rpcproxy.TTransactionalOptions;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestBuilder;

@NonNullApi
@NonNullFields
public class RemoveNode extends MutateNode<RemoveNode> implements HighLevelRequest<TReqRemoveNode.Builder> {
    private final String path;
    private boolean recursive = true;
    private boolean force = false;

    public RemoveNode(String path) {
        this.path = path;
    }

    public RemoveNode(YPath path) {
        this.path = path.toString();
    }

    public RemoveNode setRecursive(boolean f) {
        this.recursive = f;
        return this;
    }

    public RemoveNode setForce(boolean f) {
        this.force = f;
        return this;
    }

    @Override
    public void writeTo(RpcClientRequestBuilder<TReqRemoveNode.Builder, ?> builder) {
        builder.body()
                .setPath(path)
                .setRecursive(recursive)
                .setForce(force);

        if (transactionalOptions != null) {
            builder.body().setTransactionalOptions(transactionalOptions.writeTo(TTransactionalOptions.newBuilder()));
        }
        if (mutatingOptions != null) {
            builder.body().setMutatingOptions(mutatingOptions.writeTo(TMutatingOptions.newBuilder()));
        }
        if (additionalData != null) {
            builder.body().mergeFrom(additionalData);
        }
    }

    @Override
    protected void writeArgumentsLogString(StringBuilder sb) {
        super.writeArgumentsLogString(sb);
        if (force) {
            sb.append("Force: true; ");
        }
        if (recursive) {
            sb.append("Recursive: true; ");
        }
    }

    @Nonnull
    @Override
    protected RemoveNode self() {
        return this;
    }
}
