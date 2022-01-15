#include "journal_manager.h"
#include "bootstrap.h"
#include "private.h"
#include "chunk_store.h"
#include "config.h"
#include "journal_chunk.h"
#include "journal_dispatcher.h"
#include "location.h"

#include <yt/yt/server/node/cluster_node/config.h>

#include <yt/yt/server/lib/hydra_common/changelog.h>
#include <yt/yt/server/lib/hydra_common/lazy_changelog.h>
#include <yt/yt/server/lib/hydra_common/file_helpers.h>
#include <yt/yt/server/lib/hydra_common/private.h>

#include <yt/yt/server/lib/hydra/file_changelog_dispatcher.h>
#include <yt/yt/server/lib/hydra/private.h>

#include <yt/yt/ytlib/hydra/proto/hydra_manager.pb.h>
#include <yt/yt/client/hydra/version.h>

#include <yt/yt/core/concurrency/periodic_executor.h>
#include <yt/yt/core/concurrency/thread_affinity.h>

#include <yt/yt/core/misc/enum.h>
#include <yt/yt/core/misc/fs.h>

#include <yt/yt/core/profiling/timing.h>

namespace NYT::NDataNode {

using namespace NHydra;
using namespace NHydra::NProto;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto MultiplexedCleanupPeriod = TDuration::Seconds(10);
static const auto BarrierCleanupPeriod = TDuration::Seconds(3);

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EMultiplexedRecordType,
    (Create) // create chunk
    (Append) // append record to chunk
    (Remove) // remove chunk
    (Skip)   // record was skipped
);

#pragma pack(push, 4)

struct TMultiplexedRecordHeader
{
    //! Type of the record.
    EMultiplexedRecordType Type;

    // Record id within the chunk (for |Append| only).
    i32 RecordId;

    //! Id of chunk this record is about.
    TChunkId ChunkId;

    //! For |Append| type the data follows.
};

static_assert(sizeof(TMultiplexedRecordHeader) == 24, "Binary size of TMultiplexedRecordHeader has changed.");

#pragma pack(pop)

struct TMultiplexedRecord
{
    TMultiplexedRecordHeader Header;
    TSharedRef Data;
};

} // namespace NYT::NDataNode

Y_DECLARE_PODTYPE(NYT::NDataNode::TMultiplexedRecordHeader);

namespace NYT::NDataNode {

////////////////////////////////////////////////////////////////////////////////

namespace {

TString ChopExtension(TString* fileName)
{
    auto extension = NFS::GetFileExtension(*fileName);
    *fileName = NFS::GetFileNameWithoutExtension(*fileName);
    return extension;
}

int ParseChangelogId(const TString& str, const TString& fileName)
{
    try {
        return FromString<int>(str);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing multiplexed changelog id %Qv",
            fileName);
    }
}

struct TMultiplexedChangelogDescriptor
{
    int Id;
    bool Clean;
};

struct IMultiplexedReplayerCallbacks
{
    virtual ~IMultiplexedReplayerCallbacks() = default;

    virtual std::vector<TMultiplexedChangelogDescriptor> ListMultiplexedChangelogs() = 0;
    virtual IChangelogPtr OpenMultiplexedChangelog(int id) = 0;
    virtual void MarkMultiplexedChangelogClean(int id) = 0;

