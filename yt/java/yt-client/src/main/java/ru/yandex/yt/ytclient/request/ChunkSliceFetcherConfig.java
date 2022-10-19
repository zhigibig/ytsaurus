package ru.yandex.yt.ytclient.request;

import javax.annotation.Nullable;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.TReqPartitionTables;

@NonNullApi
@NonNullFields
public class ChunkSliceFetcherConfig {
    @Nullable
    private final Integer maxSlicesPerFetch;

    ChunkSliceFetcherConfig(Builder builder) {
        this.maxSlicesPerFetch = builder.maxSlicesPerFetch;
    }

    public static Builder builder() {
        return new Builder();
    }

    public TReqPartitionTables.TChunkSliceFetcherConfig.Builder writeTo(
            TReqPartitionTables.TChunkSliceFetcherConfig.Builder builder) {
        if (maxSlicesPerFetch != null) {
            builder.setMaxSlicesPerFetch(maxSlicesPerFetch);
        }
        return builder;
    }

    public static class Builder {
        @Nullable
        private Integer maxSlicesPerFetch;

        public Builder setMaxSlicesPerFetch(@Nullable Integer maxSlicesPerFetch) {
            this.maxSlicesPerFetch = maxSlicesPerFetch;
            return this;
        }

        public ChunkSliceFetcherConfig build() {
            return new ChunkSliceFetcherConfig(this);
        }
    }
}
