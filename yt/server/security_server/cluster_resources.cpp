#include "cluster_resources.h"

#include <yt/server/cell_master/serialize.h>

#include <yt/server/security_server/security_manager.pb.h>

#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NSecurityServer {

using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TClusterResources::TClusterResources()
    : DiskSpace(0)
    , NodeCount(0)
    , ChunkCount(0)
{ }

TClusterResources::TClusterResources(
    i64 diskSpace,
    int nodeCount,
    int chunkCount)
    : DiskSpace(diskSpace)
    , NodeCount(nodeCount)
    , ChunkCount(chunkCount)
{ }

void TClusterResources::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, DiskSpace);
    Save(context, NodeCount);
    Save(context, ChunkCount);
}

void TClusterResources::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    Load(context, DiskSpace);
    Load(context, NodeCount);
    Load(context, ChunkCount);
}

////////////////////////////////////////////////////////////////////////////////

void ToProto(NProto::TClusterResources* protoResources, const TClusterResources& resources)
{
    protoResources->set_disk_space(resources.DiskSpace);
    protoResources->set_chunk_count(resources.ChunkCount);
    protoResources->set_node_count(resources.NodeCount);
}

void FromProto(TClusterResources* resources, const NProto::TClusterResources& protoResources)
{
    resources->DiskSpace = protoResources.disk_space();
    resources->ChunkCount = protoResources.chunk_count();
    resources->NodeCount = protoResources.node_count();
}

////////////////////////////////////////////////////////////////////////////////

//! A serialization-enabling wrapper around TClusterResources.
struct TSerializableClusterAttributes
    : public TClusterResources
    , public TYsonSerializableLite
{
    TSerializableClusterAttributes()
    {
        RegisterParameter("disk_space", DiskSpace)
            .GreaterThanOrEqual(0);
        RegisterParameter("node_count", NodeCount)
            .GreaterThanOrEqual(0);
        RegisterParameter("chunk_count", ChunkCount)
            .GreaterThanOrEqual(0);
    }
};

void Serialize(const TClusterResources& resources, IYsonConsumer* consumer)
{
    TSerializableClusterAttributes wrapper;
    static_cast<TClusterResources&>(wrapper) = resources;
    Serialize(static_cast<const TYsonSerializableLite&>(wrapper), consumer);
}

void Deserialize(TClusterResources& resources, INodePtr node)
{
    TSerializableClusterAttributes wrapper;
    Deserialize(static_cast<TYsonSerializableLite&>(wrapper), node);
    // TODO(babenko): we shouldn't be concerned with manual validation here
    wrapper.Validate();
    resources = static_cast<TClusterResources&>(wrapper);
}

////////////////////////////////////////////////////////////////////////////////

TClusterResources& operator += (TClusterResources& lhs, const TClusterResources& rhs)
{
    lhs.DiskSpace += rhs.DiskSpace;
    lhs.NodeCount += rhs.NodeCount;
    lhs.ChunkCount += rhs.ChunkCount;
    return lhs;
}

TClusterResources operator + (const TClusterResources& lhs, const TClusterResources& rhs)
{
    auto result = lhs;
    result += rhs;
    return result;
}

TClusterResources& operator -= (TClusterResources& lhs, const TClusterResources& rhs)
{
    lhs.DiskSpace -= rhs.DiskSpace;
    lhs.NodeCount -= rhs.NodeCount;
    lhs.ChunkCount -= rhs.ChunkCount;
    return lhs;
}

TClusterResources operator - (const TClusterResources& lhs, const TClusterResources& rhs)
{
    auto result = lhs;
    result -= rhs;
    return result;
}

TClusterResources& operator *= (TClusterResources& lhs, i64 rhs)
{
    lhs.DiskSpace *= rhs;
    lhs.NodeCount *= rhs;
    lhs.ChunkCount *= rhs;
    return lhs;
}

TClusterResources operator * (const TClusterResources& lhs, i64 rhs)
{
    auto result = lhs;
    result *= rhs;
    return result;
}

TClusterResources operator -  (const TClusterResources& resources)
{
    TClusterResources result;
    result.DiskSpace = -resources.DiskSpace;
    result.NodeCount = -resources.NodeCount;
    result.ChunkCount = -resources.ChunkCount;
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityServer
} // namespace NYT

