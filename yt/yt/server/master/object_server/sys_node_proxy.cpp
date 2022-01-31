#include "sys_node_proxy.h"
#include "private.h"

#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/server/master/cypress_server/node_proxy_detail.h>

#include <yt/yt/server/lib/hydra_common/hydra_manager.h>

#include <yt/yt/server/master/chunk_server/chunk_manager.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/server/master/cell_master/alert_manager.h>
#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/config_manager.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>
#include <yt/yt/server/master/cell_master/multicell_manager.h>

#include <yt/yt/ytlib/api/native/config.h>

namespace NYT::NObjectServer {

using namespace NYson;
using namespace NYTree;
using namespace NCypressServer;
using namespace NTransactionServer;
using namespace NCellMaster;

using NApi::NNative::TConnectionConfig;

////////////////////////////////////////////////////////////////////////////////

class TSysNodeProxy
    : public TMapNodeProxy
{
public:
    using TMapNodeProxy::TMapNodeProxy;

private:
    using TBase = TMapNodeProxy;

    void ListSystemAttributes(std::vector<TAttributeDescriptor>* descriptors) override
    {
        TBase::ListSystemAttributes(descriptors);

        descriptors->push_back(EInternedAttributeKey::CellTag);
        descriptors->push_back(EInternedAttributeKey::PrimaryCellTag);
        descriptors->push_back(EInternedAttributeKey::CellId);
        descriptors->push_back(EInternedAttributeKey::PrimaryCellId);
        descriptors->push_back(EInternedAttributeKey::CurrentCommitRevision);
        descriptors->push_back(EInternedAttributeKey::ChunkReplicatorEnabled);
        descriptors->push_back(EInternedAttributeKey::ChunkRefreshEnabled);
        descriptors->push_back(EInternedAttributeKey::ChunkRequisitionUpdateEnabled);
        descriptors->push_back(EInternedAttributeKey::RegisteredMasterCellTags);
        descriptors->push_back(EInternedAttributeKey::MasterAlerts);
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::Config)
            .SetWritable(true)
            .SetOpaque(true));
        descriptors->push_back(EInternedAttributeKey::HydraReadOnly);
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::ClusterName)
            .SetWritable(true)
            .SetCustom(true));
        descriptors->push_back(TAttributeDescriptor(EInternedAttributeKey::ClusterConnection)
            .SetWritable(true)
            .SetCustom(true));
    }

    static void ValidateClusterName(const TString& clusterName)
    {
        if (clusterName.size() > MaxClusterNameLength) {
            THROW_ERROR_EXCEPTION("Cluster name is too long")
                << TErrorAttribute("cluster_name_length", clusterName.size())
                << TErrorAttribute("max_cluster_name_length", MaxClusterNameLength);
        }

        auto isAsciiText = [] (char c) {
            return IsAsciiAlnum(c) || IsAsciiSpace(c) || IsAsciiPunct(c);
        };

        if (!::AllOf(clusterName.cbegin(), clusterName.cend(), isAsciiText)) {
            THROW_ERROR_EXCEPTION("Only ASCII alphanumeric, white-space and punctuation characters are allowed in cluster names");
        }
    }

    void ValidateCustomAttributeUpdate(
        const TString& key,
        const TYsonString& oldValue,
        const TYsonString& newValue) override
    {
        auto internedKey = TInternedAttributeKey::Lookup(key);

        switch (internedKey) {
            case EInternedAttributeKey::ClusterName:
                ValidateClusterName(ConvertTo<TString>(newValue));
                return;

            case EInternedAttributeKey::ClusterConnection: {
                auto node = ConvertToNode(newValue);
                if (node->GetType() != ENodeType::Entity) {
                    New<TConnectionConfig>()->Load(node);
                }
                return;
            }

            default:
                break;
        }

        return TBase::ValidateCustomAttributeUpdate(key, oldValue, newValue);
    }

    bool GetBuiltinAttribute(TInternedAttributeKey key, IYsonConsumer* consumer) override
    {
        const auto& multicellManager = Bootstrap_->GetMulticellManager();
        const auto& hydraManager = Bootstrap_->GetHydraFacade()->GetHydraManager();
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto& configManager = Bootstrap_->GetConfigManager();
        const auto& alertManager = Bootstrap_->GetAlertManager();

        switch (key) {
            case EInternedAttributeKey::CellTag:
                BuildYsonFluently(consumer)
                    .Value(multicellManager->GetCellTag());
                return true;

            case EInternedAttributeKey::PrimaryCellTag:
                BuildYsonFluently(consumer)
                    .Value(multicellManager->GetPrimaryCellTag());
                return true;

            case EInternedAttributeKey::CellId:
                BuildYsonFluently(consumer)
                    .Value(multicellManager->GetCellId());
                return true;

            case EInternedAttributeKey::PrimaryCellId:
                BuildYsonFluently(consumer)
                    .Value(multicellManager->GetPrimaryCellId());
                return true;

            case EInternedAttributeKey::CurrentCommitRevision:
                BuildYsonFluently(consumer)
                    .Value(hydraManager->GetAutomatonVersion().ToRevision());
                return true;

            case EInternedAttributeKey::ChunkReplicatorEnabled:
                RequireLeader();
                BuildYsonFluently(consumer)
                    .Value(chunkManager->IsChunkReplicatorEnabled());
                return true;

            case EInternedAttributeKey::ChunkRefreshEnabled:
                RequireLeader();
                BuildYsonFluently(consumer)
                    .Value(chunkManager->IsChunkRefreshEnabled());
                return true;

            case EInternedAttributeKey::ChunkRequisitionUpdateEnabled:
                RequireLeader();
                BuildYsonFluently(consumer)
                    .Value(chunkManager->IsChunkRequisitionUpdateEnabled());
                return true;

            case EInternedAttributeKey::ChunkSealerEnabled:
                RequireLeader();
                BuildYsonFluently(consumer)
                    .Value(chunkManager->IsChunkSealerEnabled());
                return true;

            case EInternedAttributeKey::RegisteredMasterCellTags:
                BuildYsonFluently(consumer)
                    .Value(multicellManager->GetRegisteredMasterCellTags());
                return true;

            case EInternedAttributeKey::Config:
                BuildYsonFluently(consumer)
                    .Value(configManager->GetConfig());
                return true;

            case EInternedAttributeKey::HydraReadOnly:
                BuildYsonFluently(consumer)
                    .Value(hydraManager->GetReadOnly());
                return true;

            case EInternedAttributeKey::MasterAlerts:
                BuildYsonFluently(consumer)
                    .Value(alertManager->GetAlerts());
                return true;

            default:
                break;
        }

        return TBase::GetBuiltinAttribute(key, consumer);
    }

    bool SetBuiltinAttribute(TInternedAttributeKey key, const TYsonString& value) override
    {
        switch (key) {
            case EInternedAttributeKey::Config: {
                ValidatePermission(EPermissionCheckScope::This, EPermission::Write);
                const auto& configManager = Bootstrap_->GetConfigManager();
                configManager->SetConfig(ConvertTo<INodePtr>(value));
                return true;
            }

            default:
                break;
        }

        return TBase::SetBuiltinAttribute(key, value);
    }
};

////////////////////////////////////////////////////////////////////////////////

ICypressNodeProxyPtr CreateSysNodeProxy(
    TBootstrap* bootstrap,
    TObjectTypeMetadata* metadata,
    TTransaction* transaction,
    TMapNode* trunkNode)
{
    return New<TSysNodeProxy>(
        bootstrap,
        metadata,
        transaction,
        trunkNode);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectServer
