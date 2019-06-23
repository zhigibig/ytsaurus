#pragma once

#include <mapreduce/yt/interface/fwd.h>
#include <mapreduce/yt/interface/client_method_options.h>
#include <mapreduce/yt/interface/operation.h>
#include <mapreduce/yt/interface/retry_policy.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class IRequestRetryPolicy;
struct TAuth;
struct TExecuteBatchOptions;

////////////////////////////////////////////////////////////////////////////////

namespace NDetail::NRawClient {

////////////////////////////////////////////////////////////////////////////////

class TRawBatchRequest;

////////////////////////////////////////////////////////////////////////////////

TOperationAttributes ParseOperationAttributes(const TNode& node);

TCheckPermissionResponse ParseCheckPermissionResponse(const TNode& node);

////////////////////////////////////////////////////////////////////////////////

//
// marks `batchRequest' as executed
void ExecuteBatch(
    IRequestRetryPolicyPtr retryPolicy,
    const TAuth& auth,
    TRawBatchRequest& batchRequest,
    const TExecuteBatchOptions& options = TExecuteBatchOptions());

//
// Cypress
//

TNode Get(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& path,
    const TGetOptions& options = TGetOptions());

void Set(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& path,
    const TNode& value,
    const TSetOptions& options = TSetOptions());

bool Exists(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& path);

TNodeId Create(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& path,
    const ENodeType& type,
    const TCreateOptions& options = TCreateOptions());

TNodeId Copy(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& sourcePath,
    const TYPath& destinationPath,
    const TCopyOptions& options = TCopyOptions());

TNodeId Move(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& sourcePath,
    const TYPath& destinationPath,
    const TMoveOptions& options = TMoveOptions());

void Remove(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& path,
    const TRemoveOptions& options = TRemoveOptions());

TNode::TListType List(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& path,
    const TListOptions& options = TListOptions());

TNodeId Link(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& targetPath,
    const TYPath& linkPath,
    const TLinkOptions& options = TLinkOptions());

TLockId Lock(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& path,
    ELockMode mode,
    const TLockOptions& options = TLockOptions());

void Unlock(
    IRequestRetryPolicyPtr retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& path,
    const TUnlockOptions& options = TUnlockOptions());

void Concatenate(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TVector<TYPath>& sourcePaths,
    const TYPath& destinationPath,
    const TConcatenateOptions& options);

//
// Transactions
//

void PingTx(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId);

//
// Operations
//

TOperationAttributes GetOperation(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TOperationId& operationId,
    const TGetOperationOptions& options = TGetOperationOptions());

void AbortOperation(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TOperationId& operationId);

void CompleteOperation(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TOperationId& operationId);

void SuspendOperation(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TOperationId& operationId,
    const TSuspendOperationOptions& options = TSuspendOperationOptions());

void ResumeOperation(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TOperationId& operationId,
    const TResumeOperationOptions& options = TResumeOperationOptions());

TListOperationsResult ListOperations(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TListOperationsOptions& options = TListOperationsOptions());

void UpdateOperationParameters(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TOperationId& operationId,
    const TUpdateOperationParametersOptions& options = TUpdateOperationParametersOptions());

//
// Jobs
//

TJobAttributes GetJob(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TOperationId& operationId,
    const TJobId& jobId,
    const TGetJobOptions& options = TGetJobOptions());

TListJobsResult ListJobs(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TOperationId& operationId,
    const TListJobsOptions& options = TListJobsOptions());

TIntrusivePtr<IFileReader> GetJobInput(
    const TAuth& auth,
    const TJobId& jobId,
    const TGetJobInputOptions& options = TGetJobInputOptions());

TIntrusivePtr<IFileReader> GetJobFailContext(
    const TAuth& auth,
    const TOperationId& operationId,
    const TJobId& jobId,
    const TGetJobFailContextOptions& options = TGetJobFailContextOptions());

TString GetJobStderrWithRetries(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TOperationId& operationId,
    const TJobId& jobId,
    const TGetJobStderrOptions& /* options */ = TGetJobStderrOptions());

TIntrusivePtr<IFileReader> GetJobStderr(
    const TAuth& auth,
    const TOperationId& operationId,
    const TJobId& jobId,
    const TGetJobStderrOptions& options = TGetJobStderrOptions());

//
// File cache
//

TMaybe<TYPath> GetFileFromCache(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TString& md5Signature,
    const TYPath& cachePath,
    const TGetFileFromCacheOptions& options = TGetFileFromCacheOptions());

TYPath PutFileToCache(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TYPath& filePath,
    const TString& md5Signature,
    const TYPath& cachePath,
    const TPutFileToCacheOptions& options = TPutFileToCacheOptions());

TCheckPermissionResponse CheckPermission(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TString& user,
    EPermission permission,
    const TYPath& path,
    const TCheckPermissionOptions& options = TCheckPermissionOptions());

//
// Tables
//

void AlterTable(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TTransactionId& transactionId,
    const TYPath& path,
    const TAlterTableOptions& options);

void AlterTableReplica(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TReplicaId& replicaId,
    const TAlterTableReplicaOptions& options);

void DeleteRows(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TYPath& path,
    const TNode::TListType& keys,
    const TDeleteRowsOptions& options);

void EnableTableReplica(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TReplicaId& replicaId);

void DisableTableReplica(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TReplicaId& replicaId);

void FreezeTable(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TYPath& path,
    const TFreezeTableOptions& options);

void UnfreezeTable(
    const IRequestRetryPolicyPtr& retryPolicy,
    const TAuth& auth,
    const TYPath& path,
    const TUnfreezeTableOptions& options);

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail::NRawClient
} // namespace NYT
