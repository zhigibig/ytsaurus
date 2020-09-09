#pragma once

#include "public.h"

#include <yt/server/lib/election/config.h>

#include <yt/client/api/config.h>

#include <yt/ytlib/chunk_client/config.h>

#include <yt/core/compression/public.h>

#include <yt/core/misc/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT::NHydra {

////////////////////////////////////////////////////////////////////////////////

class TFileChangelogConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Minimum total index records size between consecutive index records.
    i64 IndexBlockSize;

    //! When the number of unflushed bytes exceeds this value, an automatic flush is performed.
    i64 FlushBufferSize;

    //! Interval between consequent automatic flushes.
    TDuration FlushPeriod;

    //! When |false|, no |fdatasync| calls are actually made.
    //! Should only be used in tests and local mode.
    bool EnableSync;

    // TODO(savrus): implement this
    std::optional<i64> PreallocateSize;

    TFileChangelogConfig()
    {
        RegisterParameter("index_block_size", IndexBlockSize)
            .GreaterThan(0)
            .Default(1_MB);
        RegisterParameter("flush_buffer_size", FlushBufferSize)
            .GreaterThanOrEqual(0)
            .Default(16_MB);
        RegisterParameter("flush_period", FlushPeriod)
            .Default(TDuration::MilliSeconds(10));
        RegisterParameter("enable_sync", EnableSync)
            .Default(true);
        RegisterParameter("preallocate_size", PreallocateSize)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(TFileChangelogConfig)

class TFileChangelogDispatcherConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    int IOClass;
    int IOPriority;
    TDuration FlushQuantum;

    TFileChangelogDispatcherConfig()
    {
        RegisterParameter("io_class", IOClass)
            .Default(1); // IOPRIO_CLASS_RT
        RegisterParameter("io_priority", IOPriority)
            .Default(3);
        RegisterParameter("flush_quantum", FlushQuantum)
            .Default(TDuration::MilliSeconds(10));
    }
};

DEFINE_REFCOUNTED_TYPE(TFileChangelogDispatcherConfig)

