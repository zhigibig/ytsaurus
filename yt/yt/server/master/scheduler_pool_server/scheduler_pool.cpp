#include "private.h"

#include "scheduler_pool.h"

#include "scheduler_pool_manager.h"

#include <yt/server/master/cell_master/bootstrap.h>

namespace NYT::NSchedulerPoolServer {

using namespace NCellMaster;
using namespace NObjectServer;
using namespace NScheduler;
using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

static const NLogging::TLogger& Logger = SchedulerPoolServerLogger;

////////////////////////////////////////////////////////////////////////////////

INodePtr ConvertToNode(const THashMap<TInternedAttributeKey, TYsonString>& attributes)
{
    return BuildYsonNodeFluently()
        .DoMapFor(attributes, [] (TFluentMap fluent, const auto& pair) {
            fluent.Item(pair.first.Unintern()).Value(pair.second);
        });
}

////////////////////////////////////////////////////////////////////////////////

TSchedulerPool::TSchedulerPool(TObjectId id, bool isRoot)
    : TBase(id, isRoot)
    , FullConfig_(New<NScheduler::TPoolConfig>())
{ }

TString TSchedulerPool::GetLowercaseObjectName() const
{
    return IsRoot()
        ? MaybePoolTree_->GetLowercaseObjectName()
        : Format("scheduler pool %Qv", GetName());
}

TString TSchedulerPool::GetCapitalizedObjectName() const
{
    return IsRoot()
        ? MaybePoolTree_->GetCapitalizedObjectName()
        : Format("Scheduler pool %Qv", GetName());
}

void TSchedulerPool::ValidateAll()
{
    FullConfig_->Validate();
    ValidateChildrenCompatibility();
    GetParent()->ValidateChildrenCompatibility();
}

void TSchedulerPool::ValidateChildrenCompatibility()
{
    if (IsRoot()) {
        return;
    }

    // TODO(renadeen): move children validation to pool config?
    FullConfig()->MinShareResources->ForEachResource([this] (auto TResourceLimitsConfig::* resourceDataMember, const TString& name) {
        using TResource = typename std::remove_reference_t<decltype(std::declval<TResourceLimitsConfig>().*resourceDataMember)>::value_type;
        auto getResource = [&] (TSchedulerPool* object) -> TResource {
            return (object->FullConfig()->MinShareResources.Get()->*resourceDataMember).value_or(0);
        };

        auto parentResource = getResource(this);
        TResource childrenResourceSum = 0;
        for (const auto& [_, child] : KeyToChild_) {
            childrenResourceSum += getResource(child);
        }

        if (parentResource < childrenResourceSum) {
            THROW_ERROR_EXCEPTION("Guarantee of resource for pool %Qv is less than the sum of children guarantees",  GetName())
                << TErrorAttribute("resource_name", name)
                << TErrorAttribute("pool_name", GetName())
                << TErrorAttribute("parent_resource_value", parentResource)
                << TErrorAttribute("children_resource_sum", childrenResourceSum);
        }
    });

    if (!KeyToChild().empty() && FullConfig()->Mode == NScheduler::ESchedulingMode::Fifo) {
        THROW_ERROR_EXCEPTION("Pool %Qv cannot have subpools since it is in FIFO mode")
            << TErrorAttribute("pool_name", GetName());
    }
}

void TSchedulerPool::Save(NCellMaster::TSaveContext& context) const
{
    TBase::Save(context);

    using NYT::Save;

    Save(context, SpecifiedAttributes_);
    Save(context, MaybePoolTree_);
}

void TSchedulerPool::Load(NCellMaster::TLoadContext& context)
{
    TBase::Load(context);

    using NYT::Load;

    // COMPAT(shakurov)
    if (context.GetVersion() < EMasterReign::SpecifiedAttributeFix) {
        auto oldSpecifiedAttributes = Load<THashMap<int, NYson::TYsonString>>(context);
        SpecifiedAttributes_.reserve(oldSpecifiedAttributes.size());
        for (auto& [k, v] : oldSpecifiedAttributes) {
            YT_VERIFY(SpecifiedAttributes_.emplace(TInternedAttributeKey(k), std::move(v)).second);
        }
    } else {
        Load(context, SpecifiedAttributes_);
    }

    Load(context, MaybePoolTree_);

    FullConfig_->Load(ConvertToNode(SpecifiedAttributes_));

    // COMPAT(mrkastep)
    // NB(mrkastep): Since we remove the attribute from Attributes_ field, this change is idempotent i.e. can be
    // safely re-applied to snapshots after upgrading masters to a new major version.
    if (context.GetVersion() < EMasterReign::InternalizeAbcSchedulerPoolAttribute) {
        static const auto abcAttributeName = "abc";
        if (auto abc = FindAttribute(abcAttributeName)) {
            auto value = std::move(*abc);
            YT_VERIFY(Attributes_->Remove(abcAttributeName));
            try {
                FullConfig_->LoadParameter(abcAttributeName, NYTree::ConvertToNode(value), EMergeStrategy::Overwrite);
                YT_VERIFY(SpecifiedAttributes_.emplace(TInternedAttributeKey::Lookup(abcAttributeName), std::move(value)).second);
            } catch (const std::exception& e) {
                // Since we make this attribute well-known, the error needs to be logged and subsequently fixed.
                YT_LOG_ERROR(e, "Cannot parse %Qv as %Qv attribute of pool %Qv", value, abcAttributeName, GetName());
            }
        }
    }

    if (context.GetVersion() != NCellMaster::GetCurrentReign() && Attributes_) {
        const auto& schedulerPoolManager = context.GetBootstrap()->GetSchedulerPoolManager();
        for (const auto& [key, value] : Attributes_->Attributes()) {
            auto internedKey = TInternedAttributeKey::Lookup(key);
            if (internedKey == InvalidInternedAttribute) {
                continue;
            }
            if (schedulerPoolManager->GetKnownPoolAttributes().contains(internedKey)) {
                if (SpecifiedAttributes_.contains(internedKey)) {
                    YT_LOG_ERROR("Found pool attribute that is stored in both SpecifiedAttributes map and common attributes map "
                        "(ObjectId: %v, AttributeName: %v, CommonAttributeValue: %v, SpecifiedAttributeValue: %v)",
                        Id_,
                        key,
                        value,
                        SpecifiedAttributes_[internedKey]);
                } else {
                    try {
                        YT_LOG_INFO("Moving pool attribute from common attributes map to SpecifiedAttributes map "
                            "(ObjectId: %v, AttributeName: %v, AttributeValue: %v)",
                            Id_,
                            key,
                            value);
                        FullConfig_->LoadParameter(key, NYTree::ConvertToNode(value), EMergeStrategy::Overwrite);
                        YT_VERIFY(SpecifiedAttributes_.emplace(internedKey, std::move(value)).second);
                        YT_VERIFY(Attributes_->Remove(key));
                    } catch (const std::exception& e) {
                        YT_LOG_ERROR(e, "Cannot parse value of pool attribute "
                            "(ObjectId: %v, AttributeName: %v, AttributeValue: %v)",
                            Id_,
                            key,
                            value);
                    }
                }
            }
        }
    }
}

void TSchedulerPool::GuardedUpdatePoolAttribute(
    TInternedAttributeKey key,
    const std::function<void(const TPoolConfigPtr&, const TString&)>& update)
{
    const auto& stringKey = key.Unintern();

    update(FullConfig_, stringKey);
    try {
        ValidateAll();
    } catch (const std::exception&) {
        auto restoringValueIt = SpecifiedAttributes_.find(key);
        if (restoringValueIt != SpecifiedAttributes_.end()) {
            // TODO(renadeen): avoid building INode
            FullConfig_->LoadParameter(stringKey, NYTree::ConvertToNode(restoringValueIt->second), EMergeStrategy::Overwrite);
        } else {
            FullConfig_->ResetParameter(stringKey);
        }
        throw;
    }
}
////////////////////////////////////////////////////////////////////////////////

TSchedulerPoolTree::TSchedulerPoolTree(NCypressClient::TObjectId id)
    : TBase(id)
    , FullConfig_(New<TFairShareStrategyTreeConfig>())
{ }

TString TSchedulerPoolTree::GetLowercaseObjectName() const
{
    return Format("scheduler pool tree %Qv", TreeName_);
}

TString TSchedulerPoolTree::GetCapitalizedObjectName() const
{
    return Format("Scheduler pool tree %Qv", TreeName_);
}

void TSchedulerPoolTree::Save(NCellMaster::TSaveContext& context) const
{
    TBase::Save(context);

    using NYT::Save;
    Save(context, TreeName_);
    Save(context, RootPool_);
    Save(context, SpecifiedAttributes_);
}

void TSchedulerPoolTree::Load(NCellMaster::TLoadContext& context)
{
    TBase::Load(context);

    using NYT::Load;
    Load(context, TreeName_);
    Load(context, RootPool_);

    // COMPAT(shakurov)
    if (context.GetVersion() < EMasterReign::SpecifiedAttributeFix) {
        auto oldSpecifiedAttributes = Load<THashMap<int, NYson::TYsonString>>(context);
        SpecifiedAttributes_.reserve(oldSpecifiedAttributes.size());
        for (auto& [k, v] : oldSpecifiedAttributes) {
            YT_VERIFY(SpecifiedAttributes_.emplace(TInternedAttributeKey(k), std::move(v)).second);
        }
    } else {
        Load(context, SpecifiedAttributes_);
    }

    FullConfig_->Load(ConvertToNode(SpecifiedAttributes_));

    // TODO(renadeen): kill after move attributes into subconfig.
    if (context.GetVersion() != NCellMaster::GetCurrentReign() && Attributes_) {
        const auto& schedulerPoolManager = context.GetBootstrap()->GetSchedulerPoolManager();
        for (const auto& [key, value] : Attributes_->Attributes()) {
            auto internedKey = TInternedAttributeKey::Lookup(key);
            if (internedKey == InvalidInternedAttribute) {
                continue;
            }
            if (schedulerPoolManager->GetKnownPoolTreeAttributes().contains(internedKey)) {
                if (SpecifiedAttributes_.contains(internedKey)) {
                    YT_LOG_ERROR("Found pool tree attribute that is stored in both SpecifiedAttributes map and common attributes map "
                        "(ObjectId: %v, AttributeName: %v, CommonAttributeValue: %v, SpecifiedAttributeValue: %v)",
                        TreeName_,
                        key,
                        value,
                        SpecifiedAttributes_[internedKey]);
                } else {
                    try {
                        YT_LOG_INFO("Moving pool tree attribute from common attributes map to SpecifiedAttributes map "
                            "(PoolTreeName: %v, AttributeName: %v, AttributeValue: %v)",
                            TreeName_,
                            key,
                            value);
                        FullConfig_->LoadParameter(key, NYTree::ConvertToNode(value), EMergeStrategy::Overwrite);
                        YT_VERIFY(SpecifiedAttributes_.emplace(internedKey, std::move(value)).second);
                        YT_VERIFY(Attributes_->Remove(key));
                    } catch (const std::exception& e) {
                        YT_LOG_ERROR(e, "Cannot parse value of pool tree attribute "
                            "(PoolTreeName: %v, AttributeName: %v, AttributeValue: %v)",
                            TreeName_,
                            key,
                            value);
                    }
                }
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSchedulerPoolServer
