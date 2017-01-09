#pragma once

#include "private.h"
#include "dynamic_store_bits.h"
#include "sorted_dynamic_comparer.h"
#include "store_detail.h"
#include "transaction.h"

#include <yt/ytlib/chunk_client/chunk_meta.pb.h>

#include <yt/ytlib/table_client/row_buffer.h>
#include <yt/ytlib/table_client/versioned_row.h>

#include <yt/ytlib/transaction_client/public.h>

#include <yt/core/actions/signal.h>

#include <yt/core/misc/chunked_vector.h>
#include <yt/core/misc/property.h>

#include <yt/core/concurrency/rw_spinlock.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TRowBlockedException
    : public std::exception
{
public:
    TRowBlockedException(
        TSortedDynamicStorePtr store,
        TSortedDynamicRow row,
        ui32 lockMask,
        TTimestamp timestamp)
        : Store_(std::move(store))
        , Row_(row)
        , LockMask_(lockMask)
        , Timestamp_(timestamp)
    { }

    DEFINE_BYVAL_RO_PROPERTY(TSortedDynamicStorePtr, Store);
    DEFINE_BYVAL_RO_PROPERTY(TSortedDynamicRow, Row);
    DEFINE_BYVAL_RO_PROPERTY(ui32, LockMask);
    DEFINE_BYVAL_RO_PROPERTY(TTimestamp, Timestamp);

};

////////////////////////////////////////////////////////////////////////////////

