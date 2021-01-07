#pragma once

#include "public.h"

#include <yt/client/chunk_client/proto/read_limit.pb.h>

#include <yt/client/table_client/key_bound.h>
#include <yt/client/table_client/unversioned_row.h>

#include <yt/core/yson/consumer.h>

#include <yt/core/ytree/public.h>
#include <yt/core/ytree/serialize.h>

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

class TLegacyReadLimit
{
public:
    TLegacyReadLimit() = default;

    explicit TLegacyReadLimit(const NProto::TReadLimit& readLimit);
    explicit TLegacyReadLimit(NProto::TReadLimit&& readLimit);
    explicit TLegacyReadLimit(const std::unique_ptr<NProto::TReadLimit>& protoLimit);

    explicit TLegacyReadLimit(const NTableClient::TLegacyOwningKey& key);
    explicit TLegacyReadLimit(NTableClient::TLegacyOwningKey&& key);

    TLegacyReadLimit& operator= (const NProto::TReadLimit& protoLimit);
    TLegacyReadLimit& operator= (NProto::TReadLimit&& protoLimit);

    TLegacyReadLimit GetSuccessor() const;

    const NProto::TReadLimit& AsProto() const;

    const NTableClient::TLegacyOwningKey& GetLegacyKey() const;
    bool HasLegacyKey() const;
    TLegacyReadLimit& SetLegacyKey(const NTableClient::TLegacyOwningKey& key);
    TLegacyReadLimit& SetLegacyKey(NTableClient::TLegacyOwningKey&& key);

    i64 GetRowIndex() const;
    bool HasRowIndex() const;
    TLegacyReadLimit& SetRowIndex(i64 rowIndex);

    i64 GetOffset() const;
    bool HasOffset() const;
    TLegacyReadLimit& SetOffset(i64 offset);

    i64 GetChunkIndex() const;
    bool HasChunkIndex() const;
    TLegacyReadLimit& SetChunkIndex(i64 chunkIndex);

    i32 GetTabletIndex() const;
    bool HasTabletIndex() const;
    TLegacyReadLimit& SetTabletIndex(i32 tabletIndex);

    bool IsTrivial() const;

    void MergeLowerLegacyKey(const NTableClient::TLegacyOwningKey& key);
    void MergeUpperLegacyKey(const NTableClient::TLegacyOwningKey& key);

    void MergeLowerRowIndex(i64 rowIndex);
    void MergeUpperRowIndex(i64 rowIndex);

    void Persist(const TStreamPersistenceContext& context);

    size_t SpaceUsed() const;

private:
    NProto::TReadLimit ReadLimit_;
    NTableClient::TLegacyOwningKey Key_;

    void InitKey();
    void InitCopy(const NProto::TReadLimit& readLimit);
    void InitMove(NProto::TReadLimit&& readLimit);

};

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TLegacyReadLimit& limit);

bool IsTrivial(const TLegacyReadLimit& limit);
bool IsTrivial(const NProto::TReadLimit& limit);

void ToProto(NProto::TReadLimit* protoReadLimit, const TLegacyReadLimit& readLimit);
void FromProto(TLegacyReadLimit* readLimit, const NProto::TReadLimit& protoReadLimit);

void Serialize(const TLegacyReadLimit& readLimit, NYson::IYsonConsumer* consumer);
void Deserialize(TLegacyReadLimit& readLimit, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

class TLegacyReadRange
{
public:
    TLegacyReadRange() = default;
    TLegacyReadRange(const TLegacyReadLimit& lowerLimit, const TLegacyReadLimit& upperLimit);
    explicit TLegacyReadRange(const TLegacyReadLimit& exact);

    explicit TLegacyReadRange(const NProto::TReadRange& range);
    explicit TLegacyReadRange(NProto::TReadRange&& range);
    TLegacyReadRange& operator= (const NProto::TReadRange& range);
    TLegacyReadRange& operator= (NProto::TReadRange&& range);

    DEFINE_BYREF_RW_PROPERTY(TLegacyReadLimit, LowerLimit);
    DEFINE_BYREF_RW_PROPERTY(TLegacyReadLimit, UpperLimit);

    void Persist(const TStreamPersistenceContext& context);

private:
    void InitCopy(const NProto::TReadRange& range);
    void InitMove(NProto::TReadRange&& range);
};

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TLegacyReadRange& range);

