#pragma once

#include <yt/ytlib/table_client/public.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/misc/public.h>

namespace NYT {
namespace NApi {

///////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMasterChannelKind,
    (Leader)
    (Follower)
    (Cache)
);

DEFINE_ENUM(EUserWorkloadCategory,
    (Batch)
    (Realtime)
);

DEFINE_ENUM(EErrorCode,
    ((TooManyConcurrentRequests)                         (1800))
);

DEFINE_ENUM(ERowModificationType,
    ((Write) (0))
    ((Delete)(1))
);

///////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IRowset)
DECLARE_REFCOUNTED_STRUCT(IPersistentQueueRowset)

struct TAdminOptions;
struct TClientOptions;
struct TTransactionParticipantOptions;

DECLARE_REFCOUNTED_STRUCT(IConnection)
DECLARE_REFCOUNTED_STRUCT(IAdmin)
DECLARE_REFCOUNTED_STRUCT(IClientBase)
DECLARE_REFCOUNTED_STRUCT(IClient)
DECLARE_REFCOUNTED_STRUCT(ITransaction)

DECLARE_REFCOUNTED_STRUCT(INativeConnection)
DECLARE_REFCOUNTED_STRUCT(INativeClient)
DECLARE_REFCOUNTED_STRUCT(INativeTransaction)

DECLARE_REFCOUNTED_STRUCT(IFileReader)
DECLARE_REFCOUNTED_STRUCT(IFileWriter)

DECLARE_REFCOUNTED_STRUCT(IJournalReader)
DECLARE_REFCOUNTED_STRUCT(IJournalWriter)

DECLARE_REFCOUNTED_CLASS(TPersistentQueuePoller)

DECLARE_REFCOUNTED_CLASS(TConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TMasterConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TNativeConnectionConfig)
DECLARE_REFCOUNTED_CLASS(TFileReaderConfig)
DECLARE_REFCOUNTED_CLASS(TFileWriterConfig)
DECLARE_REFCOUNTED_CLASS(TJournalReaderConfig)
DECLARE_REFCOUNTED_CLASS(TJournalWriterConfig)
DECLARE_REFCOUNTED_CLASS(TPersistentQueuePollerConfig)

DECLARE_REFCOUNTED_STRUCT(TPrerequisiteRevisionConfig)

///////////////////////////////////////////////////////////////////////////////

} // namespace NApi
} // namespace NYT

