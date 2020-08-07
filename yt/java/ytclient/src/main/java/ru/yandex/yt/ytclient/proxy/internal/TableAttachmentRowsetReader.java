package ru.yandex.yt.ytclient.proxy.internal;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;

import javax.annotation.Nullable;

import NYT.NChunkClient.NProto.DataStatistics;

import ru.yandex.yt.rpcproxy.TRowsetDescriptor;
import ru.yandex.yt.rpcproxy.TRowsetStatistics;
import ru.yandex.yt.ytclient.proxy.ApiServiceUtil;
import ru.yandex.yt.ytclient.tables.TableSchema;

public abstract class TableAttachmentRowsetReader<T> implements TableAttachmentReader<T> {
    protected final AtomicLong totalRowCount = new AtomicLong(-1);
    protected volatile DataStatistics.TDataStatistics currentDataStatistics;

    protected volatile TRowsetDescriptor currentRowsetDescriptor;
    protected volatile TableSchema currentReadSchema;

    protected abstract List<T> parseMergedRow(ByteBuffer bb, int size);

    protected void parseDescriptorDelta(ByteBuffer bb, int size) throws Exception {
        int endPosition = bb.position() + size;
        TRowsetDescriptor rowsetDescriptor = TRowsetDescriptor.parseFrom((ByteBuffer) bb.slice().limit(size)); //
        // (ByteBuffer) for java8 compatibility
        ApiServiceUtil.validateRowsetDescriptor(rowsetDescriptor);

        if (currentReadSchema == null) {
            currentRowsetDescriptor = rowsetDescriptor;
            currentReadSchema = ApiServiceUtil.deserializeRowsetSchema(rowsetDescriptor);
        } else if (rowsetDescriptor.getNameTableEntriesCount() > 0) {
            TRowsetDescriptor.Builder builder = TRowsetDescriptor.newBuilder();
            builder.mergeFrom(currentRowsetDescriptor);
            builder.addAllNameTableEntries(rowsetDescriptor.getNameTableEntriesList());
            currentRowsetDescriptor = builder.build();
            currentReadSchema = ApiServiceUtil.deserializeRowsetSchema(currentRowsetDescriptor);
        } else {
            // schema is not changed
        }

        bb.position(endPosition);
    }

    private List<T> parseRowData(ByteBuffer bb, int size) throws Exception {
        int endPosition = bb.position() + size;

        int parts = bb.getInt();

        if (parts != 2) {
            throw new IllegalArgumentException();
        }

        int descriptorDeltaSize = (int) bb.getLong();
        parseDescriptorDelta(bb, descriptorDeltaSize);

        int mergedRowSize = (int) bb.getLong();
        List<T> rowset = parseMergedRow(bb, mergedRowSize);

        if (bb.position() != endPosition) {
            throw new IllegalArgumentException();
        }

        return rowset;
    }

    private void parseStatistics(ByteBuffer bb, int size) throws Exception {
        int endPosition = bb.position() + size;
        TRowsetStatistics statistics = TRowsetStatistics.parseFrom((ByteBuffer) bb.slice().limit(size)); //
        // (ByteBuffer) for java8 compatibility
        currentDataStatistics = statistics.getDataStatistics();
        totalRowCount.set(statistics.getTotalRowCount());
        bb.position(endPosition);
    }

    private List<T> parseRowsWithStatistics(byte[] attachment, int offset, int length) throws Exception {
        if (attachment == null) {
            return null;
        }

        ByteBuffer bb = ByteBuffer.wrap(attachment, offset, length).order(ByteOrder.LITTLE_ENDIAN);
        int parts = bb.getInt();
        if (parts != 2) {
            throw new IllegalArgumentException();
        }

        int rowDataSize = (int) bb.getLong();
        List<T> rowset = parseRowData(bb, rowDataSize);

        int statisticsSize = (int) bb.getLong();
        parseStatistics(bb, statisticsSize);

        if (bb.hasRemaining()) {
            throw new IllegalArgumentException();
        }

        return rowset;
    }

    @Override
    public List<T> parse(byte[] attachments, int offset, int length) throws Exception {
        return parseRowsWithStatistics(attachments, offset, length);
    }

    @Override
    public List<T> parse(byte[] attachments) throws Exception {
        if (attachments == null) {
            return null;
        }
        return parseRowsWithStatistics(attachments, 0, attachments.length);
    }

    @Override
    public long getTotalRowCount() {
        return totalRowCount.get();
    }

    @Override
    public DataStatistics.TDataStatistics getDataStatistics() {
        return currentDataStatistics;
    }

    @Nullable
    @Override
    public TableSchema getCurrentReadSchema() {
        return currentReadSchema;
    }

    @Nullable
    @Override
    public TRowsetDescriptor getRowsetDescriptor() {
        return currentRowsetDescriptor;
    }
}
