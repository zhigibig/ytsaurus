#include "stdafx.h"

#include "confirming_writer.h"
#include "config.h"
#include "chunk_ypath_proxy.h"
#include "chunk_replica.h"
#include "private.h"
#include "dispatcher.h"
#include "replication_writer.h"
#include "erasure_writer.h"
#include "chunk_meta_extensions.h"
#include "helpers.h"

#include <ytlib/api/client.h>
#include <ytlib/api/connection.h>

#include <ytlib/new_table_client/chunk_meta_extensions.h>

#include <ytlib/object_client/object_service_proxy.h>
#include <ytlib/object_client/master_ypath_proxy.h>
#include <ytlib/object_client/helpers.h>

#include <core/erasure/codec.h>

#include <core/misc/finally.h>

#include <core/ytree/yson_serializable.h>

#include <core/concurrency/scheduler.h>

#include <core/logging/log.h>

namespace NYT {
namespace NChunkClient {

using namespace NApi;
using namespace NProto;
using namespace NRpc;
using namespace NObjectClient;
using namespace NErasure;
using namespace NConcurrency;
using namespace NYTree;
using namespace NVersionedTableClient;
using namespace NNodeTrackerClient;

///////////////////////////////////////////////////////////////////////////////

class TConfirmingWriter
    : public IChunkWriter
{
public:
    TConfirmingWriter(
        TMultiChunkWriterConfigPtr config,
        TMultiChunkWriterOptionsPtr options,
        const TTransactionId& transactionId,
        const TChunkListId& parentChunkListId,
        TNodeDirectoryPtr nodeDirectory,
        IClientPtr client,
        IBlockCachePtr blockCache,
        IThroughputThrottlerPtr throttler);

    virtual TFuture<void> Open() override;
    virtual bool WriteBlock(const TSharedRef& block) override;
    virtual bool WriteBlocks(const std::vector<TSharedRef>& blocks) override;

    virtual TFuture<void> GetReadyEvent() override;

    virtual TFuture<void> Close(const NProto::TChunkMeta& chunkMeta) override;

    virtual const NProto::TChunkInfo& GetChunkInfo() const override;

    virtual TChunkReplicaList GetWrittenChunkReplicas() const override;

    virtual TChunkId GetChunkId() const override;

    virtual NErasure::ECodec GetErasureCodecId() const override;

private:
    TMultiChunkWriterConfigPtr Config_;
    TMultiChunkWriterOptionsPtr Options_;

    const NTransactionClient::TTransactionId TransactionId_;
    const TChunkListId ParentChunkListId_;

    NNodeTrackerClient::TNodeDirectoryPtr NodeDirectory_;
    NApi::IClientPtr Client_;
    IBlockCachePtr BlockCache_;
    NConcurrency::IThroughputThrottlerPtr Throttler_;

    IChunkWriterPtr UnderlyingWriter_;

    std::atomic<bool> Initialized_ = { false };
    TChunkId ChunkId_ = NullChunkId;
    TFuture<void> OpenFuture_;

    NProto::TChunkMeta ChunkMeta_;

    NLogging::TLogger Logger;

    void OpenSession();
    TChunkId CreateChunk() const;
    IChunkWriterPtr CreateUnderlyingWriter() const;
    void DoClose();
};

///////////////////////////////////////////////////////////////////////////////

TConfirmingWriter::TConfirmingWriter(
    TMultiChunkWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    const NTransactionClient::TTransactionId& transactionId,
    const TChunkListId& parentChunkListId,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    NApi::IClientPtr client,
    IBlockCachePtr blockCache,
    NConcurrency::IThroughputThrottlerPtr throttler)
    : Config_(config)
    , Options_(options)
    , TransactionId_(transactionId)
    , ParentChunkListId_(parentChunkListId)
    , NodeDirectory_(nodeDirectory)
    , Client_(client)
    , BlockCache_(blockCache)
    , Throttler_(throttler)
    , Logger(ChunkClientLogger)
{
    Logger.AddTag("TransactionId: %v", TransactionId_);
}

TFuture<void> TConfirmingWriter::Open()
{
    YCHECK(!Initialized_);
    YCHECK(!OpenFuture_);

    OpenFuture_ = BIND(&TConfirmingWriter::OpenSession, MakeWeak(this))
        .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
        .Run();
    return OpenFuture_;
}

bool TConfirmingWriter::WriteBlock(const TSharedRef& block)
{
    return WriteBlocks(std::vector<TSharedRef>(1, block));
}

bool TConfirmingWriter::WriteBlocks(const std::vector<TSharedRef>& blocks)
{
    YCHECK(Initialized_);
    YCHECK(OpenFuture_.IsSet());

    if (!OpenFuture_.Get().IsOK()) {
        return false;
    } else {
        return UnderlyingWriter_->WriteBlocks(blocks);
    }
}

TFuture<void> TConfirmingWriter::GetReadyEvent()
{
    YCHECK(Initialized_);
    YCHECK(OpenFuture_.IsSet());
    if (!OpenFuture_.Get().IsOK()) {
        return OpenFuture_;
    } else {
        return UnderlyingWriter_->GetReadyEvent();
    }
}

TFuture<void> TConfirmingWriter::Close(const TChunkMeta& chunkMeta)
{
    YCHECK(Initialized_);
    YCHECK(OpenFuture_.IsSet());

    ChunkMeta_ = chunkMeta;
    return BIND(
        &TConfirmingWriter::DoClose,
        MakeWeak(this))
    .AsyncVia(TDispatcher::Get()->GetWriterInvoker())
    .Run();
}

const TChunkInfo& TConfirmingWriter::GetChunkInfo() const
{
    YCHECK(UnderlyingWriter_);
    return UnderlyingWriter_->GetChunkInfo();
}

TChunkReplicaList TConfirmingWriter::GetWrittenChunkReplicas() const
{
    YCHECK(UnderlyingWriter_);
    return UnderlyingWriter_->GetWrittenChunkReplicas();
}

TChunkId TConfirmingWriter::GetChunkId() const
{
    return ChunkId_;
}

NErasure::ECodec TConfirmingWriter::GetErasureCodecId() const
{
    return Options_->ErasureCodec;
}

void TConfirmingWriter::OpenSession()
{
    TFinallyGuard finally([&] () {
        Initialized_ = true;
    });

    ChunkId_ = CreateChunk();

    Logger.AddTag("ChunkId: %v", ChunkId_);
    LOG_DEBUG("Chunk created");

    UnderlyingWriter_ = CreateUnderlyingWriter();
    WaitFor(UnderlyingWriter_->Open())
        .ThrowOnError();

    LOG_DEBUG("Chunk writer opened");
}

TChunkId TConfirmingWriter::CreateChunk() const
{
    auto rspOrError = WaitFor(NChunkClient::CreateChunk(
        Client_->GetMasterChannel(EMasterChannelKind::Leader),
        Options_,
        TransactionId_,
        ParentChunkListId_));

    THROW_ERROR_EXCEPTION_IF_FAILED(
        rspOrError,
        EErrorCode::MasterCommunicationFailed,
        "Error creating chunk");

    const auto& rsp = rspOrError.Value();
    return NYT::FromProto<TChunkId>(rsp->object_ids(0));
}

IChunkWriterPtr TConfirmingWriter::CreateUnderlyingWriter() const
{
    if (Options_->ErasureCodec == ECodec::None) {
        return CreateReplicationWriter(
            Config_,
            Options_,
            ChunkId_,
            TChunkReplicaList(),
            NodeDirectory_,
            Client_,
            BlockCache_,
            Throttler_);
    }

    auto* erasureCodec = GetCodec(Options_->ErasureCodec);
    // NB(psushin): we don't ask master for new erasure replicas,
    // because we cannot guarantee proper replica placement.
    auto options = CloneYsonSerializable(Options_);
    options->AllowAllocatingNewTargetNodes = false;
    auto writers = CreateErasurePartWriters(
        Config_,
        options,
        ChunkId_,
        erasureCodec,
        NodeDirectory_,
        Client_,
        Throttler_,
        BlockCache_);
    return CreateErasureWriter(
        Config_,
        ChunkId_,
        Options_->ErasureCodec,
        erasureCodec,
        writers);
}

void TConfirmingWriter::DoClose()
{
    auto error = WaitFor(UnderlyingWriter_->Close(ChunkMeta_));

    THROW_ERROR_EXCEPTION_IF_FAILED(
        error,
        "Failed to close chunk %v",
        ChunkId_);

    LOG_DEBUG("Chunk closed");

    auto replicas = UnderlyingWriter_->GetWrittenChunkReplicas();
    YCHECK(!replicas.empty());

    static const yhash_set<int> masterMetaTags{
        TProtoExtensionTag<TMiscExt>::Value,
        TProtoExtensionTag<NVersionedTableClient::NProto::TBoundaryKeysExt>::Value,
        TProtoExtensionTag<NVersionedTableClient::NProto::TOldBoundaryKeysExt>::Value
    };

    auto masterChunkMeta = ChunkMeta_;
    FilterProtoExtensions(
        masterChunkMeta.mutable_extensions(),
        ChunkMeta_.extensions(),
        masterMetaTags);

    // At least misc extension must be present.
    YCHECK(masterChunkMeta.extensions().size() > 0);

    auto req = TChunkYPathProxy::Confirm(FromObjectId(ChunkId_));
    GenerateMutationId(req);
    *req->mutable_chunk_info() = UnderlyingWriter_->GetChunkInfo();
    *req->mutable_chunk_meta() = masterChunkMeta;

    NYT::ToProto(req->mutable_replicas(), replicas);

    TObjectServiceProxy objectProxy(Client_->GetMasterChannel(EMasterChannelKind::Leader));
    auto rspOrError = WaitFor(objectProxy.Execute(req));

    THROW_ERROR_EXCEPTION_IF_FAILED(
        rspOrError,
        EErrorCode::MasterCommunicationFailed,
        "Failed to confirm chunk %v",
        ChunkId_);

    LOG_DEBUG("Chunk confirmed");
}

///////////////////////////////////////////////////////////////////////////////

IChunkWriterPtr CreateConfirmingWriter(
    TMultiChunkWriterConfigPtr config,
    TMultiChunkWriterOptionsPtr options,
    const NTransactionClient::TTransactionId& transactionId,
    const TChunkListId& parentChunkListId,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    NApi::IClientPtr client,
    IBlockCachePtr blockCache,
    NConcurrency::IThroughputThrottlerPtr throttler)
{
    return New<TConfirmingWriter>(
        config,
        options,
        transactionId,
        parentChunkListId,
        nodeDirectory,
        client,
        blockCache,
        throttler);
}

///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
