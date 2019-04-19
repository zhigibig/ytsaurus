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

    struct TSetOptions;

    struct TListOptions;

    struct TCopyOptions;

    struct TMoveOptions;

    struct TLinkOptions;

    struct TConcatenateOptions;

    struct TInsertRowsOptions;

    struct TDeleteRowsOptions;

    struct TTrimRowsOptions;

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

    enum class EFormatType : int;

    struct TFormat;

    class IFileReader;

    using IFileReaderPtr = ::TIntrusivePtr<IFileReader>;

    class IFileWriter;

    using IFileWriterPtr = ::TIntrusivePtr<IFileWriter>;

    class IBlobTableReader;
    using IBlobTableReaderPtr = ::TIntrusivePtr<IBlobTableReader>;

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

    enum class EJobState : int;
    enum class EJobType : int;

    class TJobStatistics;

    template <typename T>
    class TJobStatisticsEntry;

    ////////////////////////////////////////////////////////////////////////////////
    // operation.h
    ////////////////////////////////////////////////////////////////////////////////

    class TFormatHints;

    struct TUserJobSpec;

    struct TMapOperationSpec;

    struct TRawMapOperationSpec;

    struct TReduceOperationSpec;

    struct TMapReduceOperationSpec;

    struct TJoinReduceOperationSpec;

    struct TSortOperationSpec;

    class IISchemaInferenceContext;

    class TSchemaInferenceResultBuilder;

    class IJob;

    class IRawJob;

    enum EMergeMode : int;

    struct TMergeOperationSpec;

    struct TEraseOperationSpec;

    template <class TR, class TW>
    class IMapper;

    template <class TR, class TW>
    class IReducer;

    template <class TR, class TW>
    class IAggregatorReducer;

    enum class EOperationBriefState : int;

    struct TOperationAttributes;

    struct TOperationOptions;

    enum class EOperationAttribute : int;

    struct TOperationAttributeFilter;

    struct TGetOperationOptions;

    struct TListOperationsOptions;

    struct TGetJobOptions;

    struct TListJobsOptions;

    struct IOperationClient;

    enum class EFinishedJobState : int;

    enum class EJobType : int;

    struct TJobBinaryDefault;

    struct TJobBinaryLocalPath;

    struct TJobBinaryCypressPath;

    using TJobBinaryConfig = ::TVariant<
        TJobBinaryDefault,
        TJobBinaryLocalPath,
        TJobBinaryCypressPath>;

    class IRequestRetryPolicy;
    using IRequestRetryPolicyPtr = ::TIntrusivePtr<IRequestRetryPolicy>;
}
