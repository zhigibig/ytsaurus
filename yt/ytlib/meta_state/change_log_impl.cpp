#include "change_log_impl.h"

#include <ytlib/misc/fs.h>
#include <ytlib/misc/nullable.h>
#include <ytlib/misc/serialize.h>
#include <ytlib/misc/string.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = MetaStateLogger;
static const char* const IndexSuffix = ".index";

////////////////////////////////////////////////////////////////////////////////

TChangeLog::TImpl::TImpl(
    const Stroka& fileName,
    int id,
    i64 indexBlockSize)
    : Id(id)
    , IndexBlockSize(indexBlockSize)
    , FileName(fileName)
    , IndexFileName(fileName + IndexSuffix)
    , State(EState::Uninitialized)
    , RecordCount(-1)
    , CurrentBlockSize(-1)
    , CurrentFilePosition(-1)
    , PrevRecordCount(-1)
    , Logger(MetaStateLogger)
{
    Logger.AddTag(Sprintf("ChangeLogId: %d", Id));
}

////////////////////////////////////////////////////////////////////////////////

void TChangeLog::TImpl::Append(const std::vector<TSharedRef>& records)
{
    YCHECK(State == EState::Open);

    LOG_DEBUG("Appending %" PRISZT " records to changelog", records.size());

    TGuard<TMutex> guard(Mutex);
    FOREACH (const auto& record, records) {
        Append(record);
    }
}

void TChangeLog::TImpl::Append(int firstRecordId, const std::vector<TSharedRef>& records)
{
    YCHECK(firstRecordId == RecordCount);
    Append(records);
}

void TChangeLog::TImpl::Append(const TSharedRef& recordData)
{
    int recordId = RecordCount;
    TRecordHeader header(recordId, recordData.Size(), GetChecksum(recordData));

    int readSize = 0;
    readSize += AppendPodPadded(*File, header);
    readSize += AppendPadded(*File, recordData);

    ProcessRecord(recordId, readSize);
}

////////////////////////////////////////////////////////////////////////////////

void TChangeLog::TImpl::Read(int firstRecordId, int recordCount, std::vector<TSharedRef>* records)
{
    // Check stupid conditions.
    YCHECK(firstRecordId >= 0);
    YCHECK(recordCount >= 0);
    YCHECK(State != EState::Uninitialized);

    LOG_DEBUG("Reading records %d-%d", firstRecordId, firstRecordId + recordCount - 1);

    // Prevent search in empty index.
    if (Index.empty()) {
        records->clear();
        return;
    }

    recordCount = std::min(recordCount, RecordCount - firstRecordId);
    int lastRecordId = firstRecordId + recordCount;

    // Read envelope piece of changelog.
    auto envelope = ReadEnvelope(firstRecordId, lastRecordId);

    // Read records from envelope data and save them to the records.
    records->resize(recordCount);
    TMemoryInput inputStream(envelope.Blob.Begin(), envelope.Length());
    for (int recordId = envelope.StartRecordId(); recordId < envelope.EndRecordId(); ++recordId) {
        // Read and check header.
        TRecordHeader header;
        ReadPodPadded(inputStream, header);
        YCHECK(header.RecordId == recordId);

        // Save and pad data.
        TSharedRef data(envelope.Blob, TRef(const_cast<char*>(inputStream.Buf()), header.DataLength));
        inputStream.Skip(AlignUp(header.DataLength));

        // Add data to the records.
        if (recordId >= firstRecordId && recordId < lastRecordId) {
            (*records)[recordId - firstRecordId] = data;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

namespace {

template <class FileType, class HeaderType>
void AtomicWriteHeader(
    const Stroka& fileName,
    const HeaderType& header,
    THolder<FileType>* fileHolder)
{
    Stroka tempFileName(fileName + NFS::TempFileSuffix);
    FileType tempFile(tempFileName, WrOnly|CreateAlways);
    WritePod(tempFile, header);
    tempFile.Close();

    CheckedMoveFile(tempFileName, fileName);
    fileHolder->Reset(new FileType(fileName, RdWr));
    (*fileHolder)->Seek(0, sEnd);
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////

void TChangeLog::TImpl::Create(int prevRecordCount, const TEpochId& epoch)
{
    YCHECK(State == EState::Uninitialized);

    LOG_DEBUG("Creating changelog");

    PrevRecordCount = prevRecordCount;
    Epoch = epoch;
    RecordCount = 0;
    State = EState::Open;

    {
        TGuard<TMutex> guard(Mutex);

        AtomicWriteHeader(FileName, TLogHeader(Id, epoch, prevRecordCount, /*finalized*/ false), &File);
        AtomicWriteHeader(IndexFileName, TLogIndexHeader(Id, 0), &IndexFile);

        CurrentFilePosition = sizeof(TLogHeader);
        CurrentBlockSize = 0;
    }

    LOG_DEBUG("Changelog created");
}

////////////////////////////////////////////////////////////////////////////////

namespace {

template <class T>
void ValidateSignature(const T& header)
{
    LOG_FATAL_UNLESS(header.Signature == T::CorrectSignature,
        "Invalid signature (expected %" PRIx64 ", got %" PRIx64 ")",
        T::CorrectSignature,
        header.Signature);
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////

void TChangeLog::TImpl::Open()
{
    YCHECK(State == EState::Uninitialized);
    LOG_DEBUG("Opening changelog (FileName: %s)", ~FileName);

    TGuard<TMutex> guard(Mutex);

    File.Reset(new TBufferedFile(FileName, RdWr|Seq));

    // Read and check header of changelog.
    TLogHeader header;
    ReadPod(*File, header);
    ValidateSignature(header);
    YCHECK(header.ChangeLogId == Id);

    PrevRecordCount = header.PrevRecordCount;
    Epoch = header.Epoch;
    State = header.Finalized ? EState::Finalized : EState::Open;

    ReadIndex();
    ReadChangeLogUntilEnd();

    LOG_DEBUG("Changelog opened (RecordCount: %d, Finalized: %s)",
        RecordCount,
        ~FormatBool(header.Finalized));
}

////////////////////////////////////////////////////////////////////////////////

void TChangeLog::TImpl::Truncate(int truncatedRecordCount)
{
    YCHECK(State == EState::Open);
    YCHECK(truncatedRecordCount >= 0);

    if (truncatedRecordCount >= RecordCount) {
        return;
    }

    LOG_DEBUG("Truncating changelog: %d->%d",
        RecordCount,
        truncatedRecordCount);

    auto envelope = ReadEnvelope(truncatedRecordCount, truncatedRecordCount);
    if (truncatedRecordCount == 0) {
        Index.clear();
    } else {
        auto cutBound = (envelope.LowerBound.RecordId == truncatedRecordCount) ? envelope.LowerBound : envelope.UpperBound;
        auto indexPosition =
            std::lower_bound(Index.begin(), Index.end(), cutBound) - Index.begin();
        Index.resize(indexPosition);
    }
    
    i64 readSize = 0;
    TMemoryInput inputStream(envelope.Blob.Begin(), envelope.Length());
    for (int i = envelope.StartRecordId(); i < truncatedRecordCount; ++i) {
        TRecordHeader header;
        readSize += ReadPodPadded(inputStream, header);
        auto alignedSize = AlignUp(header.DataLength);
        inputStream.Skip(alignedSize);
        readSize += alignedSize;
    }

    RecordCount = truncatedRecordCount;
    CurrentBlockSize = readSize;
    CurrentFilePosition = envelope.StartPosition() + readSize;
    {
        TGuard<TMutex> guard(Mutex);
        IndexFile->Resize(sizeof(TLogIndexHeader) + Index.size() * sizeof(TLogIndexRecord));
        RefreshIndexHeader();
        File->Resize(CurrentFilePosition);
        File->Seek(0, sEnd);
    }
}

////////////////////////////////////////////////////////////////////////////////

void TChangeLog::TImpl::Flush()
{
    LOG_DEBUG("Flushing changelog");
    {
        TGuard<TMutex> guard(Mutex);
        File->Flush();
        IndexFile->Flush();
    }
    LOG_DEBUG("Changelog flushed");
}


////////////////////////////////////////////////////////////////////////////////

void TChangeLog::TImpl::WriteHeader(bool finalized)
{
    TGuard<TMutex> guard(Mutex);

    File->Seek(0, sSet);
    WritePod(*File, TLogHeader(Id, Epoch, PrevRecordCount, finalized));
    File->Flush();
}

void TChangeLog::TImpl::Finalize()
{
    YCHECK(State != EState::Uninitialized);
    if (State == EState::Finalized) {
        return;
    }

    LOG_DEBUG("Finalizing changelog");
    WriteHeader(true);
    State = EState::Finalized;
    LOG_DEBUG("Changelog finalized");
}

void TChangeLog::TImpl::Definalize()
{
    YCHECK(State == EState::Finalized);

    LOG_DEBUG("Definalizing changelog");

    WriteHeader(false);
    
    {
        // Additionally seek to the end of changelog
        TGuard<TMutex> guard(Mutex);
        File->Seek(0, sEnd);
    }
    State = EState::Open;

    LOG_DEBUG("Changelog definalized");
}


////////////////////////////////////////////////////////////////////////////////

int TChangeLog::TImpl::GetId() const
{
    return Id;
}

int TChangeLog::TImpl::GetPrevRecordCount() const
{
    return PrevRecordCount;
}

int TChangeLog::TImpl::GetRecordCount() const
{
    return RecordCount;
}

const TEpochId& TChangeLog::TImpl::GetEpoch() const
{
    YCHECK(State != EState::Uninitialized);
    return Epoch;
}

bool TChangeLog::TImpl::IsFinalized() const
{
    return State == EState::Finalized;
}

////////////////////////////////////////////////////////////////////////////////

namespace {

struct TRecordInfo
{
    TRecordInfo():
        Id(-1), TotalSize(-1)
    { }

    TRecordInfo(int id, int takenPlace):
        Id(id), TotalSize(takenPlace)
    { }

    int Id;
    int TotalSize;
};

// Trying to read one record from changelog file.
// Returns Null if reading is failed and record info otherwise.
template <class Stream>
TNullable<TRecordInfo> ReadRecord(TCheckableFileReader<Stream>& input)
{
    int readSize = 0;
    TRecordHeader header;
    readSize += ReadPodPadded(input, header);
    if (!input.Success() || header.DataLength <= 0) {
        return Null;
    }

    TSharedRef data(header.DataLength);
    readSize += ReadPadded(input, data.GetRef());
    if (!input.Success()) {
        return Null;
    }

    auto checksum = GetChecksum(data);
    LOG_FATAL_UNLESS(header.Checksum == checksum,
        "Incorrect checksum of record %d", header.RecordId);
    return TRecordInfo(header.RecordId, readSize);
}

// Calculates maximal correct prefix of index.
size_t GetMaxCorrectIndexPrefix(const std::vector<TLogIndexRecord>& index, TBufferedFile* changelogFile)
{
    // Check adequacy of index
    size_t correctPrefixLength = 0;
    for (int i = 0; i < index.size(); ++i) {
        bool correct;
        if (i == 0) {
            correct = index[i].FilePosition == sizeof(TLogHeader) && index[i].RecordId == 0;
        } else {
            correct =
                index[i].FilePosition > index[i - 1].FilePosition &&
                index[i].RecordId > index[i - 1].RecordId;
        }
        if (!correct) {
            break;
        }
        correctPrefixLength += 1;
    }
    
    // Truncate excess index records
    i64 fileLength = changelogFile->GetLength();
    while (correctPrefixLength > 0 && index[correctPrefixLength - 1].FilePosition > fileLength) {
        correctPrefixLength -= 1;
    }
    
    if (correctPrefixLength == 0) {
        return 0;
    }

    // Truncate last index record if changelog file is corrupted
    changelogFile->Seek(index[correctPrefixLength - 1].FilePosition, sSet);
    auto checkableFile = CreateCheckableReader(*changelogFile);
    if (!ReadRecord(checkableFile)) {
        correctPrefixLength -= 1;
    }

    return correctPrefixLength;
}


} // anonymous namespace


void TChangeLog::TImpl::ProcessRecord(int recordId, int readSize)
{
    if (CurrentBlockSize >= IndexBlockSize || RecordCount == 0) {
        // Add index record in two cases:
        // 1) processing first record;
        // 2) size of records since previous index record is more than IndexBlockSize.
        YCHECK(Index.empty() || Index.back().RecordId != recordId);

        CurrentBlockSize = 0;
        Index.push_back(TLogIndexRecord(recordId, CurrentFilePosition));
        {
            TGuard<TMutex> guard(Mutex);
            WritePod(*IndexFile, Index.back());
            RefreshIndexHeader();
        }
        LOG_DEBUG("Changelog index record added (RecordId: %d, Offset: %" PRId64 ")",
            recordId, CurrentFilePosition);
    }
    // Record appended successfully.
    CurrentBlockSize += readSize;
    CurrentFilePosition += readSize;
    RecordCount += 1;
}

////////////////////////////////////////////////////////////////////////////////

void TChangeLog::TImpl::ReadIndex()
{
    // Read an existing index.
    {
        TMappedFileInput indexStream(IndexFileName);

        // Read and check index header.
        TLogIndexHeader indexHeader;
        ReadPod(indexStream, indexHeader);
        ValidateSignature(indexHeader);
        YCHECK(indexHeader.IndexSize >= 0);

        // Read index records.
        for (int i = 0; i < indexHeader.IndexSize; ++i) {
            TLogIndexRecord indexRecord;
            ReadPod(indexStream, indexRecord);
            Index.push_back(indexRecord);
        }
    }
    // Compute the maximum correct prefix and truncate the index.
    {
        auto correctPrefixSize = GetMaxCorrectIndexPrefix(Index, &(*File));
        LOG_ERROR_IF(correctPrefixSize < Index.size(), "Changelog index contains incorrect records");
        Index.resize(correctPrefixSize);

        IndexFile.Reset(new TFile(IndexFileName, RdWr|Seq|CloseOnExec));
        IndexFile->Resize(sizeof(TLogIndexHeader) + Index.size() * sizeof(TLogIndexRecord));
        IndexFile->Seek(0, sEnd);
    }
}

void TChangeLog::TImpl::RefreshIndexHeader()
{
    i64 currentIndexPosition = IndexFile->GetPosition();
    IndexFile->Seek(0, sSet);
    WritePod(*IndexFile, TLogIndexHeader(Id, Index.size()));
    IndexFile->Seek(currentIndexPosition, sSet);
}

void TChangeLog::TImpl::ReadChangeLogUntilEnd()
{
    // Extract changelog properties from index.
    i64 fileLength = File->GetLength();
    CurrentBlockSize = 0;
    if (Index.empty()) {
        RecordCount = 0;
        CurrentFilePosition = sizeof(TLogHeader);
    }
    else {
        // Record count would be set below.
        CurrentFilePosition = Index.back().FilePosition;
    }
    
    // Seek to proper position in file, initialize checkable reader.
    File->Seek(CurrentFilePosition, sSet);
    auto checkableFile = CreateCheckableReader(*File);

    TNullable<TRecordInfo> recordInfo;
    if (!Index.empty()) {
        // Skip first record.
        recordInfo = ReadRecord(checkableFile);
        // It should be correct because we have already check index.
        YASSERT(recordInfo);
        RecordCount = Index.back().RecordId + 1;
        CurrentFilePosition += recordInfo->TotalSize;
    }

    while (CurrentFilePosition < fileLength) {
        // Record size also counts size of record header.
        recordInfo = ReadRecord(checkableFile);
        if (!recordInfo || recordInfo->Id != RecordCount) {
            // Broken changelog case.
            if (State == EState::Finalized) {
                LOG_ERROR("Finalized changelog contains a broken record (RecordId: %d, Offset: %" PRId64 ")",
                    RecordCount,
                    CurrentFilePosition);
            } else {
                LOG_ERROR("Broken record found, changelog trimmed (RecordId: %d, Offset: %" PRId64 ")",
                    RecordCount,
                    CurrentFilePosition);
            }
            File->Resize(CurrentFilePosition);
            File->Seek(0, sEnd);
            break;
        }
        ProcessRecord(recordInfo->Id, recordInfo->TotalSize);
    }
}

////////////////////////////////////////////////////////////////////////////////

namespace {

// This method uses forward iterator instead of reverse because they work faster.
// Asserts if last not greater element is absent.
template <class T>
typename std::vector<T>::const_iterator LastNotGreater(const std::vector<T>& vec, const T& value)
{
    auto res = std::upper_bound(vec.begin(), vec.end(), value);
    YCHECK(res != vec.begin());
    --res;
    return res;
}

template <class T>
typename std::vector<T>::const_iterator FirstGreater(const std::vector<T>& vec, const T& value)
{
    auto res = std::upper_bound(vec.begin(), vec.end(), value);
    return res;
}

} // anonymous namesapce

TChangeLog::TImpl::TEnvelopeData TChangeLog::TImpl::ReadEnvelope(int firstRecordId, int lastRecordId)
{
    TEnvelopeData result;
    result.LowerBound = *LastNotGreater(Index, TLogIndexRecord(firstRecordId, -1));
    auto it = FirstGreater(Index, TLogIndexRecord(lastRecordId, -1));
    result.UpperBound =
        it != Index.end() ?
        *it :
        TLogIndexRecord(RecordCount, CurrentFilePosition);

    {
        result.Blob = TSharedRef(result.Length());
        TGuard<TMutex> guard(Mutex);
        size_t bytesRead = File->Pread(
            result.Blob.Begin(),
            result.Length(),
            result.StartPosition());
        YCHECK(bytesRead == result.Length());
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
