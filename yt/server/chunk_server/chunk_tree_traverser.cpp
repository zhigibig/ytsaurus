#include "chunk_tree_traverser.h"
#include "chunk.h"
#include "chunk_list.h"
#include "helpers.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>

#include <yt/server/security_server/security_manager.h>
#include <yt/server/security_server/user.h>

#include <yt/server/object_server/object.h>

#include <yt/ytlib/object_client/public.h>

#include <yt/ytlib/table_client/unversioned_row.h>

#include <yt/core/misc/singleton.h>

#include <yt/core/profiling/scoped_timer.h>

namespace NYT {
namespace NChunkServer {

using namespace NCellMaster;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NChunkClient;
using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

static const int MaxChunksPerStep = 1000;

static const auto RowCountMember = &TChunkList::TCumulativeStatisticsEntry::RowCount;
static const auto ChunkCountMember = &TChunkList::TCumulativeStatisticsEntry::ChunkCount;
static const auto DataSizeMember = &TChunkList::TCumulativeStatisticsEntry::DataSize;

////////////////////////////////////////////////////////////////////////////////

template <class TIterator, class TKey, class TIsLess, class TIsMissing>
TIterator UpperBoundWithMissingValues(
    TIterator start,
    TIterator end,
    const TKey& key,
    TIsLess isLess,
    TIsMissing isMissing)
{
    while (true) {
        auto distance = std::distance(start, end);
        if (distance <= 1) {
            break;
        }
        auto median = start + (distance / 2);
        auto cur = median;
        while (cur > start && isMissing(*cur)) {
            --cur;
        }
        if (isMissing(*cur)) {
            start = median;
        } else {
            if (isLess(key, *cur)) {
                end = cur;
            } else {
                start = median;
            }
        }
    }
    if (!isMissing(*start) && isLess(key, *start)) {
        return start;
    } else {
        return end;
    }
}

class TChunkTreeTraverser
    : public TRefCounted
{
protected:
    struct TStackEntry
    {
        TChunkList* ChunkList;
        int ChunkListVersion;
        int ChildIndex;
        i64 RowIndex;
        TReadLimit LowerBound;
        TReadLimit UpperBound;

        TStackEntry(
            TChunkList* chunkList,
            int childIndex,
            i64 rowIndex,
            const TReadLimit& lowerBound,
            const TReadLimit& upperBound)
            : ChunkList(chunkList)
            , ChunkListVersion(chunkList->GetVersion())
            , ChildIndex(childIndex)
            , RowIndex(rowIndex)
            , LowerBound(lowerBound)
            , UpperBound(upperBound)
        { }
    };

    void DoTraverse()
    {
        try {
            GuardedTraverse();
        } catch (const std::exception& ex) {
            Shutdown();
            Visitor_->OnFinish(TError(ex));
        }
    }

    void GuardedTraverse()
    {
        NProfiling::TScopedTimer timer;
        auto invoker = Callbacks_->GetInvoker();
        int visitedChunkCount = 0;
        while (visitedChunkCount < MaxChunksPerStep || !invoker) {
            if (IsStackEmpty()) {
                Shutdown();
                Callbacks_->OnTimeSpent(timer.GetElapsed());
                Visitor_->OnFinish(TError());
                return;
            }

            auto& entry = PeekStack();
            auto* chunkList = entry.ChunkList;

            if (!chunkList->IsAlive() || chunkList->GetVersion() != entry.ChunkListVersion) {
                THROW_ERROR_EXCEPTION(
                    NRpc::EErrorCode::Unavailable,
                    "Optimistic locking failed for chunk list %v",
                    chunkList->GetId());
            }

            if (entry.ChildIndex == chunkList->Children().size()) {
                PopStack();
                continue;
            }

            const auto& statistics = chunkList->Statistics();
            auto* child = chunkList->Children()[entry.ChildIndex];

            // YT-4840: Skip empty children since Get(Min|Max)Key will not work for them.
            if (IsEmpty(child)) {
                ++entry.ChildIndex;
                continue;
            }

            TReadLimit childLowerBound;
            TReadLimit childUpperBound;

            auto fetchPrevSum = [&] (i64 TChunkList::TCumulativeStatisticsEntry::* member) -> i64 {
                return entry.ChildIndex == 0
                    ? 0
                    : chunkList->CumulativeStatistics()[entry.ChildIndex - 1].*member;
            };

            auto fetchCurrentSum = [&] (i64 TChunkList::TCumulativeStatisticsEntry::* member, i64 fallback) {
                return entry.ChildIndex == chunkList->Children().size() - 1
                    ? fallback
                    : chunkList->CumulativeStatistics()[entry.ChildIndex].*member;
            };

            i64 rowIndex = 0;
            if (chunkList->GetOrdered()) {
                // Row index
                {
                    i64 childLimit = fetchPrevSum(RowCountMember);
                    rowIndex =  entry.RowIndex + childLimit;
                    if (entry.UpperBound.HasRowIndex()) {
                        if (entry.UpperBound.GetRowIndex() <= childLimit) {
                            PopStack();
                            continue;
                        }
                        childLowerBound.SetRowIndex(childLimit);
                        i64 totalRowCount = statistics.Sealed ? statistics.LogicalRowCount : std::numeric_limits<i64>::max();
                        childUpperBound.SetRowIndex(fetchCurrentSum(RowCountMember, totalRowCount));
                    } else if (entry.LowerBound.HasRowIndex()) {
                        childLowerBound.SetRowIndex(childLimit);
                    }
                }

                // Chunk index
                {
                    i64 childLimit = fetchPrevSum(ChunkCountMember);
                    if (entry.UpperBound.HasChunkIndex()) {
                        if (entry.UpperBound.GetChunkIndex() <= childLimit) {
                            PopStack();
                            continue;
                        }
                        childLowerBound.SetChunkIndex(childLimit);
                        childUpperBound.SetChunkIndex(fetchCurrentSum(ChunkCountMember, statistics.LogicalChunkCount));
                    } else if (entry.LowerBound.HasChunkIndex()) {
                        childLowerBound.SetChunkIndex(childLimit);
                    }
                }

                // Offset
                {
                    i64 childLimit = fetchPrevSum(DataSizeMember);
                    if (entry.UpperBound.HasOffset()) {
                        if (entry.UpperBound.GetOffset() <= childLimit) {
                            PopStack();
                            continue;
                        }
                        childLowerBound.SetOffset(childLimit);
                        childUpperBound.SetOffset(fetchCurrentSum(DataSizeMember, statistics.UncompressedDataSize));
                    } else if (entry.LowerBound.HasOffset()) {
                        childLowerBound.SetOffset(childLimit);
                    }
                }

                // Key
                {
                    if (entry.UpperBound.HasKey()) {
                        childLowerBound.SetKey(GetMinKey(child));
                        if (entry.UpperBound.GetKey() <= childLowerBound.GetKey()) {
                            PopStack();
                            continue;
                        }
                        childUpperBound.SetKey(GetMaxKey(child));
                    } else if (entry.LowerBound.HasKey()) {
                        childLowerBound.SetKey(GetMinKey(child));
                    }
                }
            }

            ++entry.ChildIndex;

            TReadLimit subtreeStartLimit;
            TReadLimit subtreeEndLimit;
            GetInducedSubtreeLimits(
                entry,
                childLowerBound,
                childUpperBound,
                &subtreeStartLimit,
                &subtreeEndLimit);

            switch (child->GetType()) {
                case EObjectType::ChunkList: {
                    auto* childChunkList = child->AsChunkList();
                    int childIndex = GetStartChildIndex(childChunkList, subtreeStartLimit);
                    PushStack(TStackEntry(
                        childChunkList,
                        childIndex,
                        rowIndex,
                        subtreeStartLimit,
                        subtreeEndLimit));
                    break;
                }

                case EObjectType::Chunk:
                case EObjectType::ErasureChunk:
                case EObjectType::JournalChunk: {
                    auto* childChunk = child->AsChunk();
                    if (!Visitor_->OnChunk(childChunk, rowIndex, subtreeStartLimit, subtreeEndLimit)) {
                        Shutdown();
                        return;
                    }
                    ++visitedChunkCount;
                    break;
                 }

                default:
                    Y_UNREACHABLE();
            }
        }

        // Schedule continuation.
        Callbacks_->OnTimeSpent(timer.GetElapsed());
        invoker->Invoke(BIND(&TChunkTreeTraverser::DoTraverse, MakeStrong(this)));
    }

    static int GetStartChildIndex(
        const TChunkList* chunkList,
        const TReadLimit& lowerBound)
    {
        if (chunkList->Children().empty()) {
            return 0;
        }

        int result = 0;
        const auto& statistics = chunkList->Statistics();

        auto adjustResult = [&] (i64 TChunkList::TCumulativeStatisticsEntry::* member, i64 limit, i64 total) {
            const auto& cumulativeStatistics = chunkList->CumulativeStatistics();
            if (limit < total) {
                auto it = std::upper_bound(
                    cumulativeStatistics.begin(),
                    cumulativeStatistics.end(),
                    limit,
                    [&] (i64 lhs, const TChunkList::TCumulativeStatisticsEntry& rhs) {
                        return lhs < rhs.*member;
                    });
                result = std::max(result, static_cast<int>(it - cumulativeStatistics.begin()));
            } else {
                result = chunkList->Children().size();
            }
        };

        // Row Index
        if (lowerBound.HasRowIndex()) {
            i64 totalRowCount = statistics.Sealed ? statistics.LogicalRowCount : std::numeric_limits<i64>::max();
            adjustResult(RowCountMember, lowerBound.GetRowIndex(), totalRowCount);
        }

        // Chunk index
        if (lowerBound.HasChunkIndex()) {
            adjustResult(ChunkCountMember, lowerBound.GetChunkIndex(), statistics.LogicalChunkCount);
        }

        // Offset
        if (lowerBound.HasOffset()) {
            adjustResult(DataSizeMember, lowerBound.GetOffset(), statistics.UncompressedDataSize);
        }

        // Key
        if (lowerBound.HasKey()) {
            typedef std::vector<TChunkTree*>::const_iterator TChildrenIterator;
            std::reverse_iterator<TChildrenIterator> rbegin(chunkList->Children().end());
            std::reverse_iterator<TChildrenIterator> rend(chunkList->Children().begin());

            auto it = UpperBoundWithMissingValues(
                rbegin,
                rend,
                lowerBound.GetKey(),
                // isLess
                [] (const TOwningKey& key, const TChunkTree* chunkTree) {
                    return key > GetMaxKey(chunkTree);
                },
                // isMissing
                [] (const TChunkTree* chunkTree) {
                    return IsEmpty(chunkTree);
                });

            result = std::max(result, static_cast<int>(rend - it));
        }

        return result;
    }

    static void GetInducedSubtreeLimits(
        const TStackEntry& stackEntry,
        const TReadLimit& childLowerBound,
        const TReadLimit& childUpperBound,
        TReadLimit* startLimit,
        TReadLimit* endLimit)
    {
        // Row index
        if (stackEntry.LowerBound.HasRowIndex()) {
            i64 newLowerBound = stackEntry.LowerBound.GetRowIndex() - childLowerBound.GetRowIndex();
            if (newLowerBound > 0) {
                startLimit->SetRowIndex(newLowerBound);
            }
        }
        if (stackEntry.UpperBound.HasRowIndex() &&
            stackEntry.UpperBound.GetRowIndex() < childUpperBound.GetRowIndex())
        {
            i64 newUpperBound = stackEntry.UpperBound.GetRowIndex() - childLowerBound.GetRowIndex();
            Y_ASSERT(newUpperBound > 0);
            endLimit->SetRowIndex(newUpperBound);
        }

        // Chunk index
        if (stackEntry.LowerBound.HasChunkIndex()) {
            i64 newLowerBound = stackEntry.LowerBound.GetChunkIndex() - childLowerBound.GetChunkIndex();
            if (newLowerBound > 0) {
                startLimit->SetChunkIndex(newLowerBound);
            }
        }
        if (stackEntry.UpperBound.HasChunkIndex() &&
            stackEntry.UpperBound.GetChunkIndex() < childUpperBound.GetChunkIndex())
        {
            i64 newUpperBound = stackEntry.UpperBound.GetChunkIndex() - childLowerBound.GetChunkIndex();
            YCHECK(newUpperBound > 0);
            endLimit->SetChunkIndex(newUpperBound);
        }

        // Offset
        if (stackEntry.LowerBound.HasOffset()) {
            i64 newLowerBound = stackEntry.LowerBound.GetOffset() - childLowerBound.GetOffset();
            if (newLowerBound > 0) {
                startLimit->SetOffset(newLowerBound);
            }
        }
        if (stackEntry.UpperBound.HasOffset() &&
            stackEntry.UpperBound.GetOffset() < childUpperBound.GetOffset())
        {
            i64 newUpperBound = stackEntry.UpperBound.GetOffset() - childLowerBound.GetOffset();
            Y_ASSERT(newUpperBound > 0);
            endLimit->SetOffset(newUpperBound);
        }

        // Key
        if (stackEntry.LowerBound.HasKey() &&
            stackEntry.LowerBound.GetKey() > childLowerBound.GetKey())
        {
            startLimit->SetKey(stackEntry.LowerBound.GetKey());
        }
        if (stackEntry.UpperBound.HasKey() &&
            stackEntry.UpperBound.GetKey() < childUpperBound.GetKey())
        {
            endLimit->SetKey(stackEntry.UpperBound.GetKey());
        }
    }

    bool IsStackEmpty()
    {
        return Stack_.empty();
    }

    void PushStack(const TStackEntry& newEntry)
    {
        Callbacks_->OnPush(newEntry.ChunkList);
        Stack_.push_back(newEntry);
    }

    TStackEntry& PeekStack()
    {
        return Stack_.back();
    }

    void PopStack()
    {
        auto& entry = Stack_.back();
        Callbacks_->OnPop(entry.ChunkList);
        Stack_.pop_back();
    }

    void Shutdown()
    {
        std::vector<TChunkTree*> nodes;
        for (const auto& entry : Stack_) {
            nodes.push_back(entry.ChunkList);
        }
        Callbacks_->OnShutdown(nodes);
        Stack_.clear();
    }

    const IChunkTraverserCallbacksPtr Callbacks_;
    const IChunkVisitorPtr Visitor_;

    std::vector<TStackEntry> Stack_;

public:
    TChunkTreeTraverser(
        IChunkTraverserCallbacksPtr callbacks,
        IChunkVisitorPtr visitor)
        : Callbacks_(std::move(callbacks))
        , Visitor_(std::move(visitor))
    { }

    void Run(
        TChunkList* chunkList,
        const TReadLimit& lowerBound,
        const TReadLimit& upperBound)
    {
        int childIndex = GetStartChildIndex(chunkList, lowerBound);
        PushStack(TStackEntry(
            chunkList,
            childIndex,
            0,
            lowerBound,
            upperBound));

        // Do actual traversing in the proper queue.
        auto invoker = Callbacks_->GetInvoker();
        if (invoker) {
            invoker->Invoke(BIND(&TChunkTreeTraverser::DoTraverse, MakeStrong(this)));
        } else {
            DoTraverse();
        }
    }
};

void TraverseChunkTree(
    IChunkTraverserCallbacksPtr traverserCallbacks,
    IChunkVisitorPtr visitor,
    TChunkList* root,
    const TReadLimit& lowerLimit,
    const TReadLimit& upperLimit)
{
    auto traverser = New<TChunkTreeTraverser>(
        std::move(traverserCallbacks),
        std::move(visitor));

    traverser->Run(root, lowerLimit, upperLimit);
}

////////////////////////////////////////////////////////////////////////////////

class TPreemptableChunkTraverserCallbacks
    : public IChunkTraverserCallbacks
{
public:
    explicit TPreemptableChunkTraverserCallbacks(NCellMaster::TBootstrap* bootstrap)
        : Bootstrap_(bootstrap)
        , UserName_(Bootstrap_
            ->GetSecurityManager()
            ->GetAuthenticatedUser()
            ->GetName())
    { }

    virtual IInvokerPtr GetInvoker() const override
    {
        return Bootstrap_
            ->GetHydraFacade()
            ->GetEpochAutomatonInvoker(EAutomatonThreadQueue::ChunkTraverser);
    }

    virtual void OnPop(TChunkTree* node) override
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->WeakUnrefObject(node);
    }