    virtual IChangelogPtr CreateSplitChangelog(TChunkId chunkId) = 0;
    virtual IChangelogPtr OpenSplitChangelog(TChunkId chunkId) = 0;
    virtual void FlushSplitChangelog(TChunkId chunkId) = 0;
    virtual bool RemoveSplitChangelog(TChunkId chunkId) = 0;
    virtual bool IsSplitChangelogSealed(TChunkId chunkId) = 0;
};

class TMultiplexedReplayer
    : public TRefCounted
{
public:
    TMultiplexedReplayer(
        TMultiplexedChangelogConfigPtr config,
        IMultiplexedReplayerCallbacks* callbacks,
        const NLogging::TLogger& logger)
        : Config_(config)
        , Callbacks_(callbacks)
        , Logger(logger)
    { }

    int ReplayChangelogs()
    {
        auto descriptors = Callbacks_->ListMultiplexedChangelogs();
        std::sort(
            descriptors.begin(),
            descriptors.end(),
            [] (const TMultiplexedChangelogDescriptor& lhs, const TMultiplexedChangelogDescriptor& rhs) {
                return lhs.Id < rhs.Id;
            });

        int minDirtyId = std::numeric_limits<int>::max();
        int maxDirtyId = std::numeric_limits<int>::min();
        int maxCleanId = std::numeric_limits<int>::min();
        for (const auto& descriptor : descriptors) {
            int id = descriptor.Id;
            if (descriptor.Clean) {
                YT_LOG_INFO("Found clean multiplexed changelog (ChangelogId: %v)", id);
                maxCleanId = std::max(maxCleanId, id);
            } else {
                YT_LOG_INFO("Found dirty multiplexed changelog (ChangelogId: %v)", id);
                minDirtyId = std::min(minDirtyId, id);
                maxDirtyId = std::max(maxDirtyId, id);
            }
        }

        for (const auto& descriptor : descriptors) {
            if (descriptor.Clean && descriptor.Id > minDirtyId) {
                YT_LOG_FATAL("Found unexpected clean multiplexed changelog (ChangelogId: %v)",
                    descriptor.Id);
            }
        }

        for (int id = minDirtyId; id <= maxDirtyId; ++id) {
            AnalyzeChangelog(id);
        }

        DumpAnalysisResults();

        for (int id = minDirtyId; id <= maxDirtyId; ++id) {
            ReplayChangelog(id);
        }

        FlushSplitChangelogs();

        if (maxDirtyId >= 0) {
            return maxDirtyId + 1;
        }

        if (maxCleanId >= 0) {
            return maxCleanId + 1;
        }

        return 0;
    }

private:
    const TMultiplexedChangelogConfigPtr Config_;
    IMultiplexedReplayerCallbacks* const Callbacks_;
    const NLogging::TLogger Logger;

    THashSet<TChunkId> CreateChunkIds_;
    THashSet<TChunkId> RemoveChunkIds_;
    THashSet<TChunkId> AppendChunkIds_;
    THashMap<TChunkId, TVersion> ChunkIdToFirstRelevantVersion_;

    struct TSplitEntry
    {
        TSplitEntry(TChunkId chunkId, IChangelogPtr changelog)
            : ChunkId(chunkId)
            , Changelog(changelog)
        { }

        TChunkId ChunkId;
        IChangelogPtr Changelog;

        int RecordsAdded = 0;

        bool SealedChecked = false;
        bool SkipRecordSeen = false;
        bool AppendSealedLogged = false;
        bool AppendSkipLogged = false;
        bool AppendLogged = false;
    };

    THashMap<TChunkId, TSplitEntry> SplitMap_;


    TVersion GetFirstRelevantVersion(TChunkId chunkId)
    {
        return GetOrCrash(ChunkIdToFirstRelevantVersion_, chunkId);
    }

    void ScanChangelog(
        int changelogId,
        const std::function<void(TVersion version, const TMultiplexedRecord& record)>& handler)
    {
        int startRecordId = 0;
        auto multiplexedChangelog = Callbacks_->OpenMultiplexedChangelog(changelogId);
        int recordCount = multiplexedChangelog->GetRecordCount();
        while (startRecordId < recordCount) {
            auto asyncRecordsData = multiplexedChangelog->Read(
                startRecordId,
                recordCount,
                Config_->ReplayBufferSize);
            auto recordsData = WaitFor(asyncRecordsData)
                .ValueOrThrow();

            int currentRecordId = startRecordId;
            for (const auto& recordData : recordsData) {
                YT_VERIFY(recordData.Size() >= sizeof (TMultiplexedRecordHeader));
                TMultiplexedRecord record;
                record.Header = *reinterpret_cast<const TMultiplexedRecordHeader*>(recordData.Begin());
                record.Data = recordData.Slice(sizeof (TMultiplexedRecordHeader), recordData.Size());
                handler(TVersion(changelogId, currentRecordId), record);
                ++currentRecordId;
            }

            startRecordId += recordsData.size();
        }
        WaitFor(multiplexedChangelog->Close())
            .ThrowOnError();
    }

    void AnalyzeChangelog(int changelogId)
    {
        YT_LOG_INFO("Analyzing dirty multiplexed changelog (ChangelogId: %v)", changelogId);

        ScanChangelog(changelogId, [&] (TVersion version, const TMultiplexedRecord& record) {
            auto chunkId = record.Header.ChunkId;
            switch (record.Header.Type) {
                case EMultiplexedRecordType::Skip:
                case EMultiplexedRecordType::Append: {
                    YT_VERIFY(RemoveChunkIds_.find(chunkId) == RemoveChunkIds_.end());
                    auto it = ChunkIdToFirstRelevantVersion_.find(chunkId);
                    if (it == ChunkIdToFirstRelevantVersion_.end()) {
                        YT_VERIFY(ChunkIdToFirstRelevantVersion_.emplace(chunkId, version).second);
                    }
                    AppendChunkIds_.insert(chunkId);
                    break;
                }

                case EMultiplexedRecordType::Create:
                    YT_VERIFY(AppendChunkIds_.find(chunkId) == AppendChunkIds_.end());
                    YT_VERIFY(CreateChunkIds_.find(chunkId) == CreateChunkIds_.end());
                    CreateChunkIds_.insert(chunkId);
                    RemoveChunkIds_.erase(chunkId);
                    ChunkIdToFirstRelevantVersion_[chunkId] = version;
                    break;

                case EMultiplexedRecordType::Remove:
                    // NB: RemoveChunkIds_ may already contain chunkId.
                    // Indeed, for non-multiplexed chunks we still insert a removal record into
                    // the multiplexed changelog. These records are not interleaved with create records.
                    RemoveChunkIds_.insert(chunkId);
                    CreateChunkIds_.erase(chunkId);
                    AppendChunkIds_.erase(chunkId);
                    ChunkIdToFirstRelevantVersion_[chunkId] = version;
                    break;

                default:
                    YT_ABORT();
            }
        });
    }

    void DumpAnalysisResults()
    {
        auto dumpChunkIds = [&] (const THashSet<TChunkId>& chunkIds, const TString& action) {
            for (auto chunkId : chunkIds) {
                YT_LOG_INFO("Replay may %v journal chunk (ChunkId: %v, FirstRelevantVersion: %v)",
                    action,
                    chunkId,
                    GetFirstRelevantVersion(chunkId));
            }
        };

        dumpChunkIds(CreateChunkIds_, "create");
        dumpChunkIds(AppendChunkIds_, "append to");
        dumpChunkIds(RemoveChunkIds_, "remove");
    }

    void FlushSplitChangelogs()
    {
        for (const auto& [chunkId, entry] : SplitMap_) {
            Callbacks_->FlushSplitChangelog(entry.ChunkId);
        }
    }

    void ReplayChangelog(int changelogId)
    {
        YT_LOG_INFO("Replaying dirty multiplexed changelog (ChangelogId: %v)", changelogId);

        ScanChangelog(changelogId, [&] (TVersion version, const TMultiplexedRecord& record) {
            auto chunkId = record.Header.ChunkId;
            if (version < GetFirstRelevantVersion(chunkId))
                return;

            switch (record.Header.Type) {
                case EMultiplexedRecordType::Skip:
                case EMultiplexedRecordType::Append:
                    ReplayAppendRecord(record);
                    break;

                case EMultiplexedRecordType::Create:
                    ReplayCreateRecord(record);
                    break;

                case EMultiplexedRecordType::Remove:
                    ReplayRemoveRecord(record);
                    break;

                default:
                    YT_ABORT();
            }
        });

        for (auto& [chunkId, entry] : SplitMap_) {
            if (entry.RecordsAdded == 0)
                continue;

            WaitFor(entry.Changelog->Flush())
                .ThrowOnError();

            YT_LOG_INFO("Replay appended to journal chunk (ChunkId: %v, RecordCount: %v, RecordsAdded: %v)",
                chunkId,
                entry.Changelog->GetRecordCount(),
                entry.RecordsAdded);

            entry.RecordsAdded = 0;
        }

        Callbacks_->MarkMultiplexedChangelogClean(changelogId);
    }

    void ReplayAppendRecord(const TMultiplexedRecord& record)
    {
        auto chunkId = record.Header.ChunkId;

        auto it = SplitMap_.find(chunkId);
        if (it == SplitMap_.end()) {
            auto changelog = Callbacks_->OpenSplitChangelog(chunkId);
            if (!changelog) {
                YT_LOG_FATAL("Journal chunk %v is missing but has relevant records in the multiplexed changelog",
                    chunkId);
            }
            it = SplitMap_.emplace(
                chunkId,
                TSplitEntry(chunkId, changelog)).first;
        }

        auto& splitEntry = it->second;

        if (splitEntry.AppendSealedLogged) {
            return;
        }

        if (splitEntry.SkipRecordSeen) {
            return;
        }

        if (!splitEntry.SealedChecked) {
            splitEntry.SealedChecked = true;
            if (Callbacks_->IsSplitChangelogSealed(chunkId)) {
                YT_LOG_INFO("Replay ignores sealed journal chunk; "
                    "further similar messages suppressed (ChunkId: %v)",
                    chunkId);
                splitEntry.AppendSealedLogged = true;
                return;
            }
        }

        int recordCount = splitEntry.Changelog->GetRecordCount();
        if (recordCount > record.Header.RecordId) {
            if (!splitEntry.AppendSkipLogged) {
                YT_LOG_INFO("Replay skips multiplexed records that are present in journal chunk; "
                    "further similar messages suppressed (ChunkId: %v, RecordId: %v, RecordCount: %v)",
                    chunkId,
                    record.Header.RecordId,
                    recordCount);
                splitEntry.AppendSkipLogged = true;
            }
            return;
        }

        if (recordCount != record.Header.RecordId) {
            YT_LOG_FATAL("Journal chunk %v has %v records while multiplexed changelog has relevant records starting from %v",
                record.Header.ChunkId,
                recordCount,
                record.Header.RecordId);
        }

        if (record.Header.Type == EMultiplexedRecordType::Skip) {
            YT_LOG_INFO("Replay encountered skip record; multiplexed suffix is ignored (ChunkId: %v, RecordId: %v, RecordCount: %v)",
                chunkId,
                record.Header.RecordId,
                recordCount);
            splitEntry.SkipRecordSeen = true;
            return;
        }

        if (!splitEntry.AppendLogged) {
            YT_LOG_INFO(
                "Replay appends record to journal chunk; further similar messages suppressed "
                "(ChunkId: %v, RecordId: %v)",
                chunkId,
                record.Header.RecordId);
            splitEntry.AppendLogged = true;
        }

        splitEntry.Changelog->Append({record.Data});
        ++splitEntry.RecordsAdded;
    }

    void ReplayCreateRecord(const TMultiplexedRecord& record)
    {
        auto chunkId = record.Header.ChunkId;

        auto changelog = Callbacks_->CreateSplitChangelog(chunkId);
        if (!changelog) {
            YT_LOG_INFO("Journal chunk creation skipped since the chunk already exists (ChunkId: %v)",
                chunkId);
            return;
        }

        YT_VERIFY(SplitMap_.emplace(
            chunkId,
            TSplitEntry(chunkId, changelog)).second);

        YT_LOG_INFO("Replay created journal chunk (ChunkId: %v)",
            chunkId);
    }

    void ReplayRemoveRecord(const TMultiplexedRecord& record)
    {
        auto chunkId = record.Header.ChunkId;

        YT_VERIFY(SplitMap_.find(chunkId) == SplitMap_.end());

        if (!Callbacks_->RemoveSplitChangelog(chunkId))
            return;

        YT_LOG_INFO("Replay removed journal chunk (ChunkId: %v)",
            chunkId);
    }

};

class TMultiplexedWriter
    : public TRefCounted
{
public:
    TMultiplexedWriter(
        TMultiplexedChangelogConfigPtr config,
        IFileChangelogDispatcherPtr multiplexedChangelogDispatcher,
        TString path,
        IInvokerPtr invoker,
        NLogging::TLogger logger)
        : Config_(std::move(config))
        , MultiplexedChangelogDispatcher_(std::move(multiplexedChangelogDispatcher))
        , Path_(std::move(path))
        , Invoker_(std::move(invoker))
        , Logger(std::move(logger))
    { }

    void Initialize(int changelogId)
    {
        auto changelog = CreateMultiplexedChangelog(changelogId);
        SetMultiplexedChangelog(changelog, changelogId);

        MultiplexedCleanupExecutor_ = New<TPeriodicExecutor>(
            Invoker_,
            BIND(&TMultiplexedWriter::OnMultiplexedCleanup, MakeWeak(this)),
            MultiplexedCleanupPeriod);
        MultiplexedCleanupExecutor_->Start();

        BarrierCleanupExecutor_ = New<TPeriodicExecutor>(
            Invoker_,
            BIND(&TMultiplexedWriter::OnBarrierCleanup, MakeWeak(this)),
            BarrierCleanupPeriod);
        BarrierCleanupExecutor_->Start();
    }

    TFuture<void> WriteCreateRecord(TChunkId chunkId)
    {
        TMultiplexedRecord record;
        record.Header.Type = EMultiplexedRecordType::Create;
        record.Header.ChunkId = chunkId;
        record.Header.RecordId = -1;
        return WriteMultiplexedRecords({record});
    }

    TFuture<void> WriteRemoveRecord(TChunkId chunkId)
    {
        TMultiplexedRecord record;
        record.Header.Type = EMultiplexedRecordType::Remove;
        record.Header.ChunkId = chunkId;
        record.Header.RecordId = -1;
        return WriteMultiplexedRecords({record});
    }

    TFuture<bool> WriteAppendRecords(
        TChunkId chunkId,
        int firstRecordId,
        TRange<TSharedRef> records)
    {
        std::vector<TMultiplexedRecord> multiplexedRecords;
        multiplexedRecords.reserve(records.size());
        auto currentRecordId = firstRecordId;

        bool recordsSkipped = false;
        for (const auto& record : records) {
            TMultiplexedRecord multiplexedRecord;
            multiplexedRecord.Header.RecordId = currentRecordId++;
            multiplexedRecord.Header.ChunkId = chunkId;

            if (!Config_->BigRecordThreshold || static_cast<int>(record.Size()) <= *Config_->BigRecordThreshold) {
                multiplexedRecord.Header.Type = EMultiplexedRecordType::Append;
                multiplexedRecord.Data = record;
            } else {
                multiplexedRecord.Header.Type = EMultiplexedRecordType::Skip;
                recordsSkipped = true;
            }

            multiplexedRecords.push_back(multiplexedRecord);
        }

        return WriteMultiplexedRecords(multiplexedRecords)
            .Apply(BIND([recordsSkipped] {
                return recordsSkipped;
            }));
    }

    TPromise<void> RegisterBarrier()
    {
        auto barrier = NewPromise<void>();
        auto guard = Guard(SpinLock_);
        YT_VERIFY(Barriers_.insert(barrier).second);
        return barrier;
    }

    std::vector<TMultiplexedChangelogDescriptor> ListMultiplexedChangelogs()
    {
        NFS::MakeDirRecursive(Path_);
        auto fileNames = NFS::EnumerateFiles(Path_);
        std::vector<TMultiplexedChangelogDescriptor> result;
        for (const auto& originalFileName : fileNames) {
            auto fileName = originalFileName;
            auto extension = ChopExtension(&fileName);
            if (extension == CleanExtension) {
                extension = ChopExtension(&fileName);
                if (extension == ChangelogExtension) {
                    int id = ParseChangelogId(fileName, originalFileName);
                    result.push_back(TMultiplexedChangelogDescriptor{id, true});
                }
            } else if (extension == ChangelogExtension) {
                int id = ParseChangelogId(fileName, originalFileName);
                result.push_back(TMultiplexedChangelogDescriptor{id, false});
            }
        }
        return result;
    }

    IChangelogPtr OpenMultiplexedChangelog(int changelogId)
    {
        // NB: May be called multiple times for the same #changelogId.
        MultiplexedChangelogIdToCleanResult_.emplace(changelogId, NewPromise<void>());
        auto path = GetMultiplexedChangelogPath(changelogId);
        return WaitFor(MultiplexedChangelogDispatcher_->OpenChangelog(path, Config_))
            .ValueOrThrow();
    }

    void MarkMultiplexedChangelogClean(int changelogId)
    {
        YT_LOG_INFO("Multiplexed changelog will be marked as clean (ChangelogId: %v)", changelogId);

        auto curResult = GetOrCrash(MultiplexedChangelogIdToCleanResult_, changelogId);

        auto prevResultIt = MultiplexedChangelogIdToCleanResult_.find(changelogId - 1);
        auto prevResult = prevResultIt == MultiplexedChangelogIdToCleanResult_.end()
            ? VoidFuture
            : prevResultIt->second.ToFuture();

        auto delayedResult = TDelayedExecutor::MakeDelayed(Config_->CleanDelay);

        auto combinedResult = AllSucceeded(std::vector<TFuture<void>>{prevResult, delayedResult});
        curResult.SetFrom(combinedResult.Apply(
            BIND(&TMultiplexedWriter::DoMarkMultiplexedChangelogClean, MakeStrong(this), changelogId)
                .Via(Invoker_)));
    }

private:
    const TMultiplexedChangelogConfigPtr Config_;
    const IFileChangelogDispatcherPtr MultiplexedChangelogDispatcher_;
    const TString Path_;
    const IInvokerPtr Invoker_;

    const NLogging::TLogger Logger;

    //! Protects a section of members.
    YT_DECLARE_SPIN_LOCK(NThreading::TSpinLock, SpinLock_);

    //! The current multiplexed changelog.
    IChangelogPtr MultiplexedChangelog_;

    //! The moment when the multiplexed changelog was last rotated.
    NProfiling::TCpuInstant MultiplexedChangelogRotationDeadline_;

    //! The id of #MultiplexedChangelog_.
    int MultiplexedChangelogId_;

    //! A collection of futures for various activities recorded in the current multiplexed changelog.
    //! One must wait for these futures to become set before marking the changelog as clean.
    THashSet<TFuture<void>> Barriers_;

    //! Maps multiplexed changelog ids to cleanup results.
    //! Used to guarantee that multiplexed changelogs are being marked as clean in proper order.
    THashMap<int, TPromise<void>> MultiplexedChangelogIdToCleanResult_;

    TPeriodicExecutorPtr MultiplexedCleanupExecutor_;
    TPeriodicExecutorPtr BarrierCleanupExecutor_;


    TFuture<void> WriteMultiplexedRecords(TRange<TMultiplexedRecord> multiplexedRecords)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        // Construct the multiplexed data record and append it.
        struct TMultiplexedRecordTag { };

        i64 totalSize = 0;
        for (const auto& multiplexedRecord : multiplexedRecords) {
            totalSize += sizeof (TMultiplexedRecordHeader);
            totalSize += multiplexedRecord.Data.Size();
        }

        std::vector<TSharedRef> changelogRecords;
        changelogRecords.reserve(multiplexedRecords.size());

        auto multiplexedData = TSharedMutableRef::Allocate<TMultiplexedRecordTag>(totalSize, false);
        char* currentDataPtr = multiplexedData.Begin();

        for (const auto& multiplexedRecord : multiplexedRecords) {
            char* changelogRecordPtr = currentDataPtr;

            std::copy(
                reinterpret_cast<const char*>(&multiplexedRecord.Header),
                reinterpret_cast<const char*>(&multiplexedRecord.Header) + sizeof (TMultiplexedRecordHeader),
                currentDataPtr);
            currentDataPtr += sizeof (TMultiplexedRecordHeader);

            std::copy(
                multiplexedRecord.Data.Begin(),
                multiplexedRecord.Data.End(),
                currentDataPtr);
            currentDataPtr += multiplexedRecord.Data.Size();

            changelogRecords.push_back(multiplexedData.Slice(changelogRecordPtr, currentDataPtr));
        }

        auto guard = Guard(SpinLock_);

        auto appendResult = MultiplexedChangelog_->Append(changelogRecords);

        // Check if it is time to rotate.
        if (MultiplexedChangelog_->GetRecordCount() >= Config_->MaxRecordCount ||
            MultiplexedChangelog_->GetDataSize() >= Config_->MaxDataSize ||
            NProfiling::GetCpuInstant() > MultiplexedChangelogRotationDeadline_)
        {
            YT_LOG_INFO("Started rotating multiplexed changelog (ChangelogId: %v)",
                MultiplexedChangelogId_);

            auto multiplexedFlushResult = MultiplexedChangelog_->Flush();

            // To mark a multiplexed changelog as clean we wait for
            // * the multiplexed changelog to get flushed
            // * all outstanding barriers to become set
            std::vector<TFuture<void>> barriers(Barriers_.begin(), Barriers_.end());
            barriers.push_back(multiplexedFlushResult);
            Barriers_.clear();

            auto combinedBarrier = AllSucceeded(barriers);

            int oldId = MultiplexedChangelogId_;
            int newId = MultiplexedChangelogId_ + 1;

            auto futureMultiplexedChangelog =
                BIND(
                    &TMultiplexedWriter::CreateNewMultiplexedChangelog,
                    MakeStrong(this),
                    multiplexedFlushResult,
                    oldId,
                    newId)
                .AsyncVia(MultiplexedChangelogDispatcher_->GetInvoker())
                .Run();

            BIND(
                &TMultiplexedWriter::WaitAndMarkMultplexedChangelogClean,
                MakeStrong(this),
                combinedBarrier,
                oldId)
            .AsyncVia(MultiplexedChangelogDispatcher_->GetInvoker())
            .Run();

            SetMultiplexedChangelog(CreateLazyChangelog(futureMultiplexedChangelog), newId);
        }

        return appendResult;
    }

