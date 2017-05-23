#include "input_stream.h"

namespace NYT {
namespace NChunkPools {

////////////////////////////////////////////////////////////////////

TInputStreamDescriptor::TInputStreamDescriptor(bool isTeleportable, bool isPrimary, bool isVersioned)
    : IsTeleportable_(isTeleportable)
    , IsPrimary_(isPrimary)
    , IsVersioned_(isVersioned)
{ }

bool TInputStreamDescriptor::IsTeleportable() const
{
    return IsTeleportable_;
}

bool TInputStreamDescriptor::IsForeign() const
{
    return !IsPrimary_;
}

bool TInputStreamDescriptor::IsPrimary() const
{
    return IsPrimary_;
}

bool TInputStreamDescriptor::IsVersioned() const
{
    return IsVersioned_;
}

bool TInputStreamDescriptor::IsUnversioned() const
{
    return !IsVersioned_;
}

void TInputStreamDescriptor::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, IsTeleportable_);
    Persist(context, IsPrimary_);
    Persist(context, IsVersioned_);
}

////////////////////////////////////////////////////////////////////

TInputStreamDescriptor IntermediateInputStreamDescriptor(false /* isTeleportable */, true /* isPrimary */, false /* isVersioned */);

////////////////////////////////////////////////////////////////////

TInputStreamDirectory::TInputStreamDirectory(
    std::vector<TInputStreamDescriptor> descriptors,
    TInputStreamDescriptor defaultDescriptor)
    : Descriptors_(std::move(descriptors))
    , DefaultDescriptor_(defaultDescriptor)
{
    YCHECK(DefaultDescriptor_.IsPrimary());
}

const TInputStreamDescriptor& TInputStreamDirectory::GetDescriptor(int inputStreamIndex) const
{
    if (0 <= inputStreamIndex && inputStreamIndex < static_cast<int>(Descriptors_.size())) {
        return Descriptors_[inputStreamIndex];
    } else {
        return DefaultDescriptor_;
    }
}

int TInputStreamDirectory::GetDescriptorCount() const
{
    return Descriptors_.size();
}

void TInputStreamDirectory::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Descriptors_);
    Persist(context, DefaultDescriptor_);
}

////////////////////////////////////////////////////////////////////

TInputStreamDirectory IntermediateInputStreamDirectory({}, IntermediateInputStreamDescriptor);

////////////////////////////////////////////////////////////////////

} // namespace NChunkPools
} // namespace NYT