#pragma once

#include <mapreduce/yt/interface/client.h>

#include <library/cpp/testing/gmock_in_unittest/gmock.h>

namespace NYT {
namespace NTesting {
    class TClientMock : public IClient {
    public:
        TClientMock();
        ~TClientMock();

        MOCK_METHOD(TNodeId, Create, (const TYPath&, ENodeType, const TCreateOptions&), (override));
        MOCK_METHOD(void, Remove, (const TYPath&, const TRemoveOptions&), (override));
        MOCK_METHOD(bool, Exists, (const TYPath&, const TExistsOptions&), (override));
        MOCK_METHOD(TNode, Get, (const TYPath&, const TGetOptions&), (override));
        MOCK_METHOD(void, Set, (const TYPath&, const TNode&, const TSetOptions&), (override));
        MOCK_METHOD(void, MultisetAttributes, (const TYPath&, const TNode::TMapType&, const TMultisetAttributesOptions&), (override));
        MOCK_METHOD(TNode::TListType, List, (const TYPath&, const TListOptions&), (override));
        MOCK_METHOD(TNodeId, Copy, (const TYPath&, const TYPath&, const TCopyOptions&), (override));
        MOCK_METHOD(TNodeId, Move, (const TYPath&, const TYPath&, const TMoveOptions&), (override));
        MOCK_METHOD(TNodeId, Link, (const TYPath&, const TYPath&, const TLinkOptions&), (override));
        MOCK_METHOD(void, Concatenate, (const TVector<TRichYPath>&, const TRichYPath&, const TConcatenateOptions&), (override));
        MOCK_METHOD(void, Concatenate, (const TVector<TYPath>&, const TYPath&, const TConcatenateOptions&), (override));
        MOCK_METHOD(TRichYPath, CanonizeYPath, (const TRichYPath&), (override));
        MOCK_METHOD(TVector<TTableColumnarStatistics>, GetTableColumnarStatistics, (const TVector<TRichYPath>&, const TGetTableColumnarStatisticsOptions&), (override));
        MOCK_METHOD(TBatchRequestPtr, CreateBatchRequest, (), (override));
        MOCK_METHOD(TVector<TTabletInfo>, GetTabletInfos, (const TYPath&, const TVector<int>&, const TGetTabletInfosOptions&), (override));

        MOCK_METHOD(IFileReaderPtr, CreateFileReader, (const TRichYPath&, const TFileReaderOptions&), (override));
        MOCK_METHOD(IFileWriterPtr, CreateFileWriter, (const TRichYPath&, const TFileWriterOptions&), (override));
        MOCK_METHOD(IFileReaderPtr, CreateBlobTableReader, (const TYPath&, const TKey&, const TBlobTableReaderOptions&), (override));
        MOCK_METHOD(TTableWriterPtr<::google::protobuf::Message>, CreateTableWriter, (const TRichYPath& path, const ::google::protobuf::Descriptor& descriptor, const TTableWriterOptions& options), (override));
        MOCK_METHOD(TRawTableReaderPtr, CreateRawReader, (const TRichYPath& path, const TFormat& format, const TTableReaderOptions& options), (override));
        MOCK_METHOD(TRawTableWriterPtr, CreateRawWriter, (const TRichYPath& path, const TFormat& format, const TTableWriterOptions& options), (override));
        MOCK_METHOD(::TIntrusivePtr<INodeReaderImpl>, CreateNodeReader, (const TRichYPath&, const TTableReaderOptions&), (override));
        MOCK_METHOD(::TIntrusivePtr<IYaMRReaderImpl>, CreateYaMRReader, (const TRichYPath&, const TTableReaderOptions&), (override));
        MOCK_METHOD(::TIntrusivePtr<IProtoReaderImpl>, CreateProtoReader, (const TRichYPath&, const TTableReaderOptions&, const ::google::protobuf::Message*), (override));
        MOCK_METHOD(::TIntrusivePtr<INodeWriterImpl>, CreateNodeWriter, (const TRichYPath&, const TTableWriterOptions&), (override));
        MOCK_METHOD(::TIntrusivePtr<IYaMRWriterImpl>, CreateYaMRWriter, (const TRichYPath&, const TTableWriterOptions&), (override));
        MOCK_METHOD(::TIntrusivePtr<IProtoWriterImpl>, CreateProtoWriter, (const TRichYPath&, const TTableWriterOptions&, const ::google::protobuf::Message*), (override));