    virtual void OnPush(TChunkTree* node) override
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        objectManager->WeakRefObject(node);
    }

    virtual void OnShutdown(const std::vector<TChunkTree*>& nodes) override
    {
        const auto& objectManager = Bootstrap_->GetObjectManager();
        for (const auto& node : nodes) {
            objectManager->WeakUnrefObject(node);
        }
    }

    virtual void OnTimeSpent(TDuration time) override
    {
        const auto& securityManager = Bootstrap_->GetSecurityManager();
        auto* user = securityManager->FindUserByName(UserName_);
        if (IsObjectAlive(user)) {
            securityManager->ChargeUserRead(user, 0, time);
        }
    }

private:
    NCellMaster::TBootstrap* const Bootstrap_;
    const Stroka UserName_;

};

IChunkTraverserCallbacksPtr CreatePreemptableChunkTraverserCallbacks(
    NCellMaster::TBootstrap* bootstrap)
{
    return New<TPreemptableChunkTraverserCallbacks>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

class TNonpreemptableChunkTraverserCallbacks
    : public IChunkTraverserCallbacks
{
public:
    virtual IInvokerPtr GetInvoker() const override
    {
        return nullptr;
    }

    virtual void OnPop(TChunkTree* /*node*/) override
    { }

    virtual void OnPush(TChunkTree* /*node*/) override
    { }

    virtual void OnShutdown(const std::vector<TChunkTree*>& /*nodes*/) override
    { }

    virtual void OnTimeSpent(TDuration /*time*/) override
    { }
};

IChunkTraverserCallbacksPtr GetNonpreemptableChunkTraverserCallbacks()
{
    return RefCountedSingleton<TNonpreemptableChunkTraverserCallbacks>();
}

////////////////////////////////////////////////////////////////////////////////

class TEnumeratingChunkVisitor
    : public IChunkVisitor
{
public:
    explicit TEnumeratingChunkVisitor(std::vector<TChunk*>* chunks)
        : Chunks_(chunks)
    { }

    virtual bool OnChunk(
        TChunk* chunk,
        i64 /*rowIndex*/,
        const NChunkClient::TReadLimit& /*startLimit*/,
        const NChunkClient::TReadLimit& /*endLimit*/) override
    {
        Chunks_->push_back(chunk);
        return true;
    }

    virtual void OnFinish(const TError& error) override
    {
        YCHECK(error.IsOK());
    }

private:
    std::vector<TChunk*>* const Chunks_;

};

void EnumerateChunksInChunkTree(
    TChunkList* root,
    std::vector<TChunk*>* chunks,
    const NChunkClient::TReadLimit& lowerLimit,
    const NChunkClient::TReadLimit& upperLimit)
{
    auto visitor = New<TEnumeratingChunkVisitor>(chunks);
    TraverseChunkTree(
        GetNonpreemptableChunkTraverserCallbacks(),
        visitor,
        root,
        lowerLimit,
        upperLimit);
}

std::vector<TChunk*> EnumerateChunksInChunkTree(
    TChunkList* root,
    const NChunkClient::TReadLimit& lowerLimit,
    const NChunkClient::TReadLimit& upperLimit)
{
    std::vector<TChunk*> chunks;
    EnumerateChunksInChunkTree(
        root,
        &chunks,
        lowerLimit,
        upperLimit);
    return chunks;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT
