package ru.yandex.yt.ytclient.proxy.request;

import javax.annotation.Nonnull;

import tech.ytsaurus.core.cypress.YPath;

public class ReshardTableAutomatic extends tech.ytsaurus.client.request.ReshardTableAutomatic.BuilderBase<
        ReshardTableAutomatic> {
    public ReshardTableAutomatic(YPath path, boolean keepActions) {
        setPath(path.justPath()).setKeepActions(keepActions);
    }

    /**
     * @deprecated Use {@link #ReshardTableAutomatic(YPath path,  boolean keepActions)} instead.
     */
    @Deprecated
    public ReshardTableAutomatic(String path, boolean keepActions) {
        setPath(path).setKeepActions(keepActions);
    }

    @Nonnull
    @Override
    protected ReshardTableAutomatic self() {
        return this;
    }
}