    void SetMultiplexedChangelog(IChangelogPtr changelog, int id)
    {
        MultiplexedChangelog_ = changelog;
        MultiplexedChangelogId_ = id;
        MultiplexedChangelogRotationDeadline_ =
            NProfiling::GetCpuInstant() +
            NProfiling::DurationToCpuDuration(Config_->AutoRotationPeriod);
    }

    IChangelogPtr CreateMultiplexedChangelog(int id)
    {
        YT_LOG_INFO("Started creating new multiplexed changelog (ChangelogId: %v)",
            id);

        auto changelog = WaitFor(MultiplexedChangelogDispatcher_->CreateChangelog(
            GetMultiplexedChangelogPath(id),
            Config_))
            .ValueOrThrow();

        YT_LOG_INFO("Finished creating new multiplexed changelog (ChangelogId: %v)",
            id);

        YT_VERIFY(MultiplexedChangelogIdToCleanResult_.emplace(id, NewPromise<void>()).second);

        return changelog;
    }

    IChangelogPtr CreateNewMultiplexedChangelog(
        TFuture<void> flushResult,
        int oldId,
        int newId)
    {
        auto flushError = WaitFor(flushResult);
        if (!flushError.IsOK()) {
            YT_LOG_FATAL(flushError, "Error flushing multiplexed changelog");
        }

        auto changelog = CreateMultiplexedChangelog(newId);

        YT_LOG_INFO("Finished rotating multiplexed changelog (ChangelogId: %v)",
            oldId);

        return changelog;
    }

