#include "stdafx.h"
#include "file_writer.h"
#include "file_chunk_meta.pb.h"

#include <ytlib/cypress/cypress_ypath_proxy.h>
#include <ytlib/file_server/file_ypath_proxy.h>
#include <ytlib/ytree/serialize.h>

namespace NYT {
namespace NFileClient {

using namespace NYTree;
using namespace NCypress;
using namespace NChunkServer;
using namespace NChunkClient;
using namespace NFileServer;
using namespace NProto;
using namespace NChunkHolder::NProto;
using namespace NTransactionClient;

////////////////////////////////////////////////////////////////////////////////

// TODO(babenko): use totalReplicaCount

TFileWriter::TFileWriter(
    TConfig* config,
    NRpc::IChannel* masterChannel,
    ITransaction* transaction,
    TTransactionManager* transactionManager,
    const TYPath& path)
    : TFileWriterBase(config, masterChannel)
    , Transaction(transaction)
    , TransactionManager(transactionManager)
    , Path(path)
{
    YASSERT(transactionManager);

    Logger.AddTag(Sprintf("Path: %s, TransactionId: %s",
                          ~Path, transaction ? ~transaction->GetId().ToString() : ~NullTransactionId.ToString()));
}

void TFileWriter::Open()
{
    LOG_INFO("Creating upload transaction");
    try {
        UploadTransaction = TransactionManager->Start(
            NULL,
            Transaction ? Transaction->GetId() : NullTransactionId);
    } catch (const std::exception& ex) {
        LOG_ERROR_AND_THROW(yexception(), "Error creating upload transaction\n%s",
            ex.what());
    }
    ListenTransaction(~UploadTransaction);
    LOG_INFO("Upload transaction created (TransactionId: %s)",
        ~UploadTransaction->GetId().ToString());

    TFileWriterBase::Open(UploadTransaction->GetId());
    if (Transaction) {
        ListenTransaction(~Transaction);
    }

    LOG_INFO("File writer opened");
}

void TFileWriter::SpecificClose(const NChunkServer::TChunkId& ChunkId)
{
    LOG_INFO("Creating file node");
    auto createNodeReq = TCypressYPathProxy::Create(WithTransaction(Path, Transaction ? Transaction->GetId() : NullTransactionId ));
    createNodeReq->set_type(EObjectType::File);
    auto manifest = New<TFileManifest>();
    manifest->ChunkId = ChunkId;
    createNodeReq->set_manifest(SerializeToYson(~manifest));
    auto createNodeRsp = CypressProxy.Execute(~createNodeReq)->Get();
    if (!createNodeRsp->IsOK()) {
        LOG_ERROR_AND_THROW(yexception(), "Error creating file node\n%s",
            ~createNodeRsp->GetError().ToString());
    }
    NodeId = TNodeId::FromProto(createNodeRsp->object_id());
    LOG_INFO("File node created (NodeId: %s)", ~NodeId.ToString());

    LOG_INFO("Committing upload transaction");
    try {
        UploadTransaction->Commit();
    } catch (const std::exception& ex) {
        LOG_ERROR_AND_THROW(yexception(), "Error committing upload transaction\n%s",
            ex.what());
    }
    LOG_INFO("Upload transaction committed");
}

NCypress::TNodeId TFileWriter::GetNodeId() const
{
    return NodeId;
}

void TFileWriter::Cancel()
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (UploadTransaction) {
        UploadTransaction->Abort();
        UploadTransaction.Reset();
    }

    TFileWriterBase::Cancel();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileClient
} // namespace NYT
