package ru.yandex.yt.ytclient.proxy;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.function.Function;

import ru.yandex.inside.yt.kosher.impl.ytree.object.YTreeRowSerializer;
import ru.yandex.type_info.TiType;
import ru.yandex.type_info.TypeIO;
import ru.yandex.yt.rpcproxy.ERowsetKind;
import ru.yandex.yt.rpcproxy.TColumnSchema;
import ru.yandex.yt.rpcproxy.TRowsetDescriptor;
import ru.yandex.yt.rpcproxy.TTableSchema;
import ru.yandex.yt.ytclient.SerializationResolver;
import ru.yandex.yt.ytclient.object.ConsumerSource;
import ru.yandex.yt.ytclient.object.UnversionedRowsetDeserializer;
import ru.yandex.yt.ytclient.object.VersionedRowsetDeserializer;
import ru.yandex.yt.ytclient.object.WireRowsetDeserializer;
import ru.yandex.yt.ytclient.object.WireVersionedRowsetDeserializer;
import ru.yandex.yt.ytclient.tables.ColumnSchema;
import ru.yandex.yt.ytclient.tables.ColumnSortOrder;
import ru.yandex.yt.ytclient.tables.ColumnValueType;
import ru.yandex.yt.ytclient.tables.TableSchema;
import ru.yandex.yt.ytclient.wire.UnversionedRowset;
import ru.yandex.yt.ytclient.wire.UnversionedValue;
import ru.yandex.yt.ytclient.wire.VersionedRowset;
import ru.yandex.yt.ytclient.wire.WireProtocol;
import ru.yandex.yt.ytclient.wire.WireProtocolReader;

public class ApiServiceUtil {
    public static final long MICROS_PER_SECOND = 1_000_000L;

    private ApiServiceUtil() {
    }

    /**
     * Конвертирует исходные сырые значения в значения колонок по указанной схеме
     */
    public static void convertKeyColumns(
            List<UnversionedValue> row,
            TableSchema schema,
            List<?> values,
            boolean allowMissingColumns,
            SerializationResolver serializationResolver
    ) {
        int columnsCount = schema.getKeyColumnsCount();
        if (allowMissingColumns && values.size() < columnsCount) {
            columnsCount = values.size();
        }
        for (int id = 0; id < columnsCount; ++id) {
            ColumnSchema column = schema.getColumns().get(id);
            ColumnValueType type = column.getType();
            Object value = UnversionedValue.convertValueTo(values.get(id), type, serializationResolver);
            if (value == null) {
                type = ColumnValueType.NULL;
            }
            row.add(new UnversionedValue(id, type, false, value));
        }
    }

    public static void convertKeyColumns(
            List<UnversionedValue> row,
            TableSchema schema,
            List<?> values,
            SerializationResolver serializationResolver
    ) {
        convertKeyColumns(row, schema, values, false, serializationResolver);
    }

    public static void convertValueColumns(
            List<UnversionedValue> row,
            TableSchema schema,
            List<?> values,
            boolean skipMissingValues,
            boolean aggregate,
            SerializationResolver serializationResolver
    ) {
        for (int id = schema.getKeyColumnsCount(); id < schema.getColumns().size() && id < values.size(); ++id) {
            ColumnSchema column = schema.getColumns().get(id);
            Object inputValue = values.get(id);
            if (inputValue == null && skipMissingValues) {
                continue;
            }
            ColumnValueType type = column.getType();
            Object value = UnversionedValue.convertValueTo(inputValue, type, serializationResolver);
            if (value == null) {
                type = ColumnValueType.NULL;
            }
            row.add(new UnversionedValue(id, type, aggregate, value));
        }
    }

    public static TableSchema deserializeTableSchema(TTableSchema schema) {
        TableSchema.Builder builder = new TableSchema.Builder().setUniqueKeys(schema.getUniqueKeys());

        for (TColumnSchema columnSchema : schema.getColumnsList()) {
            String name = columnSchema.getName();

            ColumnSchema.Builder columnBuilder;

            ColumnValueType type = ColumnValueType.NULL;
            if (columnSchema.hasType()) {
                type = ColumnValueType.fromValue(columnSchema.getType());
            }
            boolean required = columnSchema.getRequired();

            TiType typeV3 = null;
            if (columnSchema.hasTypeV3()) {
                typeV3 = TypeIO.parseYson(columnSchema.getTypeV3().toByteArray());
            }

            if (typeV3 != null) {
                columnBuilder = ColumnSchema.builder(name, typeV3);
            } else {
                columnBuilder = ColumnSchema.builder(name, type, required);
            }

            if (columnSchema.hasSortOrder()) {
                columnBuilder.setSortOrder(ColumnSortOrder.fromId(columnSchema.getSortOrder()));
            }

            if (columnSchema.hasAggregate()) {
                columnBuilder.setAggregate(columnSchema.getAggregate());
            }

            if (columnSchema.hasLock()) {
                columnBuilder.setLock(columnSchema.getLock());
            }

            if (columnSchema.hasExpression()) {
                columnBuilder.setExpression(columnSchema.getExpression());
            }

            if (columnSchema.hasGroup()) {
                columnBuilder.setGroup(columnSchema.getGroup());
            }

            builder.add(columnBuilder.build());
        }

        return builder.build();
    }