    void WaitAndMarkMultplexedChangelogClean(
        TFuture<void> combinedBarrier,
        int id)
    {
        YT_LOG_INFO("Waiting for multiplexed changelog to become clean (ChangelogId: %v)", id);

        auto error = WaitFor(combinedBarrier);
        if (!error.IsOK()) {
            YT_LOG_FATAL(error, "Error waiting for multiplexed changelog barrier");
        }

        MarkMultiplexedChangelogClean(id);
    }

    void OnMultiplexedCleanup()
    {
        try {
            auto fileNames = NFS::EnumerateFiles(Path_);

            std::vector<int> ids;
            for (const auto& originalFileName : fileNames) {
                auto fileName = originalFileName;
                auto cleanExtension = ChopExtension(&fileName);
                if (cleanExtension != CleanExtension)
                    continue;

                auto changelogExtension = ChopExtension(&fileName);
                if (changelogExtension != ChangelogExtension)
                    continue;

                int id = ParseChangelogId(fileName, originalFileName);
                ids.push_back(id);
            }

            if (std::ssize(ids) <= Config_->MaxCleanChangelogsToKeep)
                return;

            std::sort(ids.begin(), ids.end());
            ids.erase(ids.end() - Config_->MaxCleanChangelogsToKeep, ids.end());

            for (int id : ids) {
                YT_LOG_INFO("Removing clean multiplexed changelog (ChangelogId: %v)", id);

                auto fileName = GetMultiplexedChangelogPath(id) + "." + CleanExtension;
                RemoveChangelogFiles(fileName);
            }
        } catch (const std::exception& ex) {
            YT_LOG_ERROR(ex, "Error cleaning up multiplexed changelogs");
        }
    }

