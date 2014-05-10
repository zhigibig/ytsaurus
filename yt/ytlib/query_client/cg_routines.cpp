#include "stdafx.h"
#include "cg_routines.h"
#include "cg_routine_registry.h"

#include "helpers.h"
#include "callbacks.h"

#include <ytlib/new_table_client/unversioned_row.h>
#include <ytlib/new_table_client/schemaful_reader.h>
#include <ytlib/new_table_client/schemaful_writer.h>
#include <ytlib/new_table_client/schemaful_merging_reader.h>
#include <ytlib/new_table_client/row_buffer.h>

#include <ytlib/chunk_client/chunk_spec.h>

#include <core/concurrency/scheduler.h>

#include <core/profiling/scoped_timer.h>

#include <mutex>

namespace NYT {
namespace NQueryClient {
namespace NRoutines {

using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const size_t InitialGroupOpHashtableCapacity = 1024;

#ifdef DEBUG
#define CHECK_STACK() \
    { \
        int dummy; \
        size_t currentStackSize = P->StackSizeGuardHelper - reinterpret_cast<intptr_t>(&dummy); \
        YCHECK(currentStackSize < 10000); \
    }
#else
#define CHECK_STACK() (void) 0;
#endif

////////////////////////////////////////////////////////////////////////////////

void WriteRow(TRow row, TPassedFragmentParams* P)
{
    CHECK_STACK()

    --P->RowLimit;
    ++P->Statistics->RowsWritten;

    auto* batch = P->Batch;
    auto* writer = P->Writer;
    auto* rowBuffer = P->RowBuffer;

    YASSERT(batch->size() < batch->capacity());

    batch->push_back(rowBuffer->Capture(row));

    if (batch->size() == batch->capacity()) {
        if (!writer->Write(*batch)) {
            NProfiling::TAggregatingTimingGuard timingGuard(&P->Statistics->AsyncTime);
            auto error = WaitFor(writer->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(error);
        }
        batch->clear();
        rowBuffer->Clear();
    }
}

void ScanOpHelper(
    TPassedFragmentParams* P,
    int dataSplitsIndex,
    void** consumeRowsClosure,
    void (*consumeRows)(void** closure, TRow* rows, int size))
{
    auto* callbacks = P->Callbacks;
    auto* context = P->Context;
    auto dataSplits = (*P->DataSplitsArray)[dataSplitsIndex];

    std::vector<ISchemafulReaderPtr> splitReaders;
    TTableSchema schema;
    for (const auto& dataSplit : dataSplits) {
        if (splitReaders.empty()) {
            // All schemas are expected to be same; take the first one. 
            schema = GetTableSchemaFromDataSplit(dataSplit);
        }
        splitReaders.push_back(callbacks->GetReader(dataSplit, context));
    }

    auto mergingReader = CreateSchemafulMergingReader(splitReaders);

    {
        auto error = WaitFor(mergingReader->Open(schema));
        THROW_ERROR_EXCEPTION_IF_FAILED(error);
    }

    std::vector<TRow> rows;
    rows.reserve(MaxRowsPerRead);

    while (true) {
        P->ScratchSpace->Clear();

        bool hasMoreData = mergingReader->Read(&rows);
        bool shouldWait = rows.empty();

        P->Statistics->RowsRead += rows.size();

        i64 rowsLeft = rows.size();
        auto* currentRow = rows.data();

        while (rowsLeft > 0 && P->RowLimit > 0) {
            size_t consumeSize = std::min(P->RowLimit, rowsLeft);
            consumeRows(consumeRowsClosure, currentRow, consumeSize);
            currentRow += consumeSize;
            rowsLeft -= consumeSize;
        }

        rows.clear();

        if (!hasMoreData) {
            break;
        }

        if (P->RowLimit <= 0) {
            P->Statistics->Incomplete = true;
            break;
        }

        if (shouldWait) {
            NProfiling::TAggregatingTimingGuard timingGuard(&P->Statistics->AsyncTime);
            auto error = WaitFor(mergingReader->GetReadyEvent());
            THROW_ERROR_EXCEPTION_IF_FAILED(error);
        }
    }
}

void GroupOpHelper(
    int keySize,
    int aggregateItemCount,
    void** consumeRowsClosure,
    void (*consumeRows)(
        void** closure,
        std::vector<TRow>* groupedRows,
        TLookupRows* rows))
{
    std::vector<TRow> groupedRows;
    TLookupRows lookupRows(
        InitialGroupOpHashtableCapacity,
        NDetail::TGroupHasher(keySize),
        NDetail::TGroupComparer(keySize));

    consumeRows(consumeRowsClosure, &groupedRows, &lookupRows);
}

const TRow* FindRow(TPassedFragmentParams* P, TLookupRows* rows, TRow row)
{
    CHECK_STACK()

    auto it = rows->find(row);
    return it != rows->end()? &*it : nullptr;
}

void AddRow(
    TPassedFragmentParams* P,
    TLookupRows* lookupRows,
    std::vector<TRow>* groupedRows,
    TRow* newRow,
    int valueCount)
{
    CHECK_STACK()

    --P->RowLimit;

    groupedRows->push_back(P->RowBuffer->Capture(*newRow));
    lookupRows->insert(groupedRows->back());
    *newRow = TRow::Allocate(P->ScratchSpace, valueCount);
}

void AllocateRow(TPassedFragmentParams* P, int valueCount, TRow* row)
{
    CHECK_STACK()

    *row = TRow::Allocate(P->ScratchSpace, valueCount);
}

TRow* GetRowsData(std::vector<TRow>* groupedRows)
{
    return groupedRows->data();
}

int GetRowsSize(std::vector<TRow>* groupedRows)
{
    return groupedRows->size();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NRoutines

////////////////////////////////////////////////////////////////////////////////

void RegisterCGRoutinesImpl()
{
#define REGISTER_ROUTINE(routine) \
    TRoutineRegistry::RegisterRoutine(#routine, NRoutines::routine)
    REGISTER_ROUTINE(WriteRow);
    REGISTER_ROUTINE(ScanOpHelper);
    REGISTER_ROUTINE(GroupOpHelper);
    REGISTER_ROUTINE(FindRow);
    REGISTER_ROUTINE(AddRow);
    REGISTER_ROUTINE(AllocateRow);
    REGISTER_ROUTINE(GetRowsData);
    REGISTER_ROUTINE(GetRowsSize);
#undef REGISTER_ROUTINE
}

void RegisterCGRoutines()
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, &RegisterCGRoutinesImpl);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

