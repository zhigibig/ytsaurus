﻿#include "stdafx.h"

#include "validating_writer.h"

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

TValidatingWriter::TValidatingWriter(
    const TSchema& schema, 
    const std::vector<TColumn>& keyColumns,
    IAsyncWriter* writer)
    : Writer(writer)
    , Schema(schema)
    , KeyColumnsCount(keyColumns.size())
    , RowStart(true)
{
    VERIFY_THREAD_AFFINITY(ClientThread);

    YASSERT(writer);
    {
        int columnIndex = 0;
        FOREACH(auto& keyColumn, keyColumns) {
            auto res = ColumnIndexes.insert(MakePair(keyColumn, columnIndex));
            YASSERT(res.Second());
            ++columnIndex;
        }

        FOREACH(auto& channel, Schema.GetChannels()) {
            FOREACH(auto& column, channel.GetColumns()) {
                auto res = ColumnIndexes.insert(MakePair(column, columnIndex));
                if (res.Second()) {
                    ++columnIndex;
                }
            }
        }

        IsColumnUsed.resize(columnIndex, false);
    }

    CurrentKey.resize(KeyColumnsCount);

    // Fill protobuf chunk meta.
    FOREACH(auto channel, Schema.GetChannels()) {
        *Attributes.add_chunk_channels()->mutable_channel() = channel.ToProto();
        ChannelWriters.push_back(New<TChannelWriter>(channel, ColumnIndexes));
    }
}

TAsyncError::TPtr TValidatingWriter::AsyncOpen()
{
    VERIFY_THREAD_AFFINITY(ClientThread);

    return Writer->AsyncOpen(Attributes);
}

void TValidatingWriter::Write(const TColumn& column, TValue value)
{
    VERIFY_THREAD_AFFINITY(ClientThread);

    if (RowStart) {
        CurrentKey.assign(KeyColumnsCount, TNullable<Stroka>());
        RowStart = false;
    }

    int columnIndex = TChannelWriter::UnknownIndex;
    auto it = ColumnIndexes.find(column);

    if (it == ColumnIndexes.end()) {
        auto res = UsedRangeColumns.insert(column);
        if (!res.Second()) {
            ythrow yexception() << Sprintf(
                "Column \"%s\" already used in the current row.", 
                ~column);
        }
    } else {
        columnIndex = it->Second();
        if (IsColumnUsed[columnIndex]) {
            ythrow yexception() << Sprintf(
                "Column \"%s\" already used in the current row.", 
                ~column);
        } else {
            IsColumnUsed[columnIndex] = true;
        }

        if (columnIndex < KeyColumnsCount) {
            CurrentKey[columnIndex] = value.ToString();
        }
    }

    FOREACH(auto& channelWriter, ChannelWriters) {
        channelWriter->Write(columnIndex, column, value);
    }
}

TAsyncError::TPtr TValidatingWriter::AsyncEndRow()
{
    VERIFY_THREAD_AFFINITY(ClientThread);

    FOREACH(auto& channelWriter, ChannelWriters) {
        channelWriter->EndRow();
    }

    for (int i = 0; i < IsColumnUsed.size(); ++i)
        IsColumnUsed[i] = false;
    UsedRangeColumns.clear();
    RowStart = true;

    return Writer->AsyncEndRow(CurrentKey, ChannelWriters);
}

TAsyncError::TPtr TValidatingWriter::AsyncClose()
{
    VERIFY_THREAD_AFFINITY(ClientThread);
    return Writer->AsyncClose(CurrentKey, ChannelWriters);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
