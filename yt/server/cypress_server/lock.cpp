#include "lock.h"

#include <yt/server/cell_master/serialize.h>

#include <yt/server/transaction_server/transaction.h>

#include <yt/core/misc/serialize.h>

namespace NYT {
namespace NCypressServer {

using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

bool TLockKey::operator ==(const TLockKey& other) const
{
    return Kind == other.Kind && Name == other.Name;
}

bool TLockKey::operator !=(const TLockKey& other) const
{
    return !(*this == other);
}

bool TLockKey::operator <(const TLockKey& other) const
{
    return std::tie(Kind, Name) < std::tie(other.Kind, other.Name);
}

TLockKey::operator size_t() const
{
    return THash<ELockKeyKind>()(Kind) ^ THash<TString>()(Name);
}

void TLockKey::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Kind);
    Persist(context, Name);
}

void FormatValue(TStringBuilder* builder, const TLockKey& key, const TStringBuf& format)
{
    if (key.Kind == ELockKeyKind::None) {
        builder->AppendFormat("%v", key.Kind);
    } else {
        builder->AppendFormat("%v[%v]", key.Kind, key.Name);
    }
}

////////////////////////////////////////////////////////////////////////////////

TLockRequest::TLockRequest()
{ }

TLockRequest::TLockRequest(ELockMode mode)
    : Mode(mode)
{ }

TLockRequest TLockRequest::MakeSharedChild(const TString& key)
{
    TLockRequest result(ELockMode::Shared);
    result.Key.Kind = ELockKeyKind::Child;
    result.Key.Name = key;
    return result;
}

TLockRequest TLockRequest::MakeSharedAttribute(const TString& key)
{
    TLockRequest result(ELockMode::Shared);
    result.Key.Kind = ELockKeyKind::Attribute;
    result.Key.Name = key;
    return result;
}

void TLockRequest::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Mode);
    Persist(context, Key);
}

bool TLockRequest::operator==(const TLockRequest& other) const
{
    return Mode == other.Mode && Key == other.Key;
}

bool TLockRequest::operator!=(const TLockRequest& other) const
{
    return !(*this == other);
}

////////////////////////////////////////////////////////////////////////////////

bool TCypressNodeLockingState::IsEmpty() const
{
    return
        AcquiredLocks.empty() &&
        PendingLocks.empty() &&
        ExclusiveLocks.empty() &&
        SharedLocks.empty() &&
        SnapshotLocks.empty();
}

void TCypressNodeLockingState::Persist(TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, AcquiredLocks);
    Persist(context, PendingLocks);
    Persist(context, ExclusiveLocks);
    Persist(context, SharedLocks);
    Persist(context, SnapshotLocks);
}

TCypressNodeLockingState TCypressNodeLockingState::Empty;

////////////////////////////////////////////////////////////////////////////////

TLock::TLock(const TLockId& id)
    : TNonversionedObjectBase(id)
    , State_(ELockState::Pending)
{ }

void TLock::Save(TSaveContext& context) const
{
    TNonversionedObjectBase::Save(context);

    using NYT::Save;
    Save(context, Implicit_);
    Save(context, State_);
    Save(context, Request_);
    TNonversionedObjectRefSerializer::Save(context, TrunkNode_);
    Save(context, Transaction_);
}

void TLock::Load(NCellMaster::TLoadContext& context)
{
    TNonversionedObjectBase::Load(context);

    using NYT::Load;
    Load(context, Implicit_);
    Load(context, State_);
    Load(context, Request_);
    TNonversionedObjectRefSerializer::Load(context, TrunkNode_);
    Load(context, Transaction_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT

