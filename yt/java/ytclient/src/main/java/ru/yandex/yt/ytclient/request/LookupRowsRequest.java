package ru.yandex.yt.ytclient.request;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;

import javax.annotation.Nullable;

import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.ytclient.object.UnversionedRowSerializer;
import ru.yandex.yt.ytclient.proxy.ApiServiceUtil;
import ru.yandex.yt.ytclient.tables.TableSchema;
import ru.yandex.yt.ytclient.wire.UnversionedRow;
import ru.yandex.yt.ytclient.wire.UnversionedValue;
import ru.yandex.yt.ytclient.wire.WireProtocolWriter;

@NonNullApi
@NonNullFields
public class LookupRowsRequest extends AbstractLookupRowsRequest<LookupRowsRequest.Builder, LookupRowsRequest> {
    private final List<UnversionedRow> filters = new ArrayList<>();

    public LookupRowsRequest(BuilderBase<?> builder) {
        super(builder);
        if (builder.convertedFilters != null) {
            filters.addAll(builder.convertedFilters);
        }
        for (List<?> filter : builder.filters) {
            filters.add(convertFilterToRow(filter));
        }
    }

    public LookupRowsRequest(String path, TableSchema schema) {
        this(builder().setPath(path).setSchema(schema));
    }

    public static Builder builder() {
        return new Builder();
    }

    private UnversionedRow convertFilterToRow(List<?> filter) {
        if (filter.size() != schema.getColumns().size()) {
            throw new IllegalArgumentException("Number of filter columns must match the number key columns");
        }
        List<UnversionedValue> row = new ArrayList<>(schema.getColumns().size());
        ApiServiceUtil.convertKeyColumns(row, schema, filter);
        return new UnversionedRow(row);
    }

    @Override
    public void serializeRowsetTo(List<byte[]> attachments) {
        WireProtocolWriter writer = new WireProtocolWriter(attachments);
        writer.writeUnversionedRowset(filters, new UnversionedRowSerializer(getSchema()));
        writer.finish();
    }

    @Override
    public Builder toBuilder() {
        return builder()
                .setFilters(filters)
                .setPath(path)
                .setSchema(schema)
                .setTimestamp(timestamp)
                .setRetentionTimestamp(retentionTimestamp)
                .setKeepMissingRows(keepMissingRows)
                .setTimeout(timeout)
                .setRequestId(requestId)
                .setUserAgent(userAgent)
                .setTraceId(traceId, traceSampled)
                .setAdditionalData(additionalData);
    }

    public static class Builder extends BuilderBase<Builder> {
        @Override
        protected Builder self() {
            return this;
        }

        @Override
        public LookupRowsRequest build() {
            return new LookupRowsRequest(this);
        }
    }

    @NonNullApi
    @NonNullFields
    public abstract static class BuilderBase<
            TBuilder extends BuilderBase<TBuilder>>
            extends AbstractLookupRowsRequest.Builder<TBuilder, LookupRowsRequest> {
        private final List<List<?>> filters = new ArrayList<>();
        @Nullable
        private List<UnversionedRow> convertedFilters;

        public TBuilder addFilter(List<?> filter) {
            filters.add(Objects.requireNonNull(filter));
            return self();
        }

        public TBuilder addFilter(Object... filterValues) {
            return addFilter(Arrays.asList(filterValues));
        }

        public TBuilder addFilters(Iterable<? extends List<?>> filters) {
            for (List<?> filter : filters) {
                addFilter(filter);
            }
            return self();
        }

        TBuilder setFilters(List<UnversionedRow> filters) {
            this.convertedFilters = filters;
            return self();
        }
    }
}