    TString GetMultiplexedChangelogPath(int changelogId)
    {
        return NFS::CombinePaths(
            Path_,
            Format("%09d.%v", changelogId, ChangelogExtension));
    }

    void OnBarrierCleanup()
    {
        auto guard = Guard(SpinLock_);

        std::vector<TFuture<void>> activeBarriers;
        activeBarriers.reserve(Barriers_.size());

        for (const auto& barrier : Barriers_) {
            if (!barrier.IsSet()) {
                activeBarriers.push_back(barrier);
            }
        }

        Barriers_ = THashSet<TFuture<void>>(activeBarriers.begin(), activeBarriers.end());
    }

    void DoMarkMultiplexedChangelogClean(int changelogId)
    {
        try {
            auto dataFileName = GetMultiplexedChangelogPath(changelogId);
            auto cleanDataFileName = dataFileName + "." + CleanExtension;
            NFS::Rename(dataFileName, cleanDataFileName);
            NFS::Rename(dataFileName + "." + ChangelogIndexExtension, cleanDataFileName + "." + ChangelogIndexExtension);
            YT_LOG_INFO("Multiplexed changelog is marked as clean (ChangelogId: %v)", changelogId);
        } catch (const std::exception& ex) {
            YT_LOG_FATAL(ex, "Error marking multiplexed changelog as clean (ChangelogId: %v)", changelogId);
        }
    }

};

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TJournalManager::TImpl
    : public TRefCounted
{
public:
    TImpl(
        TDataNodeConfigPtr config,
        TStoreLocation* location,
        TChunkContextPtr chunkContext)
        : MultiplexedChangelogConfig_(UpdateYsonSerializable(config->MultiplexedChangelog, location->GetConfig()->MultiplexedChangelog))
        , HighLatencySplitChangelogConfig_(UpdateYsonSerializable(config->HighLatencySplitChangelog, location->GetConfig()->HighLatencySplitChangelog))
        , LowLatencySplitChangelogConfig_(UpdateYsonSerializable(config->LowLatencySplitChangelog, location->GetConfig()->LowLatencySplitChangelog))
        , Location_(location)
        , ChunkContext_(chunkContext)
        , Logger(DataNodeLogger.WithTag("LocationId: %v", Location_->GetId()))
    {
        MultiplexedChangelogDispatcher_ = CreateFileChangelogDispatcher(
            Location_->GetIOEngine(),
            MultiplexedChangelogConfig_,
            "MFlush:" + Location_->GetId(),
            Location_->GetProfiler().WithPrefix("/multiplexed_changelogs"));

        SplitChangelogDispatcher_ = CreateFileChangelogDispatcher(
            Location_->GetIOEngine(),
            MultiplexedChangelogConfig_,
            "SFlush:" + Location_->GetId(),
            Location_->GetProfiler().WithPrefix("/split_changelogs"));

        MultiplexedWriter_ = New<TMultiplexedWriter>(
            MultiplexedChangelogConfig_,
            MultiplexedChangelogDispatcher_,
            NFS::CombinePaths(Location_->GetPath(), MultiplexedDirectory),
            MultiplexedChangelogDispatcher_->GetInvoker(),
            Logger);
    }

    void Initialize()
    {
        YT_LOG_INFO("Initializing journals");

        // Initialize and replay multiplexed changelogs.
        TMultiplexedReplayCallbacks replayCallbacks(this);
        auto replayer = New<TMultiplexedReplayer>(
            MultiplexedChangelogConfig_,
            &replayCallbacks,
            Logger);
        int newId = replayer->ReplayChangelogs();

        // Create new multiplexed changelog.
        MultiplexedWriter_->Initialize(newId);

        YT_LOG_INFO("Journals initialized");
    }

    TFuture<IChangelogPtr> OpenChangelog(TChunkId chunkId)
    {
        return BIND(Location_->DisableOnError(BIND(&TImpl::DoOpenChangelog, MakeStrong(this), chunkId)))
            .AsyncVia(SplitChangelogDispatcher_->GetInvoker())
            .Run()
            .ToUncancelable();
    }

    TFuture<IChangelogPtr> CreateChangelog(
        TChunkId chunkId,
        bool enableMultiplexing,
        const TWorkloadDescriptor& workloadDescriptor)
    {
        auto creator = BIND(Location_->DisableOnError(
            BIND(
                &TImpl::DoCreateChangelog,
                MakeStrong(this),
                chunkId,
                enableMultiplexing,
                workloadDescriptor)))
            .AsyncVia(SplitChangelogDispatcher_->GetInvoker());

        TFuture<IChangelogPtr> asyncChangelog;
        if (enableMultiplexing) {
            auto barrier = MultiplexedWriter_->RegisterBarrier();
            asyncChangelog = MultiplexedWriter_->WriteCreateRecord(chunkId)
                .Apply(creator)
                .Apply(BIND([=] (const TErrorOr<IChangelogPtr>& result) mutable {
                    barrier.Set(result.IsOK() ? TError() : TError(result));
                    return result.ValueOrThrow();
                }));
        } else {
            asyncChangelog = creator.Run();
        }
        return asyncChangelog.ToUncancelable();
    }

    TFuture<void> RemoveChangelog(
        const TJournalChunkPtr& chunk,
        bool enableMultiplexing)
    {
        auto remover = BIND(Location_->DisableOnError(
            BIND(
                &TImpl::DoRemoveChangelog,
                MakeStrong(this),
                chunk)))
            .AsyncVia(SplitChangelogDispatcher_->GetInvoker());

        TFuture<void> asyncResult;
        if (enableMultiplexing) {
            auto barrier = MultiplexedWriter_->RegisterBarrier();
            asyncResult = MultiplexedWriter_->WriteRemoveRecord(chunk->GetId())
                .Apply(remover)
                .Apply(BIND([=] (const TError& result) mutable {
                    barrier.Set(result);
                    result.ThrowOnError();
                }));
        } else {
            asyncResult = remover.Run();
        }
        return asyncResult.ToUncancelable();
    }

    TFuture<bool> AppendMultiplexedRecords(
        TChunkId chunkId,
        int firstRecordId,
        TRange<TSharedRef> records,
        TFuture<void> splitResult)
    {
        auto barrier = MultiplexedWriter_->RegisterBarrier();
        barrier.SetFrom(splitResult);
        return MultiplexedWriter_->WriteAppendRecords(chunkId, firstRecordId, records);
    }

    TFuture<bool> IsChangelogSealed(TChunkId chunkId)
    {
        return BIND(Location_->DisableOnError(BIND(&TImpl::DoIsChangelogSealed, MakeStrong(this), chunkId)))
            .AsyncVia(SplitChangelogDispatcher_->GetInvoker())
            .Run();
    }

    TFuture<void> SealChangelog(const TJournalChunkPtr& chunk)
    {
        return BIND(Location_->DisableOnError(BIND(&TImpl::DoSealChangelog, MakeStrong(this), chunk)))
            .AsyncVia(SplitChangelogDispatcher_->GetInvoker())
            .Run()
            .ToUncancelable();
    }

private:
    const TMultiplexedChangelogConfigPtr MultiplexedChangelogConfig_;
    const TFileChangelogConfigPtr HighLatencySplitChangelogConfig_;
    const TFileChangelogConfigPtr LowLatencySplitChangelogConfig_;
    TStoreLocation* const Location_;
    TChunkContextPtr ChunkContext_;

    const NLogging::TLogger Logger;

    IFileChangelogDispatcherPtr MultiplexedChangelogDispatcher_;
    IFileChangelogDispatcherPtr SplitChangelogDispatcher_;

    TIntrusivePtr<TMultiplexedWriter> MultiplexedWriter_;


    TFileChangelogConfigPtr GetSplitChangelogConfig(bool enableMultiplexing)
    {
        return enableMultiplexing
            ? HighLatencySplitChangelogConfig_
            : LowLatencySplitChangelogConfig_;
    }

    IChangelogPtr DoCreateChangelog(
        TChunkId chunkId,
        bool enableMultiplexing,
        const TWorkloadDescriptor& /*workloadDescriptor*/)
    {
        IChangelogPtr changelog;

        YT_LOG_DEBUG("Started creating journal chunk (ChunkId: %v)",
            chunkId);

        {
            NProfiling::TEventTimerGuard timingGuard(Location_->GetPerformanceCounters().JournalChunkCreateTime);
            auto fileName = Location_->GetChunkPath(chunkId);
            changelog = WaitFor(SplitChangelogDispatcher_->CreateChangelog(
                fileName,
                GetSplitChangelogConfig(enableMultiplexing)))
                .ValueOrThrow();
        }

        YT_LOG_DEBUG("Finished creating journal chunk (ChunkId: %v)",
            chunkId);

        return changelog;
    }

    IChangelogPtr DoOpenChangelog(TChunkId chunkId)
    {
        IChangelogPtr changelog;

        YT_LOG_DEBUG("Started opening journal chunk (ChunkId: %v)",
            chunkId);

        {
            NProfiling::TEventTimerGuard timingGuard(Location_->GetPerformanceCounters().JournalChunkOpenTime);
            auto fileName = Location_->GetChunkPath(chunkId);
            changelog = WaitFor(SplitChangelogDispatcher_->OpenChangelog(fileName, HighLatencySplitChangelogConfig_))
                .ValueOrThrow();
        }

        YT_LOG_DEBUG("Finished opening journal chunk (ChunkId: %v)",
            chunkId);

        return changelog;
    }

    void DoRemoveChangelog(const TJournalChunkPtr& chunk)
    {
        NProfiling::TEventTimerGuard guard(Location_->GetPerformanceCounters().JournalChunkRemoveTime);
        chunk->SyncRemove(false);
    }

    bool DoIsChangelogSealed(TChunkId chunkId)
    {
        return NFS::Exists(GetSealedFlagFileName(chunkId));
    }

    void DoSealChangelog(const TJournalChunkPtr& chunk)
    {
        TFile file(GetSealedFlagFileName(chunk->GetId()), CreateNew);
    }

    TString GetSealedFlagFileName(TChunkId chunkId)
    {
        return Location_->GetChunkPath(chunkId) + "." + SealedFlagExtension;
    }


    class TMultiplexedReplayCallbacks
        : public IMultiplexedReplayerCallbacks
    {
    public:
        explicit TMultiplexedReplayCallbacks(TImpl* impl)
            : Impl_(impl)
        { }

        // IMultiplexedReplayerCallbacks
        std::vector<TMultiplexedChangelogDescriptor> ListMultiplexedChangelogs() override
        {
            return Impl_->MultiplexedWriter_->ListMultiplexedChangelogs();
        }

        IChangelogPtr OpenMultiplexedChangelog(int id) override
        {
            return Impl_->MultiplexedWriter_->OpenMultiplexedChangelog(id);
        }

        void MarkMultiplexedChangelogClean(int id) override
        {
            Impl_->MultiplexedWriter_->MarkMultiplexedChangelogClean(id);
        }

        IChangelogPtr CreateSplitChangelog(TChunkId chunkId) override
        {
            const auto& chunkStore = Impl_->Location_->GetChunkStore();
            if (chunkStore->FindChunk(chunkId)) {
                return nullptr;
            }

            auto chunk = New<TJournalChunk>(
                Impl_->ChunkContext_,
                Impl_->Location_,
                TChunkDescriptor(chunkId));

            const auto& dispatcher = Impl_->ChunkContext_->JournalDispatcher;
            auto asyncChangelog = dispatcher->CreateChangelog(
                chunk->GetStoreLocation(),
                chunkId,
                false,
                TWorkloadDescriptor(EWorkloadCategory::SystemRepair));
            auto changelog = WaitFor(asyncChangelog)
                .ValueOrThrow();

            YT_VERIFY(IdToChangelog_.emplace(chunkId, changelog).second);
            chunkStore->RegisterNewChunk(chunk, /*session*/ nullptr);

            return changelog;
        }

        IChangelogPtr OpenSplitChangelog(TChunkId chunkId) override
        {
            const auto& chunkStore = Impl_->Location_->GetChunkStore();
            auto chunk = chunkStore->FindChunk(chunkId);
            if (!chunk) {
                return nullptr;
            }

            const auto& dispatcher = Impl_->ChunkContext_->JournalDispatcher;
            auto journalChunk = chunk->AsJournalChunk();
            auto changelog = WaitFor(dispatcher->OpenChangelog(journalChunk->GetStoreLocation(), chunkId))
                .ValueOrThrow();

            YT_VERIFY(IdToChangelog_.emplace(chunkId, changelog).second);

            return changelog;
        }

        void FlushSplitChangelog(TChunkId chunkId) override
        {
            const auto& chunkStore = Impl_->Location_->GetChunkStore();
            auto chunk = chunkStore->FindChunk(chunkId);
            if (!chunk) {
                return;
            }

            auto changelog = GetChangelogById(chunkId);
            WaitFor(changelog->Flush())
                .ThrowOnError();

            auto journalChunk = chunk->AsJournalChunk();
            journalChunk->UpdateFlushedRowCount(changelog->GetRecordCount());
            journalChunk->UpdateDataSize(changelog->GetDataSize());
        }

        bool RemoveSplitChangelog(TChunkId chunkId) override
        {
            const auto& chunkStore = Impl_->Location_->GetChunkStore();
            auto chunk = chunkStore->FindChunk(chunkId);
            if (!chunk) {
                return false;
            }

            auto journalChunk = chunk->AsJournalChunk();
            chunkStore->UnregisterChunk(chunk);

            const auto& dispatcher = Impl_->ChunkContext_->JournalDispatcher;
            WaitFor(dispatcher->RemoveChangelog(journalChunk, false))
                .ThrowOnError();

            return true;
        }

        bool IsSplitChangelogSealed(TChunkId chunkId) override
        {
             return WaitFor(Impl_->IsChangelogSealed(chunkId))
                 .ValueOrThrow();
        }

    private:
        TImpl* const Impl_;

        THashMap<TChunkId, IChangelogPtr> IdToChangelog_;

        IChangelogPtr GetChangelogById(TChunkId chunkId)
        {
            auto it = IdToChangelog_.find(chunkId);
            YT_VERIFY(it != IdToChangelog_.end());
            return it->second;
        }
    };
};

