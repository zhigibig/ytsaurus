package ru.yandex.yt.ytclient.object;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

import javax.annotation.Nonnull;

import ru.yandex.yt.ytclient.tables.TableSchema;
import ru.yandex.yt.ytclient.wire.UnversionedRow;
import ru.yandex.yt.ytclient.wire.UnversionedRowset;

public class UnversionedRowsetDeserializer extends UnversionedRowDeserializer implements WireRowsetDeserializer<UnversionedRow> {

    private final TableSchema schema;
    private List<UnversionedRow> rows = Collections.emptyList();

    public UnversionedRowsetDeserializer(TableSchema schema) {
        this.schema = Objects.requireNonNull(schema);
    }

    @Override
    public void setRowCount(int rowCount) {
        this.rows = new ArrayList<>(rowCount);
    }

    @Override
    @Nonnull
    public UnversionedRow onCompleteRow() {
        UnversionedRow row = super.onCompleteRow();
        this.rows.add(row);
        return row;
    }

    @Override
    public UnversionedRow onNullRow() {
        UnversionedRow row = super.onNullRow();
        this.rows.add(row);
        return row;
    }

    public UnversionedRowset getRowset() {
        return new UnversionedRowset(schema, this.rows);
    }
}
