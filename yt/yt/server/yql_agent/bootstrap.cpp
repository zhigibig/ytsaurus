#include "bootstrap.h"

#include "config.h"
#include "private.h"
#include "yql_agent.h"
#include "yql_service.h"
#include "dynamic_config_manager.h"

#include <yt/yt/server/lib/admin/admin_service.h>

#include <yt/yt/server/lib/cypress_election/election_manager.h>

#include <yt/yt/ytlib/api/native/client.h>
#include <yt/yt/ytlib/api/native/config.h>
#include <yt/yt/ytlib/api/native/connection.h>
#include <yt/yt/ytlib/api/native/helpers.h>

#include <yt/yt/library/monitoring/http_integration.h>
#include <yt/yt/library/monitoring/monitoring_manager.h>

#include <yt/yt/ytlib/orchid/orchid_service.h>

#include <yt/yt/ytlib/hive/cluster_directory_synchronizer.h>

#include <yt/yt/library/program/build_attributes.h>
#include <yt/yt/library/program/config.h>
#include <yt/yt/ytlib/program/helpers.h>

#include <yt/yt/core/bus/server.h>

#include <yt/yt/core/bus/tcp/server.h>

#include <yt/yt/core/http/server.h>

#include <yt/yt/core/concurrency/action_queue.h>

#include <yt/yt/core/net/address.h>
#include <yt/yt/core/net/local_address.h>

#include <yt/yt/library/coredumper/coredumper.h>
#include <yt/yt/core/misc/ref_counted_tracker.h>

#include <yt/yt/core/rpc/bus/server.h>

#include <yt/yt/core/ypath/token.h>

#include <yt/yt/core/ytree/virtual.h>
#include <yt/yt/core/ytree/ypath_client.h>

