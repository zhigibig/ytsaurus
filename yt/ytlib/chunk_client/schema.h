﻿#pragma once

#include "public.h"

#include <ytlib/chunk_client/schema.pb.h>

#include <core/ytree/public.h>
#include <core/yson/consumer.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

// ToDo(psushin): deprecate.
//! Range of columns used as a part of channel description.
class TColumnRange
{
public:
    //! Creates a finite range.
    TColumnRange(const Stroka& begin, const Stroka& end);

    //! Creates an infinite range.
    explicit TColumnRange(const Stroka& begin = Stroka());

    Stroka Begin() const;
    Stroka End() const;

    bool Contains(const TStringBuf& value) const;
    bool Contains(const TColumnRange& range) const;
    bool Overlaps(const TColumnRange& range) const;

    bool IsInfinite() const;

private:
    bool IsInfinite_;
    Stroka Begin_;
    Stroka End_;
};

////////////////////////////////////////////////////////////////////////////////

//! Set of fixed columns and column ranges.
class TChannel
{
public:
    TChannel();
    TChannel(
        std::vector<Stroka> columns,
        std::vector<TColumnRange> ranges);

    void AddColumn(const Stroka& column);
    void AddRange(const TColumnRange& range);
    void AddRange(const Stroka& begin, const Stroka& end);

    bool Contains(const TStringBuf& column) const;
    bool Contains(const TChannel& channel) const;
    bool Contains(const TColumnRange& range) const;
    bool ContainsInRanges(const TStringBuf& column) const;

    bool Overlaps(const TChannel& channel) const;
    bool Overlaps(const TColumnRange& range) const;

    bool IsEmpty() const;
    bool IsUniversal() const;

    const std::vector<Stroka>& GetColumns() const;
    const std::vector<TColumnRange>& GetRanges() const;

    //! Returns the channel containing all possible columns.
    static const TChannel& Universal();

    //! Returns the empty channel.
    static const TChannel& Empty();

private:
    friend TChannel& operator -= (TChannel& lhs, const TChannel& rhs);

    std::vector<Stroka> Columns_;
    std::vector<TColumnRange> Ranges_;

};

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TRange* protoRange, const TColumnRange& range);
void FromProto(TColumnRange* range, const NProto::TRange& protoRange);

void ToProto(NProto::TChannel* protoChannel, const TChannel& channel);
void FromProto(TChannel* channel, const NProto::TChannel& protoChannel);

void Serialize(const TChannel& channel, NYson::IYsonConsumer* consumer);
void Deserialize(TChannel& channel, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
