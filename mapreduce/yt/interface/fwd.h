#pragma once

#include <util/generic/fwd.h>
#include <util/system/types.h>

namespace google {
    namespace protobuf {
        class Message;
    }
}

namespace NYT {

    ////////////////////////////////////////////////////////////////////////////////
    // batch_request.h
    ////////////////////////////////////////////////////////////////////////////////

    class IBatchRequest;
    using TBatchRequestPtr = ::TIntrusivePtr<IBatchRequest>;

    ////////////////////////////////////////////////////////////////////////////////
    // client.h
    ////////////////////////////////////////////////////////////////////////////////

    enum ELockMode : int;

    struct TStartTransactionOptions;

    struct TLockOptions;

    template <class TDerived>
    struct TTabletOptions;

    struct TMountTableOptions;

    struct TUnmountTableOptions;

    struct TRemountTableOptions;

    struct TReshardTableOptions;

    struct TAlterTableOptions;

    struct TLookupRowsOptions;

    struct TSelectRowsOptions;

    struct TCreateClientOptions;

    struct TAlterTableReplicaOptions;

    class ILock;
    using ILockPtr = ::TIntrusivePtr<ILock>;

    class ITransaction;
    using ITransactionPtr = ::TIntrusivePtr<ITransaction>;

    struct IOperation;
    using IOperationPtr = ::TIntrusivePtr<IOperation>;

    class IClientBase;

    class IClient;

    using IClientPtr = ::TIntrusivePtr<IClient>;
    using IClientBasePtr = ::TIntrusivePtr<IClientBase>;

    ////////////////////////////////////////////////////////////////////////////////
    // cypress.h
    ////////////////////////////////////////////////////////////////////////////////

    enum ENodeType : int;

    struct TCreateOptions;

    struct TRemoveOptions;

    struct TGetOptions;

    struct TListOptions;

    struct TCopyOptions;

    struct TMoveOptions;

    struct TLinkOptions;

    struct TConcatenateOptions;

    struct TInsertRowsOptions;

    struct TDeleteRowsOptions;

    class ICypressClient;

    ////////////////////////////////////////////////////////////////////////////////
    // errors.h
    ////////////////////////////////////////////////////////////////////////////////

    class TApiUsageError;

    class TYtError;

    class TErrorResponse;

    struct TFailedJobInfo;

    class TOperationFailedError;

    ////////////////////////////////////////////////////////////////////////////////
    // node.h
    ////////////////////////////////////////////////////////////////////////////////

    class TNode;

    ////////////////////////////////////////////////////////////////////////////////
    // common.h
    ////////////////////////////////////////////////////////////////////////////////

    using TTransactionId = TGUID;
    using TNodeId = TGUID;
    using TLockId = TGUID;
    using TOperationId = TGUID;
    using TTabletCellId = TGUID;
    using TReplicaId = TGUID;
    using TJobId = TGUID;

    using TYPath = TString;
    using TLocalFilePath = TString;

    template <class T>
    struct TKeyBase;

    // key column values
    using TKey = TKeyBase<TNode>;

    // key column names
    using TKeyColumns = TKeyBase<TString>;

    enum EValueType : int;

    enum ESortOrder : int;

    enum EOptimizeForAttr : i8;

    enum EErasureCodecAttr : i8;

    struct TColumnSchema;

    struct TTableSchema;

    struct TReadLimit;

    struct TReadRange;

    struct TRichYPath;

    struct TAttributeFilter;

    ////////////////////////////////////////////////////////////////////////////////
    // io.h
    ////////////////////////////////////////////////////////////////////////////////

    class IFileReader;

    using IFileReaderPtr = ::TIntrusivePtr<IFileReader>;

    class IFileWriter;

    using IFileWriterPtr = ::TIntrusivePtr<IFileWriter>;

    class TRawTableReader;

    using TRawTableReaderPtr = ::TIntrusivePtr<TRawTableReader>;

    class TRawTableWriter;

    using TRawTableWriterPtr = ::TIntrusivePtr<TRawTableWriter>;

    template <class T, class = void>
    class TTableReader;

    template <class T, class = void>
    class TTableRangesReader;

    template <class T>
    using TTableReaderPtr = ::TIntrusivePtr<TTableReader<T>>;

    template <class T, class = void>
    class TTableWriter;

    template <class T>
    using TTableWriterPtr = ::TIntrusivePtr<TTableWriter<T>>;

    struct TYaMRRow;

    using ::google::protobuf::Message;

    using TYaMRReader = TTableReader<TYaMRRow>;
    using TYaMRWriter = TTableWriter<TYaMRRow>;
    using TNodeReader = TTableReader<TNode>;
    using TNodeWriter = TTableWriter<TNode>;
    using TMessageReader = TTableReader<Message>;
    using TMessageWriter = TTableWriter<Message>;

    template <class TDerived>
    struct TIOOptions;

    struct TFileReaderOptions;

    struct TFileWriterOptions;

    struct TTableReaderOptions;

    struct TTableWriterOptions;

    ////////////////////////////////////////////////////////////////////////////////
    // job_statistics.h
    ////////////////////////////////////////////////////////////////////////////////

    enum EFinishedJobState : int;
    enum EJobType : int;

    class TJobStatistics;

    template <typename T>
    class TJobStatisticsEntry;

    ////////////////////////////////////////////////////////////////////////////////
    // operation.h
    ////////////////////////////////////////////////////////////////////////////////

    struct TUserJobSpec;

    struct TMapOperationSpec;

    struct TReduceOperationSpec;

    struct TMapReduceOperationSpec;

    struct TJoinReduceOperationSpec;

    struct TSortOperationSpec;

    class IJob;

    enum EMergeMode : int;

    struct TMergeOperationSpec;

    struct TEraseOperationSpec;

    template <class TR, class TW>
    class IMapper;

    template <class TR, class TW>
    class IReducer;

    template <class TR, class TW>
    class IAggregatorReducer;

    enum EOperationStatus : int;

    struct TOperationOptions;

    struct IOperationClient;
}
