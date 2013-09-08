#include "stdafx.h"
#include "private.h"
#include "snapshot_store.h"
#include "snapshot.h"
#include "config.h"

#include <ytlib/misc/fs.h>

#include <ytlib/concurrency/thread_affinity.h>

#include <util/folder/filelist.h>
#include <util/folder/dirut.h>

namespace NYT {
namespace NMetaState {

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = MetaStateLogger;

static const char* const SnapshotExtension = "snapshot";

////////////////////////////////////////////////////////////////////////////////

TSnapshotStore::TSnapshotStore(TSnapshotStoreConfigPtr config)
    : Config(config)
    , Started(false)
{ }

void TSnapshotStore::Start()
{
    YCHECK(!Started);

    auto path = Config->Path;

    LOG_INFO("Preparing snapshot directory %s", ~path.Quote());

    NFS::ForcePath(path);
    NFS::CleanTempFiles(path);

    LOG_INFO("Looking for snapshots in %s", ~path.Quote());

    TFileList fileList;
    fileList.Fill(path);

    Stroka fileName;
    while ((fileName = fileList.Next()) != NULL) {
        auto extension = NFS::GetFileExtension(fileName);
        if (extension == SnapshotExtension) {
            auto name = NFS::GetFileNameWithoutExtension(fileName);
            try {
                i32 snapshotId = FromString<i32>(name);
                SnapshotIds.insert(snapshotId);
                LOG_INFO("Found snapshot %d", snapshotId);
            } catch (const std::exception&) {
                LOG_WARNING("Found unrecognized file %s", ~fileName.Quote());
            }
        }
    }

    LOG_INFO("Snapshot scan complete");
    Started = true;
}

Stroka TSnapshotStore::GetSnapshotFileName(i32 snapshotId)
{
    VERIFY_THREAD_AFFINITY_ANY();

    return NFS::CombinePaths(Config->Path, Sprintf("%09d.%s", snapshotId, SnapshotExtension));
}

TSnapshotStore::TGetReaderResult TSnapshotStore::GetReader(i32 snapshotId)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YCHECK(Started);
    YCHECK(snapshotId > 0);

    auto fileName = GetSnapshotFileName(snapshotId);
    if (!isexist(~fileName)) {
        {
            TGuard<TSpinLock> guard(SpinLock);
            if (SnapshotIds.erase(snapshotId) == 1) {
                LOG_WARNING("Erased orphaned snapshot id snapshot %d from store",
                    snapshotId);
            }
        }
        return TError(
            EErrorCode::NoSuchSnapshot,
            Sprintf("No such snapshot %d", snapshotId));
    }

    return New<TSnapshotReader>(fileName, snapshotId, Config->EnableCompression);
}

TSnapshotWriterPtr TSnapshotStore::GetWriter(i32 snapshotId)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YCHECK(Started);
    YCHECK(snapshotId > 0);

    auto fileName = GetSnapshotFileName(snapshotId);
    return New<TSnapshotWriter>(fileName, snapshotId, Config->EnableCompression);
}

i32 TSnapshotStore::GetLatestSnapshotId(i32 maxSnapshotId)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YCHECK(Started);

    TGuard<TSpinLock> guard(SpinLock);
    auto it = SnapshotIds.upper_bound(maxSnapshotId);
    if (it == SnapshotIds.begin()) {
        return NonexistingSnapshotId;
    }
    int snapshotId = *(--it);
    YCHECK(snapshotId <= maxSnapshotId);
    return snapshotId;
}

void TSnapshotStore::OnSnapshotAdded(i32 snapshotId)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YCHECK(Started);

    TGuard<TSpinLock> guard(SpinLock);
    SnapshotIds.insert(snapshotId);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NMetaState
} // namespace NYT
