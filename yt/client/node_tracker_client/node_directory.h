#pragma once

#include "public.h"

#include <yt/client/chunk_client/chunk_replica.h>

#include <yt/core/concurrency/rw_spinlock.h>

#include <yt/core/rpc/helpers.h>

#include <yt/core/yson/public.h>

#include <yt/core/misc/enum.h>
#include <yt/core/misc/optional.h>
#include <yt/core/misc/property.h>

namespace NYT::NNodeTrackerClient {

////////////////////////////////////////////////////////////////////////////////

//! Network-related node information.
class TNodeDescriptor
{
public:
    TNodeDescriptor();
    explicit TNodeDescriptor(const TString& defaultAddress);
    explicit TNodeDescriptor(const std::optional<TString>& defaultAddress);
    explicit TNodeDescriptor(
        TAddressMap addresses,
        std::optional<TString> rack = std::nullopt,
        std::optional<TString> dc = std::nullopt,
        const std::vector<TString>& tags = {});

    bool IsNull() const;

    const TAddressMap& Addresses() const;

    const TString& GetDefaultAddress() const;

    const TString& GetAddressOrThrow(const TNetworkPreferenceList& networks) const;
    NRpc::TAddressWithNetwork GetAddressWithNetworkOrThrow(const TNetworkPreferenceList& networks) const;

    std::optional<TString> FindAddress(const TNetworkPreferenceList& networks) const;

    const std::optional<TString>& GetRack() const;
    const std::optional<TString>& GetDataCenter() const;

    const std::vector<TString>& GetTags() const;

    void Persist(const TStreamPersistenceContext& context);

private:
    TAddressMap Addresses_;
    TString DefaultAddress_;
    std::optional<TString> Rack_;
    std::optional<TString> DataCenter_;
    std::vector<TString> Tags_;
};

const TString& NullNodeAddress();
const TNodeDescriptor& NullNodeDescriptor();

////////////////////////////////////////////////////////////////////////////////

bool operator == (const TNodeDescriptor& lhs, const TNodeDescriptor& rhs);
bool operator != (const TNodeDescriptor& lhs, const TNodeDescriptor& rhs);

bool operator == (const TNodeDescriptor& lhs, const NProto::TNodeDescriptor& rhs);
bool operator != (const TNodeDescriptor& lhs, const NProto::TNodeDescriptor& rhs);

void FormatValue(TStringBuilderBase* builder, const TNodeDescriptor& descriptor, TStringBuf spec);
TString ToString(const TNodeDescriptor& descriptor);

// Accessors for some well-known addresses.
const TString& GetDefaultAddress(const TAddressMap& addresses);
const TString& GetDefaultAddress(const NProto::TAddressMap& addresses);

NRpc::TAddressWithNetwork GetAddressWithNetworkOrThrow(const TAddressMap& addresses, const TNetworkPreferenceList& networks);
const TString& GetAddressOrThrow(const TAddressMap& addresses, const TNetworkPreferenceList& networks);
std::optional<TString> FindAddress(const TAddressMap& addresses, const TNetworkPreferenceList& networks);

const TAddressMap& GetAddressesOrThrow(const TNodeAddressMap& nodeAddresses, EAddressType type);

//! Please keep the items in this particular order: the further the better.
DEFINE_ENUM(EAddressLocality,
    (None)
    (SameDataCenter)
    (SameRack)
    (SameHost)
);

EAddressLocality ComputeAddressLocality(const TNodeDescriptor& first, const TNodeDescriptor& second);

namespace NProto {

void ToProto(NNodeTrackerClient::NProto::TAddressMap* protoAddresses, const NNodeTrackerClient::TAddressMap& addresses);
void FromProto(NNodeTrackerClient::TAddressMap* addresses, const NNodeTrackerClient::NProto::TAddressMap& protoAddresses);

void ToProto(NNodeTrackerClient::NProto::TNodeAddressMap* proto, const NNodeTrackerClient::TNodeAddressMap& nodeAddresses);
void FromProto(NNodeTrackerClient::TNodeAddressMap* nodeAddresses, const NNodeTrackerClient::NProto::TNodeAddressMap& proto);

void ToProto(NNodeTrackerClient::NProto::TNodeDescriptor* protoDescriptor, const NNodeTrackerClient::TNodeDescriptor& descriptor);
void FromProto(NNodeTrackerClient::TNodeDescriptor* descriptor, const NNodeTrackerClient::NProto::TNodeDescriptor& protoDescriptor);

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

//! Caches node descriptors obtained by fetch requests.
/*!
 *  \note
 *  Thread affinity: thread-safe
 */
class TNodeDirectory
    : public TIntrinsicRefCounted
{
public:
    void MergeFrom(const NProto::TNodeDirectory& source);
    void MergeFrom(const TNodeDirectoryPtr& source);
    void DumpTo(NProto::TNodeDirectory* destination);
    void Serialize(NYson::IYsonConsumer* consumer) const;

    void AddDescriptor(TNodeId id, const TNodeDescriptor& descriptor);

    const TNodeDescriptor* FindDescriptor(TNodeId id) const;
    const TNodeDescriptor& GetDescriptor(TNodeId id) const;
    const TNodeDescriptor& GetDescriptor(NChunkClient::TChunkReplica replica) const;
    std::vector<TNodeDescriptor> GetDescriptors(const NChunkClient::TChunkReplicaList& replicas) const;
    std::vector<std::pair<NNodeTrackerClient::TNodeId, TNodeDescriptor>> GetAllDescriptors() const;

    const TNodeDescriptor* FindDescriptor(const TString& address);
    const TNodeDescriptor& GetDescriptor(const TString& address);

    void Save(TStreamSaveContext& context) const;
    void Load(TStreamLoadContext& context);

private:
    mutable NConcurrency::TReaderWriterSpinLock SpinLock_;
    THashMap<TNodeId, const TNodeDescriptor*> IdToDescriptor_;
    THashMap<TString, const TNodeDescriptor*> AddressToDescriptor_;
    std::vector<std::unique_ptr<TNodeDescriptor>> Descriptors_;

    void DoAddDescriptor(TNodeId id, const TNodeDescriptor& descriptor);
    void DoAddDescriptor(TNodeId id, const NProto::TNodeDescriptor& protoDescriptor);
    void DoAddCapturedDescriptor(TNodeId id, std::unique_ptr<TNodeDescriptor> descriptorHolder);

};

void Serialize(const TNodeDirectory& nodeDirectory, NYson::IYsonConsumer* consumer);

DEFINE_REFCOUNTED_TYPE(TNodeDirectory)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NNodeTrackerClient

////////////////////////////////////////////////////////////////////////////////

template <>
struct THash<NYT::NNodeTrackerClient::TNodeDescriptor>
{
    size_t operator()(const NYT::NNodeTrackerClient::TNodeDescriptor& value) const;
};

////////////////////////////////////////////////////////////////////////////////