namespace NYT::NYqlAgent {

using namespace NAdmin;
using namespace NBus;
using namespace NElection;
using namespace NHydra;
using namespace NMonitoring;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NOrchid;
using namespace NProfiling;
using namespace NRpc;
using namespace NTransactionClient;
using namespace NSecurityClient;
using namespace NYTree;
using namespace NYPath;
using namespace NConcurrency;
using namespace NApi;
using namespace NNodeTrackerClient;
using namespace NLogging;
using namespace NCypressElection;
using namespace NHiveClient;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = YqlAgentLogger;

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(TYqlAgentServerConfigPtr config, INodePtr configNode)
    : Config_(std::move(config))
    , ConfigNode_(std::move(configNode))
{
    if (Config_->AbortOnUnrecognizedOptions) {
        AbortOnUnrecognizedOptions(Logger, Config_);
    } else {
        WarnForUnrecognizedOptions(Logger, Config_);
    }
}

TBootstrap::~TBootstrap() = default;

void TBootstrap::Run()
{
    ControlQueue_ = New<TActionQueue>("Control");
    ControlInvoker_ = ControlQueue_->GetInvoker();

    BIND(&TBootstrap::DoRun, this)
        .AsyncVia(ControlInvoker_)
        .Run()
        .Get()
        .ThrowOnError();

    Sleep(TDuration::Max());
}

void TBootstrap::DoRun()
{
    YT_LOG_INFO(
        "Starting Yql agent process (NativeCluster: %v, User: %v)",
        Config_->ClusterConnection->ClusterName,
        Config_->User);

    AgentId_ = NNet::BuildServiceAddress(NNet::GetLocalHostName(), Config_->RpcPort);

    NApi::NNative::TConnectionOptions connectionOptions;
    connectionOptions.RetryRequestQueueSizeLimitExceeded = true;
    NativeConnection_ = NApi::NNative::CreateConnection(
        Config_->ClusterConnection,
        std::move(connectionOptions));

    NativeConnection_->GetClusterDirectorySynchronizer()->Start();

    NativeAuthenticator_ = NNative::CreateNativeAuthenticator(NativeConnection_);

    auto clientOptions = TClientOptions::FromUser(Config_->User);
    NativeClient_ = NativeConnection_->CreateNativeClient(clientOptions);

    const auto& clusterDirectory = NativeConnection_->GetClusterDirectory();
    const auto& clusterDirectorySynchronizer = NativeConnection_->GetClusterDirectorySynchronizer();
    WaitFor(clusterDirectorySynchronizer->Sync(/*force*/ true))
        .ThrowOnError();

    DynamicConfigManager_ = New<TDynamicConfigManager>(Config_, NativeClient_, ControlInvoker_);
    DynamicConfigManager_->SubscribeConfigChanged(BIND(&TBootstrap::OnDynamicConfigChanged, Unretained(this)));

    BusServer_ = CreateTcpBusServer(Config_->BusServer);

    RpcServer_ = NRpc::NBus::CreateBusServer(BusServer_);

    HttpServer_ = NHttp::CreateServer(Config_->CreateMonitoringHttpServerConfig());

    if (Config_->CoreDumper) {
        CoreDumper_ = NCoreDump::CreateCoreDumper(Config_->CoreDumper);
    }

    {
        TCypressElectionManagerOptionsPtr options = New<TCypressElectionManagerOptions>();
        options->GroupName = "YqlAgent";
        options->MemberName = AgentId_;
        options->TransactionAttributes = CreateEphemeralAttributes();
        options->TransactionAttributes->Set("host", AgentId_);
        ElectionManager_ = CreateCypressElectionManager(NativeClient_, ControlInvoker_, Config_->ElectionManager, std::move(options));
    }

    YqlAgent_ = CreateYqlAgent(
        Config_->YqlAgent,
        clusterDirectory,
        ControlInvoker_,
        ElectionManager_,
        AgentId_);

    DynamicConfigManager_->Start();

    NYTree::IMapNodePtr orchidRoot;
    NMonitoring::Initialize(
        HttpServer_,
        Config_->SolomonExporter,
        &MonitoringManager_,
        &orchidRoot);

    SetNodeByYPath(
        orchidRoot,
        "/config",
        CreateVirtualNode(ConfigNode_));
    SetNodeByYPath(
        orchidRoot,
        "/dynamic_config_manager",
        CreateVirtualNode(DynamicConfigManager_->GetOrchidService()));
    if (CoreDumper_) {
        SetNodeByYPath(
            orchidRoot,
            "/core_dumper",
            CreateVirtualNode(CoreDumper_->CreateOrchidService()));
    }
    SetNodeByYPath(
        orchidRoot,
        "/yql_agent",
        YqlAgent_->GetOrchidNode());
    SetBuildAttributes(
        orchidRoot,
        "yql_agent");

    RpcServer_->RegisterService(CreateAdminService(
        ControlInvoker_,
        CoreDumper_,
        NativeAuthenticator_));
    RpcServer_->RegisterService(CreateOrchidService(
        orchidRoot,
        ControlInvoker_,
        NativeAuthenticator_));
    RpcServer_->RegisterService(CreateYqlService(
        ControlInvoker_,
        YqlAgent_));

    YT_LOG_INFO("Listening for HTTP requests (Port: %v)", Config_->MonitoringPort);
    HttpServer_->Start();

    YT_LOG_INFO("Listening for RPC requests (Port: %v)", Config_->RpcPort);
    RpcServer_->Configure(Config_->RpcServer);
    RpcServer_->Start();

    UpdateCypressNode();

    ElectionManager_->SubscribeLeadingStarted(BIND(&IYqlAgent::Start, YqlAgent_));
    ElectionManager_->SubscribeLeadingEnded(BIND(&IYqlAgent::Stop, YqlAgent_));

    ElectionManager_->Start();
}

void TBootstrap::UpdateCypressNode()
{
    while (true) {
        try {
            GuardedUpdateCypressNode();
        } catch (const std::exception& ex) {
            YT_LOG_DEBUG(ex, "Error updating cypress node");
            continue;
        }
        return;
    }
}

void TBootstrap::GuardedUpdateCypressNode()
{
    VERIFY_INVOKER_AFFINITY(ControlInvoker_);

    auto instancePath = Format("%v/instances/%v", Config_->Root, ToYPathLiteral(AgentId_));

    {
        TCreateNodeOptions options;
        options.Recursive = true;
        options.Force = true;
        options.Attributes = ConvertToAttributes(
            BuildYsonStringFluently().BeginMap()
                .Item("annotations").Value(Config_->CypressAnnotations)
            .EndMap());

        YT_LOG_INFO("Creating instance node (Path: %Qv)", instancePath);

        WaitFor(NativeClient_->CreateNode(instancePath, EObjectType::MapNode, options))
            .ThrowOnError();

        YT_LOG_INFO("Instance node created");
    }
    {
        TCreateNodeOptions options;
        options.Attributes = ConvertToAttributes(
            BuildYsonStringFluently().BeginMap()
                .Item("remote_addresses").BeginMap()
                    .Item("default").Value(AgentId_)
                .EndMap()
            .EndMap());

        auto orchidPath = instancePath + "/orchid";

        YT_LOG_INFO("Creating orchid node (Path: %Qv)", orchidPath);

        WaitFor(NativeClient_->CreateNode(orchidPath, EObjectType::Orchid, options))
            .ThrowOnError();

        YT_LOG_INFO("Orchid node created");
    }
}

void TBootstrap::OnDynamicConfigChanged(
    const TYqlAgentServerDynamicConfigPtr& oldConfig,
    const TYqlAgentServerDynamicConfigPtr& newConfig)
{
    ReconfigureNativeSingletons(Config_, newConfig);

    YT_VERIFY(YqlAgent_);

    std::vector<TFuture<void>> asyncUpdateComponents{
        BIND(
            &IYqlAgent::OnDynamicConfigChanged,
            YqlAgent_,
            oldConfig->YqlAgent,
            newConfig->YqlAgent)
            .AsyncVia(ControlInvoker_)
            .Run(),
    };
    WaitFor(AllSucceeded(asyncUpdateComponents))
        .ThrowOnError();

    YT_LOG_DEBUG(
        "Updated Yql agent server dynamic config (OldConfig: %v, NewConfig: %v)",
        ConvertToYsonString(oldConfig, EYsonFormat::Text),
        ConvertToYsonString(newConfig, EYsonFormat::Text));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYqlAgent
