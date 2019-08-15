package ru.yandex.yt.ytclient.object;

import ru.yandex.yt.ytclient.tables.TableSchema;

public interface WireRowSerializer<T> {

    TableSchema getSchema();

    void serializeRow(T row, WireProtocolWriteable writeable, boolean keyFieldsOnly, int[] idMapping);

    default void serializeRow(T row, WireProtocolWriteable writeable, boolean keyFieldsOnly) {
        serializeRow(row, writeable, keyFieldsOnly, null);
    }
}