class TFileChangelogStoreConfig
    : public TFileChangelogConfig
    , public TFileChangelogDispatcherConfig
{
public:
    //! A path where changelogs are stored.
    TString Path;

    //! Maximum number of cached changelogs.
    TSlruCacheConfigPtr ChangelogReaderCache;

    NChunkClient::EIOEngineType IOEngineType;
    NYTree::INodePtr IOConfig;

    TFileChangelogStoreConfig()
    {
        RegisterParameter("path", Path);
        RegisterParameter("changelog_reader_cache", ChangelogReaderCache)
            .DefaultNew();

        RegisterParameter("io_engine_type", IOEngineType)
            .Default(NChunkClient::EIOEngineType::ThreadPool);
        RegisterParameter("io_engine", IOConfig)
            .Optional();

        RegisterPreprocessor([&] () {
           ChangelogReaderCache->Capacity = 4;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TFileChangelogStoreConfig)

////////////////////////////////////////////////////////////////////////////////

class TLocalSnapshotStoreConfig
    : public NYTree::TYsonSerializable
{
public:
    //! A path where snapshots are stored.
    TString Path;

    //! Codec used to write snapshots.
    NCompression::ECodec Codec;

    TLocalSnapshotStoreConfig()
    {
        RegisterParameter("path", Path);
        RegisterParameter("codec", Codec)
            .Default(NCompression::ECodec::Lz4);
    }
};

DEFINE_REFCOUNTED_TYPE(TLocalSnapshotStoreConfig)

////////////////////////////////////////////////////////////////////////////////

class TRemoteSnapshotStoreConfig
    : public NYTree::TYsonSerializable
{
public:
    NApi::TFileReaderConfigPtr Reader;
    NApi::TFileWriterConfigPtr Writer;

    TRemoteSnapshotStoreConfig()
    {
        RegisterParameter("reader", Reader)
            .DefaultNew();
        RegisterParameter("writer", Writer)
            .DefaultNew();

        RegisterPreprocessor([&] {
            Reader->WorkloadDescriptor.Category = EWorkloadCategory::SystemTabletRecovery;
            Writer->WorkloadDescriptor.Category = EWorkloadCategory::SystemTabletSnapshot;

            //! We want to evenly distribute snapshot load across the cluster.
            Writer->PreferLocalHost = false;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TRemoteSnapshotStoreConfig)

////////////////////////////////////////////////////////////////////////////////

class TRemoteChangelogStoreConfig
    : public NYTree::TYsonSerializable
{
public:
    NApi::TJournalReaderConfigPtr Reader;
    NApi::TJournalWriterConfigPtr Writer;

    TRemoteChangelogStoreConfig()
    {
        RegisterParameter("reader", Reader)
            .DefaultNew();
        RegisterParameter("writer", Writer)
            .DefaultNew();

        RegisterPreprocessor([&] {
            Reader->WorkloadDescriptor.Category = EWorkloadCategory::SystemTabletRecovery;

            Writer->WorkloadDescriptor.Category = EWorkloadCategory::SystemTabletLogging;
            Writer->MaxChunkRowCount = 1'000'000'000;
            Writer->MaxChunkDataSize = 1_TB;
            Writer->MaxChunkSessionDuration = TDuration::Hours(24);
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TRemoteChangelogStoreConfig)

////////////////////////////////////////////////////////////////////////////////

class THydraJanitorConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    std::optional<int> MaxSnapshotCountToKeep;
    std::optional<i64> MaxSnapshotSizeToKeep;
    std::optional<int> MaxChangelogCountToKeep;
    std::optional<i64> MaxChangelogSizeToKeep;

    THydraJanitorConfig()
    {
        RegisterParameter("max_snapshot_count_to_keep", MaxSnapshotCountToKeep)
            .GreaterThanOrEqual(0)
            .Default(10);
        RegisterParameter("max_snapshot_size_to_keep", MaxSnapshotSizeToKeep)
            .GreaterThanOrEqual(0)
            .Default();
        RegisterParameter("max_changelog_count_to_keep", MaxChangelogCountToKeep)
            .GreaterThanOrEqual(0)
            .Default();
        RegisterParameter("max_changelog_size_to_keep", MaxChangelogSizeToKeep)
            .GreaterThanOrEqual(0)
            .Default();
    }
};

DEFINE_REFCOUNTED_TYPE(THydraJanitorConfig)

////////////////////////////////////////////////////////////////////////////////

class TLocalHydraJanitorConfig
    : public THydraJanitorConfig
{
public:
    TDuration CleanupPeriod;

    TLocalHydraJanitorConfig()
    {
        RegisterParameter("cleanup_period", CleanupPeriod)
            .Default(TDuration::Seconds(10));
    }
};

DEFINE_REFCOUNTED_TYPE(TLocalHydraJanitorConfig)

////////////////////////////////////////////////////////////////////////////////

class TDistributedHydraManagerConfig
    : public virtual NYTree::TYsonSerializable
{
public:
    //! Timeout for various control RPC requests.
    TDuration ControlRpcTimeout;

    //! The maximum time interval mutations are allowed to occupy the automaton thread
    //! before yielding control to other callbacks.
    TDuration MaxCommitBatchDuration;

    //! Interval between consequent lease lease checks.
    TDuration LeaderLeaseCheckPeriod;

    //! Timeout after which leader lease expires.
    TDuration LeaderLeaseTimeout;

    //! Time a newly elected leader waits before becoming active.
    TDuration LeaderLeaseGraceDelay;

    //! When set to |true|, disables leader grace delay.
    //! For tests only!
    bool DisableLeaderLeaseGraceDelay;

    //! Leader-to-follower commit timeout.
    TDuration CommitFlushRpcTimeout;

    //! Follower-to-leader commit forwarding timeout.
    TDuration CommitForwardingRpcTimeout;

    //! Backoff time for unrecoverable errors causing restart.
    TDuration RestartBackoffTime;

    //! Maximum time allotted to construct a snapshot.
    TDuration SnapshotBuildTimeout;

    //! Maximum time interval between consequent snapshots.
    TDuration SnapshotBuildPeriod;

    //! Random splay for snapshot building.
    TDuration SnapshotBuildSplay;

    //! Generic timeout for RPC calls during changelog download.
    TDuration ChangelogDownloadRpcTimeout;

    //! Maximum number of bytes to read from a changelog at once.
    i64 MaxChangelogBytesPerRequest;

    //! Maximum number of records to read from a changelog at once.
    int MaxChangelogRecordsPerRequest;

    //! Generic timeout for RPC calls during snapshot download.
    TDuration SnapshotDownloadRpcTimeout;

    //! Block size used during snapshot download.
    i64 SnapshotDownloadBlockSize;

    //! Maximum time to wait before flushing the current batch.
    TDuration MaxCommitBatchDelay;

    //! Maximum number of records to collect before flushing the current batch.
    int MaxCommitBatchRecordCount;

    //! Maximum time to wait before syncing with leader.
    TDuration LeaderSyncDelay;

    //! Changelog record count limit.
    /*!
     *  When this limit is reached, the current changelog is rotated and a snapshot
     *  is built.
     */
    int MaxChangelogRecordCount;

    //! Changelog data size limit, in bytes.
    /*!
     *  See #MaxChangelogRecordCount.
     */
    i64 MaxChangelogDataSize;

    //! If true, empty changelogs are preallocated to avoid hiccups of segment rotation.
    bool PreallocateChangelogs;

    //! Interval between automatic "heartbeat" mutations commit.
    /*!
     *  These mutations are no-ops. Committing them regularly helps to ensure
     *  that the quorum is functioning properly and is also crucial to enable
     *  snapshot rotation as no version rotation is possible at N:0 versions.
     */
    TDuration HeartbeatMutationPeriod;

    //! If "heartbeat" mutation commit takes longer than this value, Hydra is restarted.
    TDuration HeartbeatMutationTimeout;

    //! Period for retrying while waiting for changelog record count to become
    //! sufficiently high to proceed with applying mutations.
    TDuration ChangelogRecordCountCheckRetryPeriod;

    //! If mutation logging remains suspended for this period of time,
    //! Hydra restarts.
    TDuration MutationLoggingSuspensionTimeout;

    //! Time to sleep before building a snapshot. Needed for testing.
    TDuration BuildSnapshotDelay;

    //! Persistent stores initialization has exponential retries.
    //! Minimum persistent store initializing backoff time.
    TDuration MinPersistentStoreInitializationBackoffTime;

    //! Maximum persistent store initializing backoff time.
    TDuration MaxPersistentStoreInitializationBackoffTime;

    //! Persistent store initializing backoff time multiplier.
    double PersistentStoreInitializationBackoffTimeMultiplier;

    //! Abandon leader lease request timeout.
    TDuration AbandonLeaderLeaseRequestTimeout;

    //! Enables logging in mutation handlers even during recovery.
    bool ForceMutationLogging;

    TDistributedHydraManagerConfig()
    {
        RegisterParameter("control_rpc_timeout", ControlRpcTimeout)
            .Default(TDuration::Seconds(5));

        RegisterParameter("max_commit_batch_duration", MaxCommitBatchDuration)
            .Default(TDuration::MilliSeconds(100));
        RegisterParameter("leader_lease_check_period", LeaderLeaseCheckPeriod)
            .Default(TDuration::Seconds(2));
        RegisterParameter("leader_lease_timeout", LeaderLeaseTimeout)
            .Default(TDuration::Seconds(5));
        RegisterParameter("leader_lease_grace_delay", LeaderLeaseGraceDelay)
            .Default(TDuration::Seconds(6));
        RegisterParameter("disable_leader_lease_grace_delay", DisableLeaderLeaseGraceDelay)
            .Default(false);

        RegisterParameter("commit_flush_rpc_timeout", CommitFlushRpcTimeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("commit_forwarding_rpc_timeout", CommitForwardingRpcTimeout)
            .Default(TDuration::Seconds(30));

        RegisterParameter("restart_backoff_time", RestartBackoffTime)
            .Default(TDuration::Seconds(5));

        RegisterParameter("snapshot_build_timeout", SnapshotBuildTimeout)
            .Default(TDuration::Minutes(5));
        RegisterParameter("snapshot_build_period", SnapshotBuildPeriod)
            .Default(TDuration::Minutes(60));
        RegisterParameter("snapshot_build_splay", SnapshotBuildSplay)
            .Default(TDuration::Minutes(5));

        RegisterParameter("changelog_download_rpc_timeout", ChangelogDownloadRpcTimeout)
            .Default(TDuration::Seconds(10));
        RegisterParameter("max_changelog_records_per_request", MaxChangelogRecordsPerRequest)
            .GreaterThan(0)
            .Default(64 * 1024);
        RegisterParameter("max_changelog_bytes_per_request", MaxChangelogBytesPerRequest)
            .GreaterThan(0)
            .Default(128_MB);

        RegisterParameter("snapshot_download_rpc_timeout", SnapshotDownloadRpcTimeout)
            .Default(TDuration::Seconds(10));
        RegisterParameter("snapshot_download_block_size", SnapshotDownloadBlockSize)
            .GreaterThan(0)
            .Default(32_MB);

        RegisterParameter("max_commmit_batch_delay", MaxCommitBatchDelay)
            .Default(TDuration::MilliSeconds(10));
        RegisterParameter("max_commit_batch_record_count", MaxCommitBatchRecordCount)
            .Default(10000);

        RegisterParameter("leader_sync_delay", LeaderSyncDelay)
            .Default(TDuration::MilliSeconds(10));

        RegisterParameter("max_changelog_record_count", MaxChangelogRecordCount)
            .Default(1000000)
            .GreaterThan(0);
        RegisterParameter("max_changelog_data_size", MaxChangelogDataSize)
            .Default(1_GB)
            .GreaterThan(0);
        RegisterParameter("preallocate_changelogs", PreallocateChangelogs)
            .Default(false);

        RegisterParameter("heartbeat_mutation_period", HeartbeatMutationPeriod)
            .Default(TDuration::Seconds(60));
        RegisterParameter("heartbeat_mutation_timeout", HeartbeatMutationTimeout)
            .Default(TDuration::Seconds(60));

        RegisterParameter("changelog_record_count_check_retry_period", ChangelogRecordCountCheckRetryPeriod)
            .Default(TDuration::Seconds(1));

        RegisterParameter("mutation_logging_suspension_timeout", MutationLoggingSuspensionTimeout)
            .Default(TDuration::Seconds(60));

        RegisterParameter("build_snapshot_delay", BuildSnapshotDelay)
            .Default(TDuration::Zero());

        RegisterParameter("min_persistent_store_initialization_backoff_time", MinPersistentStoreInitializationBackoffTime)
            .Default(TDuration::MilliSeconds(200));
        RegisterParameter("max_persistent_store_initialization_backoff_time", MaxPersistentStoreInitializationBackoffTime)
            .Default(TDuration::Seconds(5));
        RegisterParameter("persistent_store_initialization_backoff_time_multiplier", PersistentStoreInitializationBackoffTimeMultiplier)
            .Default(1.5);

        RegisterParameter("abandon_leader_lease_request_timeout", AbandonLeaderLeaseRequestTimeout)
            .Default(TDuration::Seconds(5));

        RegisterParameter("force_mutation_logging", ForceMutationLogging)
            .Default(false);

        RegisterPostprocessor([&] () {
            if (!DisableLeaderLeaseGraceDelay && LeaderLeaseGraceDelay <= LeaderLeaseTimeout) {
                THROW_ERROR_EXCEPTION("\"leader_lease_grace_delay\" must be larger than \"leader_lease_timeout\"");
            }
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TDistributedHydraManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
