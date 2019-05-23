#include "pod.h"
#include "pod_set.h"
#include "node.h"
#include "node_segment.h"
#include "account.h"
#include "db_schema.h"

#include <yt/core/misc/protobuf_helpers.h>

namespace NYP::NServer::NObjects {

using NYT::ToProto;
using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

const TScalarAttributeSchema<TPod, EPodCurrentState> TPod::TStatus::TAgent::StateSchema{
    &PodsTable.Fields.Status_Agent_State,
    [] (TPod* pod) { return &pod->Status().Agent().State(); }
};

const TScalarAttributeSchema<TPod, TString> TPod::TStatus::TAgent::IssPayloadSchema{
    &PodsTable.Fields.Status_Agent_IssPayload,
    [] (TPod* pod) { return &pod->Status().Agent().IssPayload(); }
};

const TScalarAttributeSchema<TPod, TPod::TStatus::TAgent::TPodAgentPayload> TPod::TStatus::TAgent::PodAgentPayloadSchema{
    &PodsTable.Fields.Status_Agent_PodAgentPayload,
    [] (TPod* pod) { return &pod->Status().Agent().PodAgentPayload(); }
};

const TScalarAttributeSchema<TPod, TPod::TStatus::TDynamicResourceStatus> TPod::TStatus::DynamicResourcesSchema{
    &PodsTable.Fields.Status_DynamicResources,
    [] (TPod* pod) { return &pod->Status().DynamicResources(); }
};

const TScalarAttributeSchema<TPod, TPod::TStatus::TAgent::TEtc> TPod::TStatus::TAgent::EtcSchema{
    &PodsTable.Fields.Status_Agent_Etc,
    [] (TPod* pod) { return &pod->Status().Agent().Etc(); }
};

TPod::TStatus::TStatus::TAgent::TAgent(TPod* pod)
    : State_(pod, &StateSchema)
    , IssPayload_(pod, &IssPayloadSchema)
    , PodAgentPayload_(pod, &PodAgentPayloadSchema)
    , Etc_(pod, &EtcSchema)
{ }

////////////////////////////////////////////////////////////////////////////////

const TScalarAttributeSchema<TPod, ui64> TPod::TStatus::GenerationNumberSchema{
    &PodsTable.Fields.Status_GenerationNumber,
    [] (TPod* pod) { return &pod->Status().GenerationNumber(); }
};

const TScalarAttributeSchema<TPod, NTransactionClient::TTimestamp> TPod::TStatus::AgentSpecTimestampSchema{
    &PodsTable.Fields.Status_AgentSpecTimestamp,
    [] (TPod* pod) { return &pod->Status().AgentSpecTimestamp(); }
};

const TScalarAttributeSchema<TPod, TPod::TStatus::TEtc> TPod::TStatus::EtcSchema{
    &PodsTable.Fields.Status_Etc,
    [] (TPod* pod) { return &pod->Status().Etc(); }
};

TPod::TStatus::TStatus(TPod* pod)
    : Agent_(pod)
    , GenerationNumber_(pod, &GenerationNumberSchema)
    , AgentSpecTimestamp_(pod, &AgentSpecTimestampSchema)
    , DynamicResources_(pod, &DynamicResourcesSchema)
    , Etc_(pod, &EtcSchema)
{ }

////////////////////////////////////////////////////////////////////////////////

const TManyToOneAttributeSchema<TPod, TNode> TPod::TSpec::NodeSchema{
    &PodsTable.Fields.Spec_NodeId,
    [] (TPod* pod) { return &pod->Spec().Node(); },
    [] (TNode* node) { return &node->Pods(); }
};

const TScalarAttributeSchema<TPod, TString> TPod::TSpec::IssPayloadSchema{
    &PodsTable.Fields.Spec_IssPayload,
    [] (TPod* pod) { return &pod->Spec().IssPayload(); }
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TPodAgentPayload> TPod::TSpec::PodAgentPayloadSchema{
    &PodsTable.Fields.Spec_PodAgentPayload,
    [] (TPod* pod) { return &pod->Spec().PodAgentPayload(); }
};

const TScalarAttributeSchema<TPod, bool> TPod::TSpec::EnableSchedulingSchema{
    &PodsTable.Fields.Spec_EnableScheduling,
    [] (TPod* pod) { return &pod->Spec().EnableScheduling(); }
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TSecrets> TPod::TSpec::SecretsSchema{
    &PodsTable.Fields.Spec_Secrets,
    [] (TPod* pod) { return &pod->Spec().Secrets(); }
};

const TTimestampAttributeSchema TPod::TSpec::UpdateTimestampSchema{
    &PodsTable.Fields.Spec_UpdateTag
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TDynamicResourceSpec> TPod::TSpec::DynamicResourcesSchema{
    &PodsTable.Fields.Spec_DynamicResources,
    [] (TPod* pod) { return &pod->Spec().DynamicResources(); }
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TResourceCache> TPod::TSpec::ResourceCacheSchema{
    &PodsTable.Fields.Spec_ResourceCache,
    [] (TPod* pod) { return &pod->Spec().ResourceCache(); }
};

const TManyToOneAttributeSchema<TPod, TAccount> TPod::TSpec::AccountSchema{
    &PodsTable.Fields.Spec_AccountId,
    [] (TPod* pod) { return &pod->Spec().Account(); },
    [] (TAccount* account) { return &account->Pods(); }
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TDynamicAttributes> TPod::TSpec::DynamicAttributesSchema{
    &PodsTable.Fields.Spec_DynamicAttributes,
    [] (TPod* pod) { return &pod->Spec().DynamicAttributes(); }
};

const TScalarAttributeSchema<TPod, TPod::TSpec::TEtc> TPod::TSpec::EtcSchema{
    &PodsTable.Fields.Spec_Etc,
    [] (TPod* pod) { return &pod->Spec().Etc(); }
};

TPod::TSpec::TSpec(TPod* pod)
    : Node_(pod, &NodeSchema)
    , IssPayload_(pod, &IssPayloadSchema)
    , PodAgentPayload_(pod, &PodAgentPayloadSchema)
    , EnableScheduling_(pod, &EnableSchedulingSchema)
    , Secrets_(pod, &SecretsSchema)
    , UpdateTimestamp_(pod, &UpdateTimestampSchema)
    , DynamicResources_(pod, &DynamicResourcesSchema)
    , ResourceCache_(pod, &ResourceCacheSchema)
    , Account_(pod, &AccountSchema)
    , DynamicAttributes_(pod, &DynamicAttributesSchema)
    , Etc_(pod, &EtcSchema)
{ }

////////////////////////////////////////////////////////////////////////////////

TPod::TPod(
    const TObjectId& id,
    const TObjectId& podSetId,
    IObjectTypeHandler* typeHandler,
    ISession* session)
    : TObject(id, podSetId, typeHandler, session)
    , PodSet_(this)
    , Status_(this)
    , Spec_(this)
{ }

EObjectType TPod::GetType() const
{
    return EObjectType::Pod;
}

void TPod::UpdateEvictionStatus(
    EEvictionState state,
    EEvictionReason reason,
    const TString& message)
{
    auto* eviction = Status().Etc()->mutable_eviction();
    eviction->set_state(static_cast<NClient::NApi::NProto::EEvictionState>(state));
    eviction->set_reason(static_cast<NClient::NApi::NProto::EEvictionReason>(reason));
    eviction->set_message(message);
    eviction->set_last_updated(ToProto<ui64>(TInstant::Now()));
}

void TPod::UpdateSchedulingStatus(
    ESchedulingState state,
    const TString& message,
    const TObjectId& nodeId)
{
    auto* scheduling = Status().Etc()->mutable_scheduling();
    scheduling->set_state(static_cast<NClient::NApi::NProto::ESchedulingState>(state));
    scheduling->set_message(message);
    if (nodeId) {
        scheduling->set_node_id(nodeId);
    } else {
        scheduling->clear_node_id();
    }
    scheduling->set_last_updated(ToProto<ui64>(TInstant::Now()));
    scheduling->clear_error();
}

////////////////////////////////////////////////////////////////////////////////

// Cf. YP-626
bool IsUnsafePortoIssSpec(const NClient::NApi::NClusterApiProto::HostConfiguration& issSpec)
{
    auto findPropertyValue = [&] (const auto& map, const auto& key) {
        auto it = map.find(key);
        return it == map.end() ? TStringBuf() : TStringBuf(it->second);
    };

    for (const auto& instance : issSpec.instances()) {
        if (instance.GettargetState() == "REMOVED") {
            continue;
        }
        if (!instance.entity().has_instance()) {
            continue;
        }
        const auto& entityInstance = instance.entity().instance();

        // Check rbinds.
        for (const auto& volume : entityInstance.volumes()) {
            auto bindValue = findPropertyValue(volume.properties(), "bind");
            if (bindValue && bindValue != "/usr/local/yasmagent /usr/local/yasmagent ro") {
                return true;
            }

            auto backendValue = findPropertyValue(volume.properties(), "backend");
            auto readOnlyValue = findPropertyValue(volume.properties(), "read_only");
            auto storageValue = findPropertyValue(volume.properties(), "storage");
            if ((backendValue == "bind" || backendValue == "rbind") &&
                (readOnlyValue != "true" || storageValue != "/Berkanavt/supervisor"))
            {
                return true;
            }
        }

        // Check constraints.
        {
            const auto& constraints = entityInstance.container().constraints();
            auto it = constraints.find("meta.enable_porto");
            if (it == constraints.end()) {
                // "full" is the default
                return true;
            }
            const auto& value = it->second;
            if (value != "false" && value != "none" && value != "read-isolate" && value != "isolate") {
                return true;
            }
        }

    }
    return false;
}

void ValidateIssPodSpecSafe(TPod* pod)
{
    const auto& issPayload = pod->Spec().IssPayload().Load();
    if (!issPayload) {
        return;
    }
    NClient::NApi::NClusterApiProto::HostConfiguration issSpec;
    if (!TryDeserializeProto(&issSpec, TRef::FromString(issPayload))) {
        THROW_ERROR_EXCEPTION("Error parsing /spec/iss_payload of pod %Qv",
            pod->GetId());
    }
    auto* podSet = pod->PodSet().Load();
    auto* nodeSegment = podSet->Spec().NodeSegment().Load();
    if (IsUnsafePortoIssSpec(issSpec) && !nodeSegment->Spec().Load().enable_unsafe_porto()) {
        THROW_ERROR_EXCEPTION("/spec/iss_payload of pod %Qv involves unsafe features; such pods cannot be allocated in %Qv segment since "
            "/spec/enable_unsafe_porto is \"false\"",
            pod->GetId(),
            nodeSegment->GetId());
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NObjects

