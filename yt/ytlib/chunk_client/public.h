﻿#pragma once

#include <ytlib/misc/common.h>

#include <ytlib/object_client/public.h>

///////////////////////////////////////////////////////////////////////////////

// Forward declarations.
namespace NYT {

class TBlobOutput;
class TFakeStringBufStore;

} // namespace NYT

///////////////////////////////////////////////////////////////////////////////

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

typedef TGuid TIncarnationId;

typedef NObjectClient::TObjectId TChunkId;
extern TChunkId NullChunkId;

typedef NObjectClient::TObjectId TChunkListId;
extern TChunkListId NullChunkListId;

typedef NObjectClient::TObjectId TChunkTreeId;
extern TChunkTreeId NullChunkTreeId;

typedef TGuid TJobId;

DECLARE_ENUM(EJobState,
    (Running)
    (Completed)
    (Failed)
);

DECLARE_ENUM(EJobType,
    (Replicate)
    (Remove)
);

//! Represents an offset inside a chunk.
typedef i64 TBlockOffset;

//! A |(chunkId, blockIndex)| pair.
struct TBlockId;

DECLARE_ENUM(EChunkType,
    ((Unknown) (0))
    ((File)    (1))
    ((Table)   (2))
);

DECLARE_ENUM(EErrorCode,
    ((AllTargetNodesFailed)  (700))
    ((PipelineFailed)        (701))
    ((NoSuchSession)         (702))
    ((SessionAlreadyExists)  (703))
    ((ChunkAlreadyExists)    (704))
    ((WindowError)           (705))
    ((BlockContentMismatch)  (706))
    ((NoSuchBlock)           (707))
    ((NoSuchChunk)           (708))
    ((ChunkPrecachingFailed) (709))
    ((OutOfSpace)            (710))
    ((IOError)               (711))

    ((MasterCommunicationFailed)(712))
);

////////////////////////////////////////////////////////////////////////////////

struct TRemoteReaderConfig;
typedef TIntrusivePtr<TRemoteReaderConfig> TRemoteReaderConfigPtr;

struct TClientBlockCacheConfig;
typedef TIntrusivePtr<TClientBlockCacheConfig> TClientBlockCacheConfigPtr;

struct TEncodingWriterConfig;
typedef TIntrusivePtr<TEncodingWriterConfig> TEncodingWriterConfigPtr;

struct TEncodingWriterOptions;
typedef TIntrusivePtr<TEncodingWriterOptions> TEncodingWriterOptionsPtr;

struct TDispatcherConfig;
typedef TIntrusivePtr<TDispatcherConfig> TDispatcherConfigPtr;

struct TMultiChunkWriterConfig;
typedef TIntrusivePtr<TMultiChunkWriterConfig> TMultiChunkWriterConfigPtr;

struct TMultiChunkWriterOptions;
typedef TIntrusivePtr<TMultiChunkWriterOptions> TMultiChunkWriterOptionsPtr;

struct TMultiChunkReaderConfig;
typedef TIntrusivePtr<TMultiChunkReaderConfig> TMultiChunkReaderConfigPtr;

class TEncodingWriter;
typedef TIntrusivePtr<TEncodingWriter> TEncodingWriterPtr;

struct IAsyncWriter;
typedef TIntrusivePtr<IAsyncWriter> IAsyncWriterPtr;

struct IAsyncReader;
typedef TIntrusivePtr<IAsyncReader> IAsyncReaderPtr;

class TSequentialReader;
typedef TIntrusivePtr<TSequentialReader> TSequentialReaderPtr;

struct IBlockCache;
typedef TIntrusivePtr<IBlockCache> IBlockCachePtr;

struct TSequentialReaderConfig;
typedef TIntrusivePtr<TSequentialReaderConfig> TSequentialReaderConfigPtr;

struct TRemoteWriterConfig;
typedef TIntrusivePtr<TRemoteWriterConfig> TRemoteWriterConfigPtr;

class TRemoteWriter;
typedef TIntrusivePtr<TRemoteWriter> TRemoteWriterPtr;

class TFileReader;
typedef TIntrusivePtr<TFileReader> TFileReaderPtr;

class TFileWriter;
typedef TIntrusivePtr<TFileWriter> TFileWriterPtr;

template <class TChunkWriter>
class TMultiChunkSequentialWriter;

template <class TBuffer>
class TKey;

template <class TStrType>
class TKeyPart;

typedef TKey<TBlobOutput> TOwningKey;
typedef TKey<TFakeStringBufStore> TNonOwningKey;

struct TRefCountedInputChunk;
typedef TIntrusivePtr<TRefCountedInputChunk> TRefCountedInputChunkPtr;

class TChannel;
typedef std::vector<TChannel> TChannels;

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