        MOCK_METHOD(IOperationPtr, Sort, (const TSortOperationSpec&, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, Merge, (const TMergeOperationSpec&, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, Erase, (const TEraseOperationSpec&, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, RemoteCopy, (const TRemoteCopyOperationSpec&, const TOperationOptions&), (override));
        MOCK_METHOD(void, AbortOperation, (const TOperationId&), (override));
        MOCK_METHOD(void, CompleteOperation, (const TOperationId&), (override));
        MOCK_METHOD(void, SuspendOperation, (const TOperationId&, const TSuspendOperationOptions&), (override));
        MOCK_METHOD(void, ResumeOperation, (const TOperationId&, const TResumeOperationOptions&), (override));
        MOCK_METHOD(void, WaitForOperation, (const TOperationId&), (override));
        MOCK_METHOD(EOperationBriefState, CheckOperation, (const TOperationId&), (override));
        MOCK_METHOD(IOperationPtr, DoMap, (const TMapOperationSpec&, const IStructuredJob&, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, RawMap, (const TRawMapOperationSpec&, ::TIntrusivePtr<IRawJob>, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, DoReduce, (const TReduceOperationSpec&, const IStructuredJob&, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, RawReduce, (const TRawReduceOperationSpec&, ::TIntrusivePtr<IRawJob>, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, DoJoinReduce, (const TJoinReduceOperationSpec&, const IStructuredJob&, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, RawJoinReduce, (const TRawJoinReduceOperationSpec&, ::TIntrusivePtr<IRawJob>, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, DoMapReduce, (const TMapReduceOperationSpec&, const IStructuredJob*, const IStructuredJob*, const IStructuredJob&, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, RawMapReduce, (const TRawMapReduceOperationSpec&, ::TIntrusivePtr<IRawJob>, ::TIntrusivePtr<IRawJob>, ::TIntrusivePtr<IRawJob>, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, RunVanilla, (const TVanillaOperationSpec&, const TOperationOptions&), (override));
        MOCK_METHOD(IOperationPtr, AttachOperation, (const TOperationId&), (override));

        MOCK_METHOD(ITransactionPtr, StartTransaction, (const TStartTransactionOptions&), (override));
        MOCK_METHOD(void, AlterTable, (const TYPath&, const TAlterTableOptions&), (override));

        MOCK_METHOD(ITransactionPtr, AttachTransaction, (const TTransactionId&, const TAttachTransactionOptions&), (override));
        MOCK_METHOD(void, MountTable, (const TYPath&, const TMountTableOptions&), (override));
        MOCK_METHOD(void, UnmountTable, (const TYPath&, const TUnmountTableOptions&), (override));
        MOCK_METHOD(void, RemountTable, (const TYPath&, const TRemountTableOptions&), (override));
        MOCK_METHOD(void, FreezeTable, (const TYPath&, const TFreezeTableOptions&), (override));
        MOCK_METHOD(void, UnfreezeTable, (const TYPath&, const TUnfreezeTableOptions&), (override));
        MOCK_METHOD(void, ReshardTable, (const TYPath&, const TVector<TKey>&, const TReshardTableOptions&), (override));
        MOCK_METHOD(void, ReshardTable, (const TYPath&, i64, const TReshardTableOptions&), (override));
        MOCK_METHOD(void, InsertRows, (const TYPath&, const TNode::TListType&, const TInsertRowsOptions&), (override));
        MOCK_METHOD(void, DeleteRows, (const TYPath&, const TNode::TListType&, const TDeleteRowsOptions&), (override));
        MOCK_METHOD(void, TrimRows, (const TYPath&, i64, i64, const TTrimRowsOptions&), (override));
        MOCK_METHOD(TNode::TListType, LookupRows, (const TYPath&, const TNode::TListType&, const TLookupRowsOptions&), (override));
        MOCK_METHOD(TNode::TListType, SelectRows, (const TString&, const TSelectRowsOptions&), (override));
        MOCK_METHOD(ui64, GenerateTimestamp, (), (override));
        MOCK_METHOD(void, EnableTableReplica, (const TReplicaId& replicaid), ());
        MOCK_METHOD(void, DisableTableReplica, (const TReplicaId& replicaid), ());
        MOCK_METHOD(void, AlterTableReplica, (const TReplicaId& replicaid, const TAlterTableReplicaOptions&), (override));

        MOCK_METHOD(TAuthorizationInfo, WhoAmI, (), (override));

        MOCK_METHOD(TOperationAttributes, GetOperation, (const TOperationId&, const TGetOperationOptions&), (override));
        MOCK_METHOD(TListOperationsResult, ListOperations, (const TListOperationsOptions&), (override));
        MOCK_METHOD(void, UpdateOperationParameters, (const TOperationId&, const TUpdateOperationParametersOptions&), (override));
        MOCK_METHOD(TJobAttributes, GetJob, (const TOperationId&, const TJobId&, const TGetJobOptions&), (override));
        MOCK_METHOD(TListJobsResult, ListJobs, (const TOperationId&, const TListJobsOptions&), (override));
        MOCK_METHOD(IFileReaderPtr, GetJobInput, (const TJobId&, const TGetJobInputOptions&), (override));
        MOCK_METHOD(IFileReaderPtr, GetJobFailContext, (const TOperationId&, const TJobId&, const TGetJobFailContextOptions&), (override));
        MOCK_METHOD(IFileReaderPtr, GetJobStderr, (const TOperationId&, const TJobId&, const TGetJobStderrOptions&), (override));

        MOCK_METHOD(TNode::TListType, SkyShareTable, (const std::vector<TYPath>&, const TSkyShareTableOptions&), (override));
        MOCK_METHOD(TMaybe<TYPath>, GetFileFromCache, (const TString& md5Signature, const TYPath& cachePath, const TGetFileFromCacheOptions&), (override));
        MOCK_METHOD(TYPath, PutFileToCache, (const TYPath&, const TString& md5Signature, const TYPath& cachePath, const TPutFileToCacheOptions&), (override));
        MOCK_METHOD(TCheckPermissionResponse, CheckPermission, (const TString&, EPermission, const TYPath&, const TCheckPermissionOptions&), (override));

        MOCK_METHOD(IClientPtr, GetParentClient, (), (override));

        MOCK_METHOD(TNodeId, CreateTable, (const TYPath&, const ::google::protobuf::Descriptor&, const TSortColumns&, const TCreateOptions&), ());
    };

    class TTransactionMock : public ITransaction {
    public:
        TTransactionMock();
        ~TTransactionMock();

        MOCK_METHOD(TNodeId, Create, (const TYPath&, ENodeType, const TCreateOptions&), (override));
        MOCK_METHOD(void, Remove, (const TYPath&, const TRemoveOptions&), (override));
        MOCK_METHOD(bool, Exists, (const TYPath&, const TExistsOptions&), (override));
        MOCK_METHOD(TNode, Get, (const TYPath&, const TGetOptions&), (override));
        MOCK_METHOD(void, Set, (const TYPath&, const TNode&, const TSetOptions&), (override));
        MOCK_METHOD(TNode::TListType, List, (const TYPath&, const TListOptions&), (override));
        MOCK_METHOD(TNodeId, Copy, (const TYPath&, const TYPath&, const TCopyOptions&), (override));
        MOCK_METHOD(TNodeId, Move, (const TYPath&, const TYPath&, const TMoveOptions&), (override));
        MOCK_METHOD(TNodeId, Link, (const TYPath&, const TYPath&, const TLinkOptions&), (override));
        MOCK_METHOD(void, Concatenate, (const TVector<TYPath>&, const TYPath&, const TConcatenateOptions&), (override));
        MOCK_METHOD(TRichYPath, CanonizeYPath, (const TRichYPath&), (override));
        MOCK_METHOD(TVector<TTableColumnarStatistics>, GetTableColumnarStatistics, (const TVector<TRichYPath>&, const TGetTableColumnarStatisticsOptions&), (override));
        MOCK_METHOD(TBatchRequestPtr, CreateBatchRequest, (), (override));

        MOCK_METHOD(IFileReaderPtr, CreateFileReader, (const TRichYPath&, const TFileReaderOptions&), (override));
        MOCK_METHOD(IFileWriterPtr, CreateFileWriter, (const TRichYPath&, const TFileWriterOptions&), (override));
        MOCK_METHOD(TTableWriterPtr<::google::protobuf::Message>, CreateTableWriter, (const TRichYPath& path, const ::google::protobuf::Descriptor& descriptor, const TTableWriterOptions& options), (override));
        MOCK_METHOD(TRawTableReaderPtr, CreateRawReader, (const TRichYPath& path, const TFormat& format, const TTableReaderOptions& options), (override));
        MOCK_METHOD(TRawTableWriterPtr, CreateRawWriter, (const TRichYPath& path, const TFormat& format, const TTableWriterOptions& options), (override));
        MOCK_METHOD3(CreateBlobTableReader, IFileReaderPtr(const TYPath&, const TKey&, const TBlobTableReaderOptions&));
        MOCK_METHOD2(CreateNodeReader, ::TIntrusivePtr<INodeReaderImpl>(const TRichYPath&, const TTableReaderOptions&));
        MOCK_METHOD2(CreateYaMRReader, ::TIntrusivePtr<IYaMRReaderImpl>(const TRichYPath&, const TTableReaderOptions&));
        MOCK_METHOD3(CreateProtoReader, ::TIntrusivePtr<IProtoReaderImpl>(const TRichYPath&, const TTableReaderOptions&, const ::google::protobuf::Message*));
        MOCK_METHOD2(CreateNodeWriter, ::TIntrusivePtr<INodeWriterImpl>(const TRichYPath&, const TTableWriterOptions&));
        MOCK_METHOD2(CreateYaMRWriter, ::TIntrusivePtr<IYaMRWriterImpl>(const TRichYPath&, const TTableWriterOptions&));
        MOCK_METHOD3(CreateProtoWriter, ::TIntrusivePtr<IProtoWriterImpl>(const TRichYPath&, const TTableWriterOptions&, const ::google::protobuf::Message*));

        MOCK_METHOD2(Sort, IOperationPtr(const TSortOperationSpec&, const TOperationOptions&));
        MOCK_METHOD2(Merge, IOperationPtr(const TMergeOperationSpec&, const TOperationOptions&));
        MOCK_METHOD2(Erase, IOperationPtr(const TEraseOperationSpec&, const TOperationOptions&));
        MOCK_METHOD1(AbortOperation, void(const TOperationId&));
        MOCK_METHOD1(CompleteOperation, void(const TOperationId&));
        MOCK_METHOD1(WaitForOperation, void(const TOperationId&));
        MOCK_METHOD1(CheckOperation, EOperationBriefState(const TOperationId&));
        MOCK_METHOD3(DoMap, IOperationPtr(const TMapOperationSpec&, const IStructuredJob&, const TOperationOptions&));
        MOCK_METHOD3(RawMap, IOperationPtr(const TRawMapOperationSpec&, ::TIntrusivePtr<IRawJob>, const TOperationOptions&));
        MOCK_METHOD3(DoReduce, IOperationPtr(const TReduceOperationSpec&, const IStructuredJob&, const TOperationOptions&));
        MOCK_METHOD3(RawReduce, IOperationPtr(const TRawReduceOperationSpec&, ::TIntrusivePtr<IRawJob>, const TOperationOptions&));
        MOCK_METHOD3(DoJoinReduce, IOperationPtr(const TJoinReduceOperationSpec&, const IStructuredJob&, const TOperationOptions&));
        MOCK_METHOD3(RawJoinReduce, IOperationPtr(const TRawJoinReduceOperationSpec&, ::TIntrusivePtr<IRawJob>, const TOperationOptions&));
        MOCK_METHOD5(DoMapReduce, IOperationPtr(const TMapReduceOperationSpec&, const IStructuredJob*, const IStructuredJob*, const IStructuredJob&, const TOperationOptions&));
        MOCK_METHOD5(RawMapReduce, IOperationPtr(const TRawMapReduceOperationSpec&, ::TIntrusivePtr<IRawJob>, ::TIntrusivePtr<IRawJob>, ::TIntrusivePtr<IRawJob>, const TOperationOptions&));
        MOCK_METHOD2(RunVanilla, IOperationPtr(const TVanillaOperationSpec&, const TOperationOptions&));
        MOCK_METHOD1(AttachOperation, IOperationPtr(const TOperationId&));

        MOCK_METHOD1(StartTransaction, ITransactionPtr(const TStartTransactionOptions&));
        MOCK_METHOD2(AlterTable, void(const TYPath&, const TAlterTableOptions&));

        MOCK_CONST_METHOD0(GetId, const TTransactionId&());

        MOCK_METHOD3(Lock, ILockPtr(const TYPath& path, ELockMode mode, const TLockOptions& options));
        MOCK_METHOD2(Unlock, void(const TYPath& path, const TUnlockOptions& options));

        MOCK_METHOD0(Commit, void());
        MOCK_METHOD0(Abort, void());
        MOCK_METHOD0(Ping, void());

        MOCK_METHOD0(GetParentClient, IClientPtr());
    };

    class TLockMock : public ILock {
    public:
        MOCK_CONST_METHOD0(GetId, const TLockId&());
        MOCK_CONST_METHOD0(GetAcquiredFuture, const NThreading::TFuture<void>&());
    };

    class TOperationMock : public IOperation {
        MOCK_CONST_METHOD0(GetId, const TOperationId&());
    };

} // namespace NTesting
} // namespace NYT