    public static TableSchema deserializeRowsetSchema(TRowsetDescriptor descriptor) {
        TableSchema.Builder builder = new TableSchema.Builder().setUniqueKeys(false);
        for (TRowsetDescriptor.TNameTableEntry entry : descriptor.getNameTableEntriesList()) {
            String name = "";
            if (entry.hasName()) {
                name = entry.getName();
            }
            ColumnValueType type = ColumnValueType.NULL;
            if (entry.hasType()) {
                type = ColumnValueType.fromValue(entry.getType());
            }
            builder.addValue(name, type);
        }
        return builder.build();
    }

    public static <T> void deserializeUnversionedRowset(
            TRowsetDescriptor descriptor,
            List<byte[]> attachments,
            YTreeRowSerializer<T> serializer,
            ConsumerSource<T> consumer,
            SerializationResolver serializationResolver
    ) {
        deserializeUnversionedRowset(descriptor, attachments,
                schema -> serializationResolver.createWireRowDeserializer(schema, serializer, consumer));
    }

    public static UnversionedRowset deserializeUnversionedRowset(
            TRowsetDescriptor descriptor,
            List<byte[]> attachments
    ) {
        return deserializeUnversionedRowset(descriptor, attachments, UnversionedRowsetDeserializer::new).getRowset();
    }

    public static void validateRowsetDescriptor(TRowsetDescriptor descriptor) {
        if (descriptor.getWireFormatVersion() != WireProtocol.WIRE_FORMAT_VERSION) {
            throw new IllegalStateException("Cannot deserialize wire format" + descriptor.getWireFormatVersion() + ": "
                    + WireProtocol.WIRE_FORMAT_VERSION + " is required");
        }
        if (descriptor.getRowsetKind() != ERowsetKind.RK_UNVERSIONED) {
            throw new IllegalStateException(
                    "Cannot deserialize " + descriptor.getRowsetKind() + ": UNVERSIONED is required");
        }
    }

    private static <B extends WireRowsetDeserializer<T>, T> B deserializeUnversionedRowset(
            TRowsetDescriptor descriptor,
            List<byte[]> attachments,
            Function<TableSchema, B> deserializerFunction
    ) {
        validateRowsetDescriptor(descriptor);
        final B deserializer = deserializerFunction.apply(deserializeRowsetSchema(descriptor));
        return new WireProtocolReader(attachments).readUnversionedRowset(deserializer);
    }

    public static VersionedRowset deserializeVersionedRowset(TRowsetDescriptor descriptor, List<byte[]> attachments) {
        return deserializeVersionedRowset(descriptor, attachments, VersionedRowsetDeserializer::new).getRowset();
    }

    private static <B extends WireVersionedRowsetDeserializer<T>, T> B deserializeVersionedRowset(
            TRowsetDescriptor descriptor,
            List<byte[]> attachments,
            Function<TableSchema, B> deserializerFunction
    ) {
        if (descriptor.getWireFormatVersion() != WireProtocol.WIRE_FORMAT_VERSION) {
            throw new IllegalStateException("Cannot deserialize wire format" + descriptor.getWireFormatVersion() + ": "
                    + WireProtocol.WIRE_FORMAT_VERSION + " is required");
        }
        if (descriptor.getRowsetKind() != ERowsetKind.RK_VERSIONED) {
            throw new IllegalStateException(
                    "Cannot deserialize " + descriptor.getRowsetKind() + ": VERSIONED is required");
        }
        final B deserializer = deserializerFunction.apply(deserializeRowsetSchema(descriptor));
        return new WireProtocolReader(attachments).readVersionedRowset(deserializer);
    }

    public static TRowsetDescriptor makeRowsetDescriptor(TableSchema schema) {
        TRowsetDescriptor.Builder builder = TRowsetDescriptor.newBuilder();
        builder.setWireFormatVersion(WireProtocol.WIRE_FORMAT_VERSION);
        builder.setRowsetKind(ERowsetKind.RK_UNVERSIONED);
        for (ColumnSchema column : schema.getColumns()) {
            builder.addNameTableEntriesBuilder()
                    .setName(column.getName())
                    .setType(column.getType().getValue());
        }
        return builder.build();
    }

    public static long durationToYtMicros(Duration duration) {
        long micros = Math.multiplyExact(duration.getSeconds(), MICROS_PER_SECOND);
        micros = Math.addExact(micros, duration.getNano() / 1000);
        return micros;
    }

    public static long instantToYtMicros(Instant instant) {
        return Math.multiplyExact(instant.toEpochMilli(), 1000L);
    }
}
