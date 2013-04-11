#include "stdafx.h"
#include "node.h"

#include <server/chunk_server/job.h>
#include <server/chunk_server/chunk.h>

#include <server/cell_master/serialization_context.h>

namespace NYT {
namespace NNodeTrackerServer {

using namespace NChunkServer;

////////////////////////////////////////////////////////////////////////////////

TNode::TNode(
    TNodeId id,
    const TNodeDescriptor& descriptor)
    : Id_(id)
    , Descriptor_(descriptor)
{
    Init();
}

TNode::TNode(TNodeId id)
    : Id_(id)
{
    Init();
}

void TNode::Init()
{
    Confirmed_ = false;
    ChunksToReplicate_.resize(ReplicationPriorityCount);
    HintedSessionCount_ = 0;
}

const TNodeDescriptor& TNode::GetDescriptor() const
{
    return Descriptor_;
}

const Stroka& TNode::GetAddress() const
{
    return Descriptor_.Address;
}

void TNode::Save(const NCellMaster::TSaveContext& context) const
{
    auto* output = context.GetOutput();
    ::Save(output, Descriptor_.Address);
    ::Save(output, State_);
    SaveProto(output, Statistics_);
    SaveObjectRefs(context, StoredReplicas_);
    SaveObjectRefs(context, CachedReplicas_);
    SaveObjectRefs(context, UnapprovedReplicas_);
}

void TNode::Load(const NCellMaster::TLoadContext& context)
{
    auto* input = context.GetInput();
    ::Load(input, Descriptor_.Address);
    ::Load(input, State_);
    LoadProto(input, Statistics_);
    LoadObjectRefs(context, StoredReplicas_);
    LoadObjectRefs(context, CachedReplicas_);
    LoadObjectRefs(context, UnapprovedReplicas_);
}

void TNode::AddJob(TJobPtr job)
{
    YCHECK(Jobs_.insert(job).second);
}

void TNode::RemoveJob(TJobPtr job)
{
    YCHECK(Jobs_.erase(job) == 1);
}

void TNode::AddReplica(TChunkPtrWithIndex replica, bool cached)
{
    if (cached) {
        YCHECK(CachedReplicas_.insert(replica).second);
    } else {
        YCHECK(StoredReplicas_.insert(replica).second);
    }
}

void TNode::RemoveReplica(TChunkPtrWithIndex replica, bool cached)
{
    if (cached) {
        YCHECK(CachedReplicas_.erase(replica) == 1);
    } else {
        YCHECK(StoredReplicas_.erase(replica) == 1);
        UnapprovedReplicas_.erase(replica);
    }
}

bool TNode::HasReplica(TChunkPtrWithIndex replica, bool cached) const
{
    if (cached) {
        return CachedReplicas_.find(replica) != CachedReplicas_.end();
    } else {
        return StoredReplicas_.find(replica) != StoredReplicas_.end();
    }
}

void TNode::MarkReplicaUnapproved(TChunkPtrWithIndex replica)
{
    YASSERT(HasReplica(replica, false));
    YCHECK(UnapprovedReplicas_.insert(replica).second);
}

bool TNode::HasUnapprovedReplica(TChunkPtrWithIndex replica) const
{
    return UnapprovedReplicas_.find(replica) != UnapprovedReplicas_.end();
}

void TNode::ApproveReplica(TChunkPtrWithIndex replica)
{
    YASSERT(HasReplica(replica, false));
    YCHECK(UnapprovedReplicas_.erase(replica) == 1);
}

int TNode::GetTotalSessionCount() const
{
    return HintedSessionCount_ + Statistics_.total_session_count();
}

TNodeId GetObjectId(const TNode* node)
{
    return node->GetId();
}

bool CompareObjectsForSerialization(const TNode* lhs, const TNode* rhs)
{
    return GetObjectId(lhs) < GetObjectId(rhs);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT

