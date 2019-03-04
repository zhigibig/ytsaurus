#include "client_impl.h"
#include "helpers.h"
#include "config.h"
#include "transaction.h"
#include "private.h"
#include "table_mount_cache.h"
#include "timestamp_provider.h"
#include "credentials_injecting_channel.h"
#include "dynamic_channel_pool.h"

#include <yt/core/net/address.h>

#include <yt/core/misc/small_set.h>

#include <yt/core/ytree/convert.h>

#include <yt/client/api/rowset.h>
#include <yt/client/api/transaction.h>

#include <yt/client/transaction_client/remote_timestamp_provider.h>

#include <yt/client/scheduler/operation_id_or_alias.h>

#include <yt/client/table_client/unversioned_row.h>
#include <yt/client/table_client/row_base.h>
#include <yt/client/table_client/row_buffer.h>
#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/schema.h>
#include <yt/client/table_client/wire_protocol.h>

#include <yt/client/tablet_client/table_mount_cache.h>

#include <yt/client/ypath/rich.h>

namespace NYT::NApi::NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

using NYT::ToProto;
using NYT::FromProto;

using namespace NRpc;
using namespace NTableClient;
using namespace NTabletClient;
using namespace NTransactionClient;
using namespace NScheduler;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

IChannelPtr CreateCredentialsInjectingChannel(
    IChannelPtr underlying,
    const TClientOptions& options)
{
    if (options.Token) {
        return CreateTokenInjectingChannel(
            underlying,
            options.PinnedUser,
            *options.Token);
    } else if (options.SessionId || options.SslSessionId) {
        return CreateCookieInjectingChannel(
            underlying,
            options.PinnedUser,
            options.SessionId.value_or(TString()),
            options.SslSessionId.value_or(TString()));
    } else {
        return CreateUserInjectingChannel(underlying, options.PinnedUser);
    }
}

////////////////////////////////////////////////////////////////////////////////

TClient::TClient(
    TConnectionPtr connection,
    TDynamicChannelPoolPtr channelPool,
    const TClientOptions& clientOptions)
    : Connection_(std::move(connection))
    , ChannelPool_(channelPool)
    , Channel_(CreateCredentialsInjectingChannel(CreateDynamicChannel(channelPool), clientOptions))
    , ClientOptions_(clientOptions)
{ }

const ITableMountCachePtr& TClient::GetTableMountCache()
{
    if (!TableMountCacheInitialized_.load()) {
        auto guard = Guard(TableMountCacheSpinLock_);
        if (!TableMountCache_) {
            TableMountCache_ = CreateTableMountCache(
                Connection_->GetConfig()->TableMountCache,
                Channel_,
                RpcProxyClientLogger,
                Connection_->GetConfig()->RpcTimeout);
        }
        TableMountCacheInitialized_.store(true);
    }
    return TableMountCache_;
}

const ITimestampProviderPtr& TClient::GetTimestampProvider()
{
    if (!TimestampProviderInitialized_.load()) {
        auto guard = Guard(TimestampProviderSpinLock_);
        if (!TimestampProvider_) {
            TimestampProvider_ = CreateBatchingTimestampProvider(
                CreateTimestampProvider(Channel_, Connection_->GetConfig()->RpcTimeout),
                Connection_->GetConfig()->TimestampProviderUpdatePeriod);
        }
        TimestampProviderInitialized_.store(true);
    }
    return TimestampProvider_;
}

TFuture<void> TClient::Terminate()
{
    return VoidFuture;
}

TConnectionPtr TClient::GetRpcProxyConnection()
{
    return Connection_;
}

TClientPtr TClient::GetRpcProxyClient()
{
    return this;
}

IChannelPtr TClient::GetChannel()
{
    return Channel_;
}

IChannelPtr TClient::GetStickyChannel()
{
    return CreateCredentialsInjectingChannel(
        CreateStickyChannel(ChannelPool_),
        ClientOptions_);
}

ITransactionPtr TClient::AttachTransaction(
    TTransactionId transactionId,
    const TTransactionAttachOptions& options)
{
    if (options.Sticky) {
        THROW_ERROR_EXCEPTION("Attaching to sticky transactions is not supported");
    }
    auto connection = GetRpcProxyConnection();
    auto client = GetRpcProxyClient();
    auto channel = GetChannel();

    auto proxy = CreateApiServiceProxy();

    auto req = proxy.AttachTransaction();
    ToProto(req->mutable_transaction_id(), transactionId);
    // COMPAT(kiselyovp): remove auto_abort from the protocol
    req->set_auto_abort(false);
    req->set_sticky(options.Sticky);
    if (options.PingPeriod) {
        req->set_ping_period(options.PingPeriod->GetValue());
    }
    req->set_ping(options.Ping);
    req->set_ping_ancestors(options.PingAncestors);

    auto rspOrError = NConcurrency::WaitFor(req->Invoke());
    const auto& rsp = rspOrError.ValueOrThrow();
    auto transactionType = static_cast<ETransactionType>(rsp->type());
    auto startTimestamp = static_cast<TTimestamp>(rsp->start_timestamp());
    auto atomicity = static_cast<EAtomicity>(rsp->atomicity());
    auto durability = static_cast<EDurability>(rsp->durability());
    auto timeout = TDuration::FromValue(NYT::FromProto<i64>(rsp->timeout()));

    return CreateTransaction(
        std::move(connection),
        std::move(client),
        std::move(channel),
        transactionId,
        startTimestamp,
        transactionType,
        atomicity,
        durability,
        timeout,
        options.PingPeriod,
        options.Sticky);
}

TFuture<void> TClient::MountTable(
    const TYPath& path,
    const TMountTableOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.MountTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    NYT::ToProto(req->mutable_cell_id(), options.CellId);
    if (!options.TargetCellIds.empty()) {
        NYT::ToProto(req->mutable_target_cell_ids(), options.TargetCellIds);
    }
    req->set_freeze(options.Freeze);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::UnmountTable(
    const TYPath& path,
    const TUnmountTableOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.UnmountTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    req->set_force(options.Force);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::RemountTable(
    const TYPath& path,
    const TRemountTableOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.RemountTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::FreezeTable(
    const TYPath& path,
    const TFreezeTableOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.FreezeTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::UnfreezeTable(
    const TYPath& path,
    const TUnfreezeTableOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.UnfreezeTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::ReshardTable(
    const TYPath& path,
    const std::vector<NTableClient::TOwningKey>& pivotKeys,
    const TReshardTableOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.ReshardTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    TWireProtocolWriter writer;
    // XXX(sandello): This is ugly and inefficient.
    std::vector<TUnversionedRow> keys;
    for (const auto& pivotKey : pivotKeys) {
        keys.push_back(pivotKey);
    }
    writer.WriteRowset(MakeRange(keys));
    req->Attachments() = writer.Finish();

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::ReshardTable(
    const TYPath& path,
    int tabletCount,
    const TReshardTableOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.ReshardTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    req->set_tablet_count(tabletCount);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().As<void>();
}

TFuture<std::vector<TTabletActionId>> TClient::ReshardTableAutomatic(
    const TYPath& path,
    const TReshardTableAutomaticOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.ReshardTableAutomatic();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    req->set_keep_actions(options.KeepActions);

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_tablet_range_options(), options);

    return req->Invoke().Apply(BIND([] (const TErrorOr<TApiServiceProxy::TRspReshardTableAutomaticPtr>& rspOrError) {
        const auto& rsp = rspOrError.ValueOrThrow();
        return FromProto<std::vector<TTabletActionId>>(rsp->tablet_actions());
    }));
}

TFuture<void> TClient::TrimTable(
    const TYPath& path,
    int tabletIndex,
    i64 trimmedRowCount,
    const TTrimTableOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.TrimTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    req->set_tablet_index(tabletIndex);
    req->set_trimmed_row_count(trimmedRowCount);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::AlterTable(
    const TYPath& path,
    const TAlterTableOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.AlterTable();
    SetTimeoutOptions(*req, options);

    req->set_path(path);

    if (options.Schema) {
        req->set_schema(ConvertToYsonString(*options.Schema).GetData());
    }
    if (options.Dynamic) {
        req->set_dynamic(*options.Dynamic);
    }
    if (options.UpstreamReplicaId) {
        ToProto(req->mutable_upstream_replica_id(), *options.UpstreamReplicaId);
    }

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_transactional_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::AlterTableReplica(
    TTableReplicaId replicaId,
    const TAlterTableReplicaOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.AlterTableReplica();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_replica_id(), replicaId);

    if (options.Enabled) {
        req->set_enabled(*options.Enabled);
    }
    if (options.Mode) {
        req->set_mode(static_cast<NProto::ETableReplicaMode>(*options.Mode));
    }
    if (options.PreserveTimestamps) {
        req->set_preserve_timestamps(*options.PreserveTimestamps);
    }
    if (options.Atomicity) {
        req->set_atomicity(static_cast<NProto::EAtomicity>(*options.Atomicity));
    }

    return req->Invoke().As<void>();
}

TFuture<std::vector<TTableReplicaId>> TClient::GetInSyncReplicas(
    const TYPath& path,
    TNameTablePtr nameTable,
    const TSharedRange<NTableClient::TKey>& keys,
    const TGetInSyncReplicasOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.GetInSyncReplicas();
    SetTimeoutOptions(*req, options);

    if (options.Timestamp) {
        req->set_timestamp(options.Timestamp);
    }

    req->set_path(path);
    req->Attachments() = SerializeRowset(nameTable, keys, req->mutable_rowset_descriptor());

    return req->Invoke().Apply(BIND([] (const TErrorOr<TApiServiceProxy::TRspGetInSyncReplicasPtr>& rspOrError) {
        const auto& rsp = rspOrError.ValueOrThrow();
        return FromProto<std::vector<TTableReplicaId>>(rsp->replica_ids());
    }));
}

TFuture<std::vector<NApi::TTabletInfo>> TClient::GetTabletInfos(
    const TYPath& path,
    const std::vector<int>& tabletIndexes,
    const TGetTabletsInfoOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.GetTabletInfos();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    ToProto(req->mutable_tablet_indexes(), tabletIndexes);

    return req->Invoke().Apply(BIND([] (const TErrorOr<TApiServiceProxy::TRspGetTabletInfosPtr>& rspOrError) {
        const auto& rsp = rspOrError.ValueOrThrow();
        std::vector<NApi::TTabletInfo> tabletInfos;
        tabletInfos.reserve(rsp->tablets_size());
        for (const auto& protoTabletInfo : rsp->tablets()) {
            tabletInfos.emplace_back();
            auto& result = tabletInfos.back();
            result.TotalRowCount = protoTabletInfo.total_row_count();
            result.TrimmedRowCount = protoTabletInfo.trimmed_row_count();
        }
        return tabletInfos;
    }));
}

TFuture<std::vector<TTabletActionId>> TClient::BalanceTabletCells(
    const TString& tabletCellBundle,
    const std::vector<NYPath::TYPath>& movableTables,
    const NApi::TBalanceTabletCellsOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.BalanceTabletCells();
    SetTimeoutOptions(*req, options);

    req->set_bundle(tabletCellBundle);
    req->set_keep_actions(options.KeepActions);
    ToProto(req->mutable_movable_tables(), movableTables);

    return req->Invoke().Apply(BIND([] (const TErrorOr<TApiServiceProxy::TRspBalanceTabletCellsPtr>& rspOrError) {
        const auto& rsp = rspOrError.ValueOrThrow();
        return FromProto<std::vector<TTabletActionId>>(rsp->tablet_actions());
    }));
}

TFuture<void> TClient::AddMember(
    const TString& group,
    const TString& member,
    const NApi::TAddMemberOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.AddMember();
    SetTimeoutOptions(*req, options);

    req->set_group(group);
    req->set_member(member);
    ToProto(req->mutable_mutating_options(), options);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::RemoveMember(
    const TString& group,
    const TString& member,
    const NApi::TRemoveMemberOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.RemoveMember();
    SetTimeoutOptions(*req, options);

    req->set_group(group);
    req->set_member(member);
    ToProto(req->mutable_mutating_options(), options);

    return req->Invoke().As<void>();
}

TFuture<TCheckPermissionResponse> TClient::CheckPermission(
    const TString& user,
    const NYPath::TYPath& path,
    NYTree::EPermission permission,
    const TCheckPermissionOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.CheckPermission();
    SetTimeoutOptions(*req, options);

    req->set_user(user);
    req->set_path(path);
    req->set_permission(static_cast<int>(permission));
    if (options.Columns) {
        auto* protoColumns = req->mutable_columns();
        ToProto(protoColumns->mutable_items(), *options.Columns);
    }

    ToProto(req->mutable_master_read_options(), options);
    ToProto(req->mutable_transactional_options(), options);
    ToProto(req->mutable_prerequisite_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspCheckPermissionPtr& rsp) {
        TCheckPermissionResponse response;
        static_cast<TCheckPermissionResult&>(response) = FromProto<NApi::TCheckPermissionResult>(rsp->result());
        if (rsp->has_columns()) {
            response.Columns = FromProto<std::vector<TCheckPermissionResult>>(rsp->columns().items());
        }
        return response;
    }));
}

TFuture<TCheckPermissionByAclResult> TClient::CheckPermissionByAcl(
    const std::optional<TString>& user,
    NYTree::EPermission permission,
    NYTree::INodePtr acl,
    const NApi::TCheckPermissionByAclOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.CheckPermissionByAcl();
    SetTimeoutOptions(*req, options);

    if (user) {
        req->set_user(*user);
    }
    req->set_permission(static_cast<int>(permission));
    req->set_acl(ConvertToYsonString(acl).GetData());
    req->set_ignore_missing_subjects(options.IgnoreMissingSubjects);

    ToProto(req->mutable_master_read_options(), options);
    ToProto(req->mutable_prerequisite_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspCheckPermissionByAclPtr& rsp) {
        return FromProto<NApi::TCheckPermissionByAclResult>(rsp->result());
    }));
}

TFuture<NScheduler::TOperationId> TClient::StartOperation(
    NScheduler::EOperationType type,
    const NYson::TYsonString& spec,
    const NApi::TStartOperationOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.StartOperation();
    SetTimeoutOptions(*req, options);

    req->set_type(NProto::ConvertOperationTypeToProto(type));
    req->set_spec(spec.GetData());

    ToProto(req->mutable_mutating_options(), options);
    ToProto(req->mutable_transactional_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspStartOperationPtr& rsp) {
        return FromProto<NScheduler::TOperationId>(rsp->operation_id());
    }));
}

TFuture<void> TClient::AbortOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TAbortOperationOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.AbortOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    if (options.AbortMessage) {
        req->set_abort_message(*options.AbortMessage);
    }

    return req->Invoke().As<void>();
}

TFuture<void> TClient::SuspendOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TSuspendOperationOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.SuspendOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);
    req->set_abort_running_jobs(options.AbortRunningJobs);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::ResumeOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TResumeOperationOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.ResumeOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::CompleteOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TCompleteOperationOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.CompleteOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::UpdateOperationParameters(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NYson::TYsonString& parameters,
    const NApi::TUpdateOperationParametersOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.UpdateOperationParameters();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    req->set_parameters(parameters.GetData());

    return req->Invoke().As<void>();
}

TFuture<NYson::TYsonString> TClient::GetOperation(
    const NScheduler::TOperationIdOrAlias& operationIdOrAlias,
    const NApi::TGetOperationOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.GetOperation();
    SetTimeoutOptions(*req, options);

    NScheduler::ToProto(req, operationIdOrAlias);

    ToProto(req->mutable_master_read_options(), options);
    if (options.Attributes) {
        NYT::ToProto(req->mutable_attributes(), *options.Attributes);
    }
    req->set_include_runtime(options.IncludeRuntime);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetOperationPtr& rsp) {
        return NYson::TYsonString(rsp->meta());
    }));
}

TFuture<void> TClient::DumpJobContext(
    NJobTrackerClient::TJobId jobId,
    const NYPath::TYPath& path,
    const NApi::TDumpJobContextOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.DumpJobContext();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);
    req->set_path(path);

    return req->Invoke().As<void>();
}

TFuture<NYson::TYsonString> TClient::GetJobInputPaths(
    NJobTrackerClient::TJobId jobId,
    const NApi::TGetJobInputPathsOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.GetJobInputPaths();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetJobInputPathsPtr& rsp) {
        return NYson::TYsonString(rsp->paths());
    }));
}

TFuture<TSharedRef> TClient::GetJobStderr(
    NJobTrackerClient::TOperationId operationId,
    NJobTrackerClient::TJobId jobId,
    const NApi::TGetJobStderrOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.GetJobStderr();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_operation_id(), operationId);
    ToProto(req->mutable_job_id(), jobId);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetJobStderrPtr& rsp) {
        YCHECK(rsp->Attachments().size() == 1);
        return rsp->Attachments().front();
    }));
}