void ToProto(NProto::TReadRange* protoReadRange, const TLegacyReadRange& readRange);
void FromProto(TLegacyReadRange* readRange, const NProto::TReadRange& protoReadRange);

void Serialize(const TLegacyReadRange& readRange, NYson::IYsonConsumer* consumer);
void Deserialize(TLegacyReadRange& readRange, NYTree::INodePtr node);

////////////////////////////////////////////////////////////////////////////////

class TReadLimit
{
public:
    DEFINE_BYREF_RW_PROPERTY(NTableClient::TOwningKeyBound, KeyBound);
    DEFINE_BYVAL_RW_PROPERTY(std::optional<i64>, RowIndex);
    DEFINE_BYVAL_RW_PROPERTY(std::optional<i64>, Offset);
    DEFINE_BYVAL_RW_PROPERTY(std::optional<i64>, ChunkIndex);
    DEFINE_BYVAL_RW_PROPERTY(std::optional<i32>, TabletIndex);

public:
    TReadLimit() = default;
    explicit TReadLimit(const NTableClient::TKeyBound& keyBound);
    explicit TReadLimit(NTableClient::TOwningKeyBound keyBound);
    TReadLimit(
        const NProto::TReadLimit& readLimit,
        bool isUpper,
        std::optional<int> keyLength = std::nullopt);

    bool IsTrivial() const;
};

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TReadLimit& readLimit);

void ToProto(NProto::TReadLimit* protoReadLimit, const TReadLimit& readLimit);
//! If protoReadLimit contains key, it is transformed into new key bound by
//! calling KeyBoundFromLegacyKey using *keyLength. In this case, if keyLength
//! is std::nullopt, exception is thrown.
void FromProto(TReadLimit* readLimit, const NProto::TReadLimit& protoReadLimit, bool isUpper, std::optional<int> keyLength);

void Serialize(const TReadLimit& readLimit, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

class TReadRange
{
public:
    DEFINE_BYREF_RW_PROPERTY(TReadLimit, LowerLimit);
    DEFINE_BYREF_RW_PROPERTY(TReadLimit, UpperLimit);

public:
    TReadRange() = default;
    TReadRange(TReadLimit lowerLimit, TReadLimit upperLimit);
    TReadRange(
        const NProto::TReadRange& range,
        std::optional<int> keyLength = std::nullopt);
};

////////////////////////////////////////////////////////////////////////////////

TString ToString(const TReadRange& readRange);

void ToProto(NProto::TReadRange* protoReadRange, const TReadRange& readRange);
//! See comment for FromProto(TReadLimit*, const NProto::TReadLimit&, bool, std::optional<int>).
void FromProto(TReadRange* readRange, const NProto::TReadRange& protoReadRange, std::optional<int> keyLength);

void Serialize(const TReadRange& readLimit, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

// Interop functions.

//! Transform legacy read limit to new read limit possibly transforming legacy key into
//! key bound by calling KeyBoundFromLegacyKey.
TReadLimit ReadLimitFromLegacyReadLimit(const TLegacyReadLimit& legacyReadLimit, bool isUpper, int keyLength);
//! Transform legacy read limit without legacy key into new read limit (merely copying all integer fields).
TReadLimit ReadLimitFromLegacyReadLimitKeyless(const TLegacyReadLimit& legacyReadLimit);

//! Transform new read limit into legacy read limit.
TLegacyReadLimit ReadLimitToLegacyReadLimit(const TReadLimit& readLimit);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