class TSortedDynamicStore
    : public TDynamicStoreBase
    , public TSortedStoreBase
{
public:
    TSortedDynamicStore(
        TTabletManagerConfigPtr config,
        const TStoreId& id,
        TTablet* tablet);
    virtual ~TSortedDynamicStore();


    //! Returns the reader to be used during flush.
    NTableClient::IVersionedReaderPtr CreateFlushReader();

    //! Returns the reader to be used during store serialization.
    NTableClient::IVersionedReaderPtr CreateSnapshotReader();


    //! Returns the cached instance of row key comparer
    //! (obtained by calling TTablet::GetRowKeyComparer).
    const TSortedDynamicRowKeyComparer& GetRowKeyComparer() const;

    using TRowBlockedHandler = TCallback<void(TSortedDynamicRow row, int lockIndex)>;

    //! Sets the handler that is being invoked when read request faces a blocked row.
    void SetRowBlockedHandler(TRowBlockedHandler handler);

    //! Clears the blocked row handler.
    void ResetRowBlockedHandler();

    //! Checks if a given #row has any locks from #lockMask with prepared timestamp
    //! less that #timestamp. If so, raises |RowBlocked| signal and loops.
    void WaitOnBlockedRow(
        TSortedDynamicRow row,
        ui32 lockMask,
        TTimestamp timestamp);

    //! Writes the row.
    /*!
     *  If #commitTimestamp is not null then no locks are checked or taken.
     *  #transaction could be null.
     *  The row is committed immediately.
     *
     *  If #commitTimstamp is null then checks and takes the locks.
     *  #transaction cannot be null.
     *  On lock failure, throws TErrorException explaining the cause.
     *  If a blocked row is encountered, throws TRowBlockedException.
     */
    TSortedDynamicRow WriteRow(
        TTransaction* transaction,
        NTableClient::TUnversionedRow row,
        TTimestamp commitTimestamp,
        ui32 lockMask);

    //! Deletes the row.
    /*!
     *  \see WriteRow
     */
    TSortedDynamicRow DeleteRow(
        TTransaction* transaction,
        TKey key,
        TTimestamp commitTimestamp);

    TSortedDynamicRow MigrateRow(TTransaction* transaction, TSortedDynamicRow row);
    void PrepareRow(TTransaction* transaction, TSortedDynamicRow row);
    void CommitRow(TTransaction* transaction, TSortedDynamicRow row);
    void AbortRow(TTransaction* transaction, TSortedDynamicRow row);

    // The following functions are made public for unit-testing.
    TSortedDynamicRow FindRow(NTableClient::TUnversionedRow key);
    std::vector<TSortedDynamicRow> GetAllRows();
    Y_FORCE_INLINE TTimestamp TimestampFromRevision(ui32 revision) const;
    TTimestamp GetLastCommitTimestamp(TSortedDynamicRow row, int lockIndex);

    // IStore implementation.
    virtual EStoreType GetType() const override;

    virtual i64 GetRowCount() const override;

    // ISortedStore implementation.
    virtual TOwningKey GetMinKey() const override;
    virtual TOwningKey GetMaxKey() const override;

    virtual NTableClient::IVersionedReaderPtr CreateReader(
        const TTabletSnapshotPtr& tabletSnapshot,
        TSharedRange<NTableClient::TRowRange> bounds,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter,
        const TWorkloadDescriptor& workloadDescriptor) override;

    virtual NTableClient::IVersionedReaderPtr CreateReader(
        const TTabletSnapshotPtr& tabletSnapshot,
        const TSharedRange<TKey>& keys,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter,
        const TWorkloadDescriptor& workloadDescriptor) override;

    virtual void CheckRowLocks(
        TUnversionedRow row,
        TTransaction* transaction,
        ui32 lockMask) override;

    virtual void Save(TSaveContext& context) const override;
    virtual void Load(TLoadContext& context) override;

    virtual TCallback<void(TSaveContext&)> AsyncSave() override;
    virtual void AsyncLoad(TLoadContext& context) override;

    virtual TSortedDynamicStorePtr AsSortedDynamic() override;

private:
    class TReaderBase;
    class TRangeReader;
    class TLookupReader;
    class TLookupHashTable;

    const TSortedDynamicRowKeyComparer RowKeyComparer_;
    const std::unique_ptr<TSkipList<TSortedDynamicRow, TSortedDynamicRowKeyComparer>> Rows_;
	std::unique_ptr<TLookupHashTable> LookupHashTable_;

    ui32 FlushRevision_ = InvalidRevision;

    static const size_t RevisionsPerChunk = 1ULL << 13;
    static const size_t MaxRevisionChunks = HardRevisionsPerDynamicStoreLimit / RevisionsPerChunk + 1;
    TChunkedVector<TTimestamp, RevisionsPerChunk> RevisionToTimestamp_;

    NConcurrency::TReaderWriterSpinLock RowBlockedLock_;
    TRowBlockedHandler RowBlockedHandler_;


    virtual void OnSetPassive() override;

    TSortedDynamicRow AllocateRow();

    TRowBlockedHandler GetRowBlockedHandler();
    int GetBlockingLockIndex(
        TSortedDynamicRow row,
        ui32 lockMask,
        TTimestamp timestamp);
    void ValidateRowNotBlocked(
        TSortedDynamicRow row,
        ui32 lockMask,
        TTimestamp timestamp);

    void CheckRowLocks(
        TSortedDynamicRow row,
        TTransaction* transaction,
        ui32 lockMask);
    void AcquireRowLocks(
        TSortedDynamicRow row,
        TTransaction* transaction,
        ui32 lockMask,
        bool deleteFlag);

    TValueList PrepareFixedValue(TSortedDynamicRow row, int index);
    void AddDeleteRevision(TSortedDynamicRow row, ui32 revision);
    void AddWriteRevision(TLockDescriptor& lock, ui32 revision);
    void SetKeys(TSortedDynamicRow dstRow, const TUnversionedValue* srcKeys);
    void SetKeys(TSortedDynamicRow dstRow, TSortedDynamicRow srcRow);

    struct TLoadScratchData
    {
        yhash_map<TTimestamp, ui32> TimestampToRevision;
        std::vector<std::vector<ui32>> WriteRevisions;
    };

    void LoadRow(TVersionedRow row, TLoadScratchData* scratchData);
    ui32 CaptureTimestamp(TTimestamp timestamp, TLoadScratchData* scratchData);
    ui32 CaptureVersionedValue(TDynamicValue* dst, const TVersionedValue& src, TLoadScratchData* scratchData);

    void CaptureUncommittedValue(TDynamicValue* dst, const TDynamicValue& src, int index);
    void CaptureUnversionedValue(TDynamicValue* dst, const TUnversionedValue& src);
    TDynamicValueData CaptureStringValue(TDynamicValueData src);
    TDynamicValueData CaptureStringValue(const TUnversionedValue& src);

    ui32 GetLatestRevision() const;
    ui32 RegisterRevision(TTimestamp timestamp);

    void OnMemoryUsageUpdated();

    void InsertIntoLookupHashTable(const TUnversionedValue* keyBegin, TSortedDynamicRow dynamicRow);
};

DEFINE_REFCOUNTED_TYPE(TSortedDynamicStore)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT

#define SORTED_DYNAMIC_STORE_INL_H_
#include "sorted_dynamic_store-inl.h"
#undef SORTED_DYNAMIC_STORE_INL_H_