TFuture<TSharedRef> TClient::GetJobFailContext(
    NJobTrackerClient::TOperationId operationId,
    NJobTrackerClient::TJobId jobId,
    const NApi::TGetJobFailContextOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.GetJobFailContext();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_operation_id(), operationId);
    ToProto(req->mutable_job_id(), jobId);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetJobFailContextPtr& rsp) {
        YCHECK(rsp->Attachments().size() == 1);
        return rsp->Attachments().front();
    }));
}

TFuture<NApi::TListOperationsResult> TClient::ListOperations(
    const NApi::TListOperationsOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.ListOperations();
    SetTimeoutOptions(*req, options);

    if (options.FromTime) {
        req->set_from_time(NYT::ToProto<i64>(*options.FromTime));
    }
    if (options.ToTime) {
        req->set_to_time(NYT::ToProto<i64>(*options.ToTime));
    }
    if (options.CursorTime) {
        req->set_cursor_time(NYT::ToProto<i64>(*options.CursorTime));
    }
    req->set_cursor_direction(static_cast<NProto::EOperationSortDirection>(options.CursorDirection));
    if (options.UserFilter) {
        req->set_user_filter(*options.UserFilter);
    }

    if (options.AccessFilter) {
        req->set_access_filter(ConvertToYsonString(options.AccessFilter).GetData());
    }

    if (options.StateFilter) {
        req->set_state_filter(NProto::ConvertOperationStateToProto(*options.StateFilter));
    }
    if (options.TypeFilter) {
        req->set_type_filter(NProto::ConvertOperationTypeToProto(*options.TypeFilter));
    }
    if (options.SubstrFilter) {
        req->set_substr_filter(*options.SubstrFilter);
    }
    if (options.Pool) {
        req->set_pool(*options.Pool);
    }
    if (options.WithFailedJobs) {
        req->set_with_failed_jobs(*options.WithFailedJobs);
    }
    req->set_include_archive(options.IncludeArchive);
    req->set_include_counters(options.IncludeCounters);
    req->set_limit(options.Limit);

    ToProto(req->mutable_attributes(), options.Attributes);

    req->set_enable_ui_mode(options.EnableUIMode);

    ToProto(req->mutable_master_read_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspListOperationsPtr& rsp) {
        return FromProto<NApi::TListOperationsResult>(rsp->result());
    }));
}

TFuture<NApi::TListJobsResult> TClient::ListJobs(
    NJobTrackerClient::TOperationId operationId,
    const NApi::TListJobsOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.ListJobs();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_operation_id(), operationId);

    if (options.Type) {
        req->set_type(NProto::ConvertJobTypeToProto(*options.Type));
    }
    if (options.State) {
        req->set_state(NProto::ConvertJobStateToProto(*options.State));
    }
    if (options.Address) {
        req->set_address(*options.Address);
    }
    if (options.WithStderr) {
        req->set_with_stderr(*options.WithStderr);
    }
    if (options.WithFailContext) {
        req->set_with_fail_context(*options.WithFailContext);
    }
    if (options.WithSpec) {
        req->set_with_spec(*options.WithSpec);
    }

    req->set_sort_field(static_cast<NProto::EJobSortField>(options.SortField));
    req->set_sort_order(static_cast<NProto::EJobSortDirection>(options.SortOrder));

    req->set_limit(options.Limit);
    req->set_offset(options.Offset);

    req->set_include_cypress(options.IncludeCypress);
    req->set_include_controller_agent(options.IncludeControllerAgent);
    req->set_include_archive(options.IncludeArchive);

    req->set_data_source(static_cast<NProto::EDataSource>(options.DataSource));
    req->set_running_jobs_lookbehind_period(NYT::ToProto<i64>(options.RunningJobsLookbehindPeriod));

    ToProto(req->mutable_master_read_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspListJobsPtr& rsp) {
        return FromProto<NApi::TListJobsResult>(rsp->result());
    }));
}

TFuture<NYson::TYsonString> TClient::GetJob(
    NJobTrackerClient::TOperationId operationId,
    NJobTrackerClient::TJobId jobId,
    const NApi::TGetJobOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.GetJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_operation_id(), operationId);
    ToProto(req->mutable_job_id(), jobId);

    ToProto(req->mutable_attributes(), options.Attributes);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetJobPtr& rsp) {
        return NYson::TYsonString(rsp->info());
    }));
}

TFuture<NYson::TYsonString> TClient::StraceJob(
    NJobTrackerClient::TJobId jobId,
    const NApi::TStraceJobOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.StraceJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspStraceJobPtr& rsp) {
        return NYson::TYsonString(rsp->trace());
    }));
}

TFuture<void> TClient::SignalJob(
    NJobTrackerClient::TJobId jobId,
    const TString& signalName,
    const NApi::TSignalJobOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.SignalJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);
    req->set_signal_name(signalName);

    return req->Invoke().As<void>();
}

TFuture<void> TClient::AbandonJob(
    NJobTrackerClient::TJobId jobId,
    const NApi::TAbandonJobOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.AbandonJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);

    return req->Invoke().As<void>();
}

TFuture<NYson::TYsonString> TClient::PollJobShell(
    NJobTrackerClient::TJobId jobId,
    const NYson::TYsonString& parameters,
    const NApi::TPollJobShellOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.PollJobShell();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);
    req->set_parameters(parameters.GetData());

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspPollJobShellPtr& rsp) {
        return NYson::TYsonString(rsp->result());
    }));
}

TFuture<void> TClient::AbortJob(
    NJobTrackerClient::TJobId jobId,
    const NApi::TAbortJobOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.AbortJob();
    SetTimeoutOptions(*req, options);

    ToProto(req->mutable_job_id(), jobId);
    if (options.InterruptTimeout) {
        req->set_interrupt_timeout(NYT::ToProto<i64>(*options.InterruptTimeout));
    }

    return req->Invoke().As<void>();
}

TFuture<NApi::TGetFileFromCacheResult> TClient::GetFileFromCache(
    const TString& md5,
    const NApi::TGetFileFromCacheOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.GetFileFromCache();
    SetTimeoutOptions(*req, options);

    req->set_md5(md5);
    req->set_cache_path(options.CachePath);

    ToProto(req->mutable_master_read_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetFileFromCachePtr& rsp) {
        return FromProto<NApi::TGetFileFromCacheResult>(rsp->result());
    }));
}

TFuture<NApi::TPutFileToCacheResult> TClient::PutFileToCache(
    const NYPath::TYPath& path,
    const TString& expectedMD5,
    const NApi::TPutFileToCacheOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.PutFileToCache();
    SetTimeoutOptions(*req, options);

    req->set_path(path);
    req->set_md5(expectedMD5);
    req->set_cache_path(options.CachePath);

    ToProto(req->mutable_prerequisite_options(), options);
    ToProto(req->mutable_master_read_options(), options);
    ToProto(req->mutable_mutating_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspPutFileToCachePtr& rsp) {
        return FromProto<NApi::TPutFileToCacheResult>(rsp->result());
    }));
}

TFuture<std::vector<NTableClient::TColumnarStatistics>> TClient::GetColumnarStatistics(
    const std::vector<NYPath::TRichYPath>& path,
    const NApi::TGetColumnarStatisticsOptions& options)
{
    auto proxy = CreateApiServiceProxy();

    auto req = proxy.GetColumnarStatistics();
    SetTimeoutOptions(*req, options);

    for (const auto& subPath: path) {
        req->add_path(ConvertToYsonString(subPath).GetData());
    }

    req->mutable_fetch_chunk_spec()->set_max_chunk_per_fetch(
        options.FetchChunkSpecConfig->MaxChunksPerFetch);
    req->mutable_fetch_chunk_spec()->set_max_chunk_per_locate_request(
        options.FetchChunkSpecConfig->MaxChunksPerLocateRequest);

    req->mutable_fetcher()->set_node_rpc_timeout(
        NYT::ToProto<i64>(options.FetcherConfig->NodeRpcTimeout));

    ToProto(req->mutable_transactional_options(), options);

    return req->Invoke().Apply(BIND([] (const TApiServiceProxy::TRspGetColumnarStatisticsPtr& rsp) {
        return NYT::FromProto<std::vector<NTableClient::TColumnarStatistics>>(rsp->statistics());
    }));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy
