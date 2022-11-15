#pragma once

#include "public.h"
#include "error.h"
#include "mpl.h"
#include "property.h"
#include "serialize_dump.h"

#include <library/cpp/yt/memory/ref.h>

#include <library/cpp/yt/assert/assert.h>

#include <util/stream/buffered.h>
#include <util/stream/file.h>
#include <util/stream/input.h>
#include <util/stream/zerocopy_output.h>

#include <util/system/align.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

//! Alignment size; measured in bytes and must be a power of two.
constexpr size_t SerializationAlignment = 8;
static_assert(
    (SerializationAlignment & (SerializationAlignment - 1)) == 0,
    "SerializationAlignment should be a power of two");

//! The size of the zero buffer used by #WriteZeroes and #WritePadding.
constexpr size_t ZeroBufferSize = 64_KB;
static_assert(
    ZeroBufferSize >= SerializationAlignment,
    "ZeroBufferSize < SerializationAlignment");
extern const std::array<ui8, ZeroBufferSize> ZeroBuffer;

////////////////////////////////////////////////////////////////////////////////

//! When active, causes the process on crash on a deserialization error is
//! encountered. (The default is to throw an exception.)
class TCrashOnDeserializationErrorGuard
{
public:
    TCrashOnDeserializationErrorGuard();
    ~TCrashOnDeserializationErrorGuard();

    TCrashOnDeserializationErrorGuard(const TCrashOnDeserializationErrorGuard&) = delete;
    TCrashOnDeserializationErrorGuard(TCrashOnDeserializationErrorGuard&&) = delete;

    static void OnError();
};

////////////////////////////////////////////////////////////////////////////////

template <class TInput>
size_t ReadRef(TInput& input, TMutableRef ref);
template <class TOutput>
void WriteRef(TOutput& output, TRef ref);

template <class TInput, class T>
void ReadPod(TInput& input, T& obj);
template <class TOutput, class T>
void WritePod(TOutput& output, const T& obj);

template <class TOutput>
size_t WriteZeroes(TOutput& output, size_t count);
template <class TOutput>
size_t WritePadding(TOutput& output, size_t writtenSize);

template <class TOutput>
size_t WriteRefPadded(TOutput& output, TRef ref);
template <class TInput>
size_t ReadRefPadded(TInput& input, TMutableRef ref);

template <class T>
TSharedRef PackRefs(const T& parts);
template <class T>
void UnpackRefs(const TSharedRef& packedRef, T* parts);
std::vector<TSharedRef> UnpackRefs(const TSharedRef& packedRef);

template <class TTag, class TParts>
TSharedRef MergeRefsToRef(const TParts& parts);
template <class TParts>
void MergeRefsToRef(const TParts& parts, TMutableRef dst);
template <class TParts>
TString MergeRefsToString(const TParts& parts);

template <typename T>
void CopyPod(char** buffer, const T& object);

void AssertSerializationAligned(i64 byteSize);
void VerifySerializationAligned(i64 byteSize);
void MakeSerializationAligned(char** buffer, i64 byteSize);

////////////////////////////////////////////////////////////////////////////////

class TSaveContextStream
{
public:
    explicit TSaveContextStream(IOutputStream* output);
    explicit TSaveContextStream(IZeroCopyOutput* output);

    void Write(const void* buf, size_t len);
    void Flush();

private:
    std::optional<TBufferedOutput> BufferedOutput_;
    IZeroCopyOutput* const Output_;

    char* BufferPtr_ = nullptr;
    size_t BufferSize_ = 0;

    void WriteSlow(const void* buf, size_t len);
};

////////////////////////////////////////////////////////////////////////////////

class TStreamSaveContext
{
public:
    explicit TStreamSaveContext(
        IOutputStream* output,
        int version = 0);
    explicit TStreamSaveContext(
        IZeroCopyOutput* output,
        int version = 0);

    virtual ~TStreamSaveContext() = default;

    TSaveContextStream* GetOutput();
    int GetVersion() const;

    void Finish();

protected:
    TSaveContextStream Output_;
    const int Version_;
};

////////////////////////////////////////////////////////////////////////////////

class TStreamLoadContext
{
public:
    DEFINE_BYVAL_RW_PROPERTY(IInputStream*, Input);
    DEFINE_BYREF_RW_PROPERTY(TSerializationDumper, Dumper);
    DEFINE_BYVAL_RW_PROPERTY(int, Version);
    DEFINE_BYVAL_RW_PROPERTY(bool, EnableTotalWriteCountReport);

public:
    TStreamLoadContext();
    explicit TStreamLoadContext(IInputStream* input);

    virtual ~TStreamLoadContext() = default;
};

////////////////////////////////////////////////////////////////////////////////

template <class TSaveContext, class TLoadContext, class TSnapshotVersion>
class TCustomPersistenceContext
{
public:
    // Deliberately not explicit.
    TCustomPersistenceContext(TSaveContext& saveContext);
    TCustomPersistenceContext(TLoadContext& loadContext);

    bool IsSave() const;
    TSaveContext& SaveContext() const;

    bool IsLoad() const;
    TLoadContext& LoadContext() const;

    template <class TOtherContext>
    operator TOtherContext() const;

    TSnapshotVersion GetVersion() const;

private:
    TCustomPersistenceContext(TSaveContext* saveContext, TLoadContext* loadContext);

    TSaveContext* const SaveContext_;
    TLoadContext* const LoadContext_;
};

////////////////////////////////////////////////////////////////////////////////

struct TEntitySerializationKey
{
    constexpr TEntitySerializationKey();
    constexpr explicit TEntitySerializationKey(int index);

    constexpr bool operator == (TEntitySerializationKey rhs) const;
    constexpr bool operator != (TEntitySerializationKey rhs) const;

    constexpr explicit operator bool() const;

    void Save(TEntityStreamSaveContext& context) const;
    void Load(TEntityStreamLoadContext& context);

    int Index;
};

////////////////////////////////////////////////////////////////////////////////

class TEntityStreamSaveContext
    : public TStreamSaveContext
{
public:
    using TStreamSaveContext::TStreamSaveContext;

    TEntitySerializationKey GenerateSerializationKey();

    static inline const TEntitySerializationKey InlineKey = TEntitySerializationKey(-3);

    template <class T>
    TEntitySerializationKey RegisterRawEntity(T* entity);
    template <class T>
    TEntitySerializationKey RegisterRefCountedEntity(const TIntrusivePtr<T>& entity);

private:
    int SerializationKeyIndex_ = 0;
    THashMap<void*, TEntitySerializationKey> RawPtrs_;
    THashMap<TRefCountedPtr, TEntitySerializationKey> RefCountedPtrs_;
};

////////////////////////////////////////////////////////////////////////////////

class TEntityStreamLoadContext
    : public TStreamLoadContext
{
public:
    template <class T>
    TEntitySerializationKey RegisterRawEntity(T* entity);
    template <class T>
    TEntitySerializationKey RegisterRefCountedEntity(const TIntrusivePtr<T>& entity);

    template <class T>
    T* GetRawEntity(TEntitySerializationKey key) const;
    template <class T>
    TIntrusivePtr<T> GetRefCountedEntity(TEntitySerializationKey key) const;

private:
    std::vector<void*> RawPtrs_;
    std::vector<TIntrusivePtr<TRefCounted>> RefCountedPtrs_;
};

////////////////////////////////////////////////////////////////////////////////

template <class T, class C, class... TArgs>
void Save(C& context, const T& value, TArgs&&... args);

template <class T, class C, class... TArgs>
void Load(C& context, T& value, TArgs&&... args);

template <class T, class C, class... TArgs>
T Load(C& context, TArgs&&... args);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

#define SERIALIZE_INL_H_
#include "serialize-inl.h"
#undef SERIALIZE_INL_H_