////////////////////////////////////////////////////////////////////////////////

TJournalManager::TJournalManager(
    TDataNodeConfigPtr config,
    TStoreLocation* location,
    TChunkContextPtr chunkContext)
    : Impl_(New<TImpl>(config, location, chunkContext))
{ }

TJournalManager::~TJournalManager() = default;

void TJournalManager::Initialize()
{
    Impl_->Initialize();
}

TFuture<IChangelogPtr> TJournalManager::OpenChangelog(
    TChunkId chunkId)
{
    return Impl_->OpenChangelog(chunkId);
}

TFuture<IChangelogPtr> TJournalManager::CreateChangelog(
    TChunkId chunkId,
    bool enableMultiplexing,
    const TWorkloadDescriptor& workloadDescriptor)
{
    return Impl_->CreateChangelog(chunkId, enableMultiplexing, workloadDescriptor);
}

TFuture<void> TJournalManager::RemoveChangelog(
    const TJournalChunkPtr& chunk,
    bool enableMultiplexing)
{
    return Impl_->RemoveChangelog(chunk, enableMultiplexing);
}

TFuture<bool> TJournalManager::AppendMultiplexedRecords(
    TChunkId chunkId,
    int firstRecordId,
    TRange<TSharedRef> records,
    TFuture<void> splitResult)
{
    return Impl_->AppendMultiplexedRecords(
        chunkId,
        firstRecordId,
        records,
        std::move(splitResult));
}

TFuture<bool> TJournalManager::IsChangelogSealed(TChunkId chunkId)
{
    return Impl_->IsChangelogSealed(chunkId);
}

TFuture<void> TJournalManager::SealChangelog(const TJournalChunkPtr& chunk)
{
    return Impl_->SealChangelog(chunk);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDataNode
