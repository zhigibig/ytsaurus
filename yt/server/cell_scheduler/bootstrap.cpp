#include "stdafx.h"
#include "bootstrap.h"
#include "config.h"

#include <ytlib/misc/address.h>
#include <ytlib/misc/ref_counted_tracker.h>

#include <ytlib/actions/action_queue.h>

#include <ytlib/bus/tcp_server.h>
#include <ytlib/bus/config.h>

#include <ytlib/rpc/server.h>

#include <ytlib/meta_state/master_channel.h>    

#include <ytlib/meta_state/config.h>

#include <ytlib/orchid/orchid_service.h>

#include <ytlib/monitoring/monitoring_manager.h>
#include <ytlib/monitoring/ytree_integration.h>
#include <ytlib/monitoring/http_server.h>
#include <ytlib/monitoring/http_integration.h>

#include <ytlib/ytree/virtual.h>
#include <ytlib/ytree/ypath_client.h>
#include <ytlib/ytree/yson_file_service.h>

#include <ytlib/profiling/profiling_manager.h>

#include <ytlib/scheduler/config.h>

#include <ytlib/transaction_client/transaction_manager.h>


#include <server/job_proxy/config.h>

#include <server/scheduler/scheduler.h>
#include <server/scheduler/config.h>

#include <yt/build.h>

namespace NYT {
namespace NCellScheduler {

using namespace NBus;
using namespace NElection;
using namespace NMonitoring;
using namespace NObjectClient;
using namespace NOrchid;
using namespace NProfiling;
using namespace NRpc;
using namespace NScheduler;
using namespace NTransactionClient;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger Logger("SchedulerBootstrap");

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(
    const Stroka& configFileName,
    TCellSchedulerConfigPtr config)
    : ConfigFileName(configFileName)
    , Config(config)
{ }

TBootstrap::~TBootstrap()
{ }

void TBootstrap::Run()
{
    PeerAddress = BuildServiceAddress(GetLocalHostName(), Config->RpcPort);

    LOG_INFO("Starting scheduler (PeerAddress: %s, MasterAddresses: [%s])",
        ~PeerAddress,
        ~JoinToString(Config->Masters->Addresses));

    LeaderChannel = CreateLeaderChannel(Config->Masters);

    ControlQueue = New<TFairShareActionQueue>(EControlQueue::GetDomainSize(), "Control");

    BusServer = CreateTcpBusServer(New<TTcpBusServerConfig>(Config->RpcPort));

    auto rpcServer = CreateRpcServer(BusServer);

    TransactionManager = New<TTransactionManager>(
        Config->TransactionManager,
        LeaderChannel);

    Scheduler = New<TScheduler>(Config->Scheduler, this);

    auto monitoringManager = New<TMonitoringManager>();
    monitoringManager->Register(
        "/ref_counted",
        BIND(&TRefCountedTracker::GetMonitoringInfo, TRefCountedTracker::Get()));
    monitoringManager->Start();

    auto orchidFactory = NYTree::GetEphemeralNodeFactory();
    auto orchidRoot = orchidFactory->CreateMap();
    SetNodeByYPath(
        orchidRoot,
        "/monitoring",
        CreateVirtualNode(CreateMonitoringProducer(monitoringManager)));
    SetNodeByYPath(
        orchidRoot,
        "/profiling",
        CreateVirtualNode(
            TProfilingManager::Get()->GetRoot()
            ->Via(TProfilingManager::Get()->GetInvoker())));
    SetNodeByYPath(
        orchidRoot,
        "/config",
        CreateVirtualNode(NYTree::CreateYsonFileProducer(ConfigFileName)));
    SetNodeByYPath(
        orchidRoot,
        "/scheduler",
        CreateVirtualNode(Scheduler->CreateOrchidProducer()));

    SyncYPathSet(orchidRoot, "/@service_name", ConvertToYsonString("scheduler"));
    SyncYPathSet(orchidRoot, "/@version", ConvertToYsonString(YT_VERSION));
    SyncYPathSet(orchidRoot, "/@build_host", ConvertToYsonString(YT_BUILD_HOST));
    SyncYPathSet(orchidRoot, "/@build_time", ConvertToYsonString(YT_BUILD_TIME));
    SyncYPathSet(orchidRoot, "/@build_machine", ConvertToYsonString(YT_BUILD_MACHINE));

    auto orchidService = New<TOrchidService>(
        orchidRoot,
        GetControlInvoker());
    rpcServer->RegisterService(orchidService);

    ::THolder<NHttp::TServer> httpServer(new NHttp::TServer(Config->MonitoringPort));
    httpServer->Register(
        "/orchid",
        NMonitoring::GetYPathHttpHandler(orchidRoot->Via(GetControlInvoker())));

    rpcServer->RegisterService(Scheduler->GetService());

    LOG_INFO("Listening for HTTP requests on port %d", Config->MonitoringPort);
    httpServer->Start();

    LOG_INFO("Listening for RPC requests on port %d", Config->RpcPort);
    rpcServer->Start();

    Scheduler->Start();

    Sleep(TDuration::Max());
}

TCellSchedulerConfigPtr TBootstrap::GetConfig() const
{
    return Config;
}

IChannelPtr TBootstrap::GetLeaderChannel() const
{
    return LeaderChannel;
}

Stroka TBootstrap::GetPeerAddress() const
{
    return PeerAddress;
}

IInvokerPtr TBootstrap::GetControlInvoker(EControlQueue queue) const
{
    return ControlQueue->GetInvoker(queue);
}

TTransactionManagerPtr TBootstrap::GetTransactionManager() const
{
    return TransactionManager;
}

TSchedulerPtr TBootstrap::GetScheduler() const
{
    return Scheduler;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellScheduler
} // namespace NYT
