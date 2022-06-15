#pragma once

#include "client.h"
#include "structured_table_formats.h"

#include <mapreduce/yt/interface/operation.h>

namespace NYT::NDetail {

////////////////////////////////////////////////////////////////////////////////

class TOperation;

class TOperationPreparer
    : public TThrRefBase
{
public:
    TOperationPreparer(TClientPtr client, TTransactionId transactionId);

    const TAuth& GetAuth() const;
    TTransactionId GetTransactionId() const;

    const TString& GetPreparationId() const;

    void LockFiles(TVector<TRichYPath>* paths);

    TOperationId StartOperation(
        TOperation* operation,
        const TString& operationType,
        const TNode& spec,
        bool useStartOperationRequest = false);

    const IClientRetryPolicyPtr& GetClientRetryPolicy() const;

private:
    TClientPtr Client_;
    TTransactionId TransactionId_;
    THolder<TPingableTransaction> FileTransaction_;
    IClientRetryPolicyPtr ClientRetryPolicy_;
    const TString PreparationId_;

private:
    void CheckValidity() const;
};

using TOperationPreparerPtr = ::TIntrusivePtr<TOperationPreparer>;

////////////////////////////////////////////////////////////////////////////////

struct IItemToUpload
{
    virtual ~IItemToUpload() = default;

    virtual TString CalculateMD5() const = 0;
    virtual THolder<IInputStream> CreateInputStream() const = 0;
    virtual TString GetDescription() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

class TJobPreparer
    : private TNonCopyable
{
public:
    TJobPreparer(
        TOperationPreparer& operationPreparer,
        const TUserJobSpec& spec,
        const IJob& job,
        size_t outputTableCount,
        const TVector<TSmallJobFile>& smallFileList,
        const TOperationOptions& options);

    TVector<TRichYPath> GetFiles() const;
    const TString& GetClassName() const;
    const TString& GetCommand() const;
    const TUserJobSpec& GetSpec() const;
    bool ShouldMountSandbox() const;
    ui64 GetTotalFileSize() const;

private:
    TOperationPreparer& OperationPreparer_;
    TUserJobSpec Spec_;
    TOperationOptions Options_;

    TVector<TRichYPath> CypressFiles_;
    TVector<TRichYPath> CachedFiles_;
    mutable TVector<TString> LockedFileSignatures_;

    TString ClassName_;
    TString Command_;
    ui64 TotalFileSize_ = 0;

private:
    TString GetFileStorage() const;
    TYPath GetCachePath() const;

    bool IsLocalMode() const;
    int GetFileCacheReplicationFactor() const;

    void CreateStorage() const;

    TString UploadToRandomPath(const IItemToUpload& itemToUpload) const;
    TString UploadToCacheUsingApi(const IItemToUpload& itemToUpload) const;
    TString UploadToCache(const IItemToUpload& itemToUpload) const;

    void UseFileInCypress(const TRichYPath& file);

    void UploadLocalFile(
        const TLocalFilePath& localPath,
        const TAddLocalFileOptions& options,
        bool isApiFile = false);

    void UploadBinary(const TJobBinaryConfig& jobBinary);
    void UploadSmallFile(const TSmallJobFile& smallFile);

    void PrepareJobBinary(const IJob& job, int outputTableCount, bool hasState);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDetail
