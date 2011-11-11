#include "stdafx.h"
#include "cell_master_server.h"

#include <yt/ytlib/meta_state/composite_meta_state.h>

#include <yt/ytlib/transaction_server/transaction_manager.h>
#include <yt/ytlib/transaction_server/transaction_service.h>
#include <yt/ytlib/transaction_server/cypress_integration.h>

#include <yt/ytlib/cypress/cypress_manager.h>
#include <yt/ytlib/cypress/cypress_service.h>
#include <yt/ytlib/cypress/world_initializer.h>
#include <yt/ytlib/cypress/cypress_integration.h>

#include <yt/ytlib/chunk_server/chunk_manager.h>
#include <yt/ytlib/chunk_server/chunk_service.h>
#include <yt/ytlib/chunk_server/cypress_integration.h>

#include <yt/ytlib/file_server/file_manager.h>
#include <yt/ytlib/file_server/file_service.h>

#include <yt/ytlib/table_server/table_manager.h>
#include <yt/ytlib/table_server/table_service.h>

#include <yt/ytlib/monitoring/monitoring_manager.h>
#include <yt/ytlib/monitoring/cypress_integration.h>

#include <yt/ytlib/orchid/cypress_integration.h>

namespace NYT {

static NLog::TLogger Logger("Server");

using NTransaction::TTransactionManager;
using NTransaction::TTransactionService;
using NTransaction::CreateTransactionMapTypeHandler;

using NChunkServer::TChunkManagerConfig;
using NChunkServer::TChunkManager;
using NChunkServer::TChunkService;
using NChunkServer::CreateChunkMapTypeHandler;
using NChunkServer::CreateChunkListMapTypeHandler;

using NMetaState::TCompositeMetaState;

using NCypress::TCypressManager;
using NCypress::TCypressService;
using NCypress::TWorldInitializer;
using NCypress::CreateLockMapTypeHandler;

using NFileServer::TFileManager;
using NFileServer::TFileService;

using NTableServer::TTableManager;
using NTableServer::TTableService;

using NMonitoring::TMonitoringManager;
using NMonitoring::CreateMonitoringTypeHandler;

using NOrchid::CreateOrchidTypeHandler;

////////////////////////////////////////////////////////////////////////////////

void TCellMasterServer::TConfig::Read(TJsonObject* json)
{
    TJsonObject* cellJson = GetSubTree(json, "Cell");
    if (cellJson != NULL) {
        MetaState.Cell.Read(cellJson);
    }

    TJsonObject* metaStateJson = GetSubTree(json, "MetaState");
    if (metaStateJson != NULL) {
        MetaState.Read(metaStateJson);
    }
}

////////////////////////////////////////////////////////////////////////////////

TCellMasterServer::TCellMasterServer(const TConfig& config)
    : Config(config)
{ }

void TCellMasterServer::Run()
{
    // TODO: extract method
    Stroka address = Config.MetaState.Cell.Addresses.at(Config.MetaState.Cell.Id);
    size_t index = address.find_last_of(":");
    int port = FromString<int>(address.substr(index + 1));

    LOG_INFO("Starting cell master on port %d", port);

    auto metaState = New<TCompositeMetaState>();

    auto controlQueue = New<TActionQueue>();

    auto server = New<NRpc::TServer>(port);

    auto metaStateManager = New<TMetaStateManager>(
        Config.MetaState,
        controlQueue->GetInvoker(),
        metaState,
        server);

    auto transactionManager = New<TTransactionManager>(
        TTransactionManager::TConfig(),
        metaStateManager,
        metaState);

    auto transactionService = New<TTransactionService>(
        ~metaStateManager,
        ~transactionManager,
        ~server);

    auto chunkManager = New<TChunkManager>(
        TChunkManagerConfig(),
        ~metaStateManager,
        ~metaState,
        ~transactionManager);

    auto chunkService = New<TChunkService>(
        ~metaStateManager,
        ~chunkManager,
        ~transactionManager,
        ~server);

    auto cypressManager = New<TCypressManager>(
        ~metaStateManager,
        ~metaState,
        ~transactionManager);

    auto cypressService = New<TCypressService>(
        ~metaStateManager,
        ~cypressManager,
        ~transactionManager,
        ~server);

    auto fileManager = New<TFileManager>(
        ~metaStateManager,
        ~metaState,
        ~cypressManager,
        ~chunkManager,
        ~transactionManager);

    auto fileService = New<TFileService>(
        ~metaStateManager,
        ~chunkManager,
        ~fileManager,
        ~server);

    auto tableManager = New<TTableManager>(
        ~metaStateManager,
        ~metaState,
        ~cypressManager,
        ~chunkManager,
        ~transactionManager);

    auto tableService = New<TTableService>(
        ~metaStateManager,
        ~chunkManager,
        ~tableManager,
        ~server);

    auto worldIntializer = New<TWorldInitializer>(
        ~metaStateManager,
        ~cypressManager);
    worldIntializer->Start();

    auto monitoringManager = New<TMonitoringManager>();
    monitoringManager->Register(
        "/refcounted",
        FromMethod(&TRefCountedTracker::GetMonitoringInfo));
    monitoringManager->Register(
        "/meta_state",
        FromMethod(&TMetaStateManager::GetMonitoringInfo, metaStateManager));

    // TODO: register more monitoring infos
    monitoringManager->Start();

    cypressManager->RegisterNodeType(~CreateChunkMapTypeHandler(
        ~cypressManager,
        ~chunkManager));
    cypressManager->RegisterNodeType(~CreateChunkListMapTypeHandler(
        ~cypressManager,
        ~chunkManager));
    cypressManager->RegisterNodeType(~CreateTransactionMapTypeHandler(
        ~cypressManager,
        ~transactionManager));
    cypressManager->RegisterNodeType(~CreateNodeMapTypeHandler(
        ~cypressManager));
    cypressManager->RegisterNodeType(~CreateLockMapTypeHandler(
        ~cypressManager));

    cypressManager->RegisterNodeType(~CreateMonitoringTypeHandler(
        ~cypressManager,
        ~monitoringManager));
    cypressManager->RegisterNodeType(~CreateOrchidTypeHandler(
        ~cypressManager));

    MonitoringServer = new THttpTreeServer(
        monitoringManager->GetProducer(),
        Config.MonitoringPort);

    MonitoringServer->Start();
    metaStateManager->Start();
    server->Start();

    Sleep(TDuration::Max());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
