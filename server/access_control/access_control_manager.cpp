#include "access_control_manager.h"
#include "config.h"
#include "private.h"

#include <yp/server/master/bootstrap.h>
#include <yp/server/master/yt_connector.h>

#include <yp/server/objects/type_handler.h>
#include <yp/server/objects/transaction.h>
#include <yp/server/objects/transaction_manager.h>
#include <yp/server/objects/db_schema.h>
#include <yp/server/objects/object.h>
#include <yp/server/objects/helpers.h>

#include <yt/client/api/rowset.h>

#include <yt/client/table_client/helpers.h>

#include <yt/ytlib/api/native/client.h>

#include <yt/core/misc/property.h>
#include <yt/core/misc/error.h>

#include <yt/core/concurrency/rw_spinlock.h>
#include <yt/core/concurrency/periodic_executor.h>
#include <yt/core/concurrency/thread_affinity.h>
#include <yt/core/concurrency/fls.h>

namespace NYP::NServer::NAccessControl {

using namespace NObjects;
using namespace NApi;
using namespace NRpc;
using namespace NTableClient;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

TAuthenticatedUserGuard::TAuthenticatedUserGuard(
    TAccessControlManagerPtr accessControlManager,
    const TObjectId& userId)
{
    accessControlManager->SetAuthenticatedUser(userId);
    AccessControlManager_ = std::move(accessControlManager);
}

TAuthenticatedUserGuard::TAuthenticatedUserGuard(TAuthenticatedUserGuard&& other)
{
    std::swap(AccessControlManager_, other.AccessControlManager_);
}

TAuthenticatedUserGuard::~TAuthenticatedUserGuard()
{
    if (AccessControlManager_) {
        AccessControlManager_->ResetAuthenticatedUser();
    }
}

TAuthenticatedUserGuard& TAuthenticatedUserGuard::operator=(TAuthenticatedUserGuard&& other)
{
    Release();
    std::swap(AccessControlManager_, other.AccessControlManager_);
    return *this;
}

void TAuthenticatedUserGuard::Release()
{
    if (AccessControlManager_) {
        AccessControlManager_->ResetAuthenticatedUser();
        AccessControlManager_.Reset();
    }
}

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TClusterSnapshot)
class TSubject;
class TUser;
class TGroup;

////////////////////////////////////////////////////////////////////////////////

class TSubject
{
public:
    DEFINE_BYVAL_RO_PROPERTY(TObjectId, Id);
    DEFINE_BYVAL_RO_PROPERTY(EObjectType, Type);

public:
    TSubject(TObjectId id, EObjectType type)
        : Id_(std::move(id))
        , Type_(type)
    { }

    virtual ~TSubject() = default;

    TUser* AsUser();
    TGroup* AsGroup();
};

////////////////////////////////////////////////////////////////////////////////

class TUser
    : public TSubject
{
public:
    explicit TUser(
        TObjectId id,
        NClient::NApi::NProto::TUserSpec spec)
        : TSubject(std::move(id), EObjectType::User)
        , Spec_(std::move(spec))
    { }

    DEFINE_BYREF_RO_PROPERTY(NClient::NApi::NProto::TUserSpec, Spec);
};

TUser* TSubject::AsUser()
{
    YCHECK(Type_ == EObjectType::User);
    return static_cast<TUser*>(this);
}

////////////////////////////////////////////////////////////////////////////////

class TGroup
    : public TSubject
{
public:
    DEFINE_BYREF_RW_PROPERTY(THashSet<TObjectId>, RecursiveUserIds);
    DEFINE_BYREF_RO_PROPERTY(NClient::NApi::NProto::TGroupSpec, Spec);

public:
    explicit TGroup(TObjectId id, NClient::NApi::NProto::TGroupSpec spec = {})
        : TSubject(std::move(id), EObjectType::Group)
        , Spec_(std::move(spec))
    { }

    bool ContainsUser(const TObjectId& userId) const
    {
        return RecursiveUserIds_.find(userId) != RecursiveUserIds_.end();
    }
};

TGroup* TSubject::AsGroup()
{
    YCHECK(Type_ == EObjectType::Group);
    return static_cast<TGroup*>(this);
}

////////////////////////////////////////////////////////////////////////////////

namespace {

bool ContainsPermission(const NClient::NApi::NProto::TAccessControlEntry& ace, EAccessControlPermission permission)
{
    return std::find(
        ace.permissions().begin(),
        ace.permissions().end(),
        static_cast<NClient::NApi::NProto::EAccessControlPermission>(permission)) !=
        ace.permissions().end();
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

class TClusterSnapshot
    : public TRefCounted
{
public:
    TClusterSnapshot()
    {
        auto everyoneGroup = std::make_unique<TGroup>(EveryoneSubjectId);
        EveryoneGroup_ = everyoneGroup.get();
        AddSubject(std::move(everyoneGroup));
    }

    void AddSubject(std::unique_ptr<TSubject> subjectHolder)
    {
        auto* subject = subjectHolder.get();
        const auto& id = subject->GetId();
        if (!IdToSubject_.emplace(id, std::move(subjectHolder)).second) {
            THROW_ERROR_EXCEPTION("Duplicate subject %Qv",
                id);
        }

        if (subject->GetType() == EObjectType::User) {
            YCHECK(EveryoneGroup_->RecursiveUserIds().insert(id).second);
        }
    }

    bool IsSuperuser(const TObjectId& userId)
    {
        if (userId == RootUserId) {
            return true;
        }

        if (SuperusersGroup_ && SuperusersGroup_->ContainsUser(userId)) {
            return true;
        }

        return false;
    }

    TSubject* FindSubject(const TObjectId& id)
    {
        auto it = IdToSubject_.find(id);
        return it == IdToSubject_.end() ? nullptr : it->second.get();
    }

    TGroup* GetSuperusersGroup()
    {
        return SuperusersGroup_;
    }

    TGroup* GetEveryoneGroup()
    {
        return EveryoneGroup_;
    }

    void Prepare()
    {
        THashSet<TGroup*> visitedGroups;
        for (const auto& pair : IdToSubject_) {
            auto* subject = pair.second.get();
            if (subject->GetType() == EObjectType::Group) {
                auto* group = subject->AsGroup();
                visitedGroups.clear();
                ComputeRecursiveUsers(group, group, &visitedGroups);
            }
        }

        {
            auto* superusersSubject = FindSubject(SuperusersGroupId);
            if (superusersSubject) {
                if (superusersSubject->GetType() != EObjectType::Group) {
                    THROW_ERROR_EXCEPTION("%Qv must be a group",
                        SuperusersGroupId);
                }
                SuperusersGroup_ = superusersSubject->AsGroup();
            }
        }
    }

    std::optional<std::tuple<EAccessControlAction, TObjectId>> ApplyAcl(
        const std::vector<NClient::NApi::NProto::TAccessControlEntry>& acl,
        EAccessControlPermission permission,
        const TObjectId& userId)
    {
        std::optional<std::tuple<EAccessControlAction, TObjectId>> result;
        for (const auto& ace : acl) {
            auto subresult = ApplyAce(ace, permission, userId);
            if (subresult) {
                if (std::get<0>(*subresult) == EAccessControlAction::Deny) {
                    return subresult;
                }
                result = subresult;
            }
        }
        return result;
    }

private:
    THashMap<TObjectId, std::unique_ptr<TSubject>> IdToSubject_;
    TGroup* SuperusersGroup_ = nullptr;
    TGroup* EveryoneGroup_ = nullptr;

private:
    void ComputeRecursiveUsers(
        TGroup* forGroup,
        TGroup* currentGroup,
        THashSet<TGroup*>* visitedGroups)
    {
        if (!visitedGroups->insert(currentGroup).second) {
            return;
        }

        for (const auto& subjectId : currentGroup->Spec().members()) {
            auto* subject = FindSubject(subjectId);
            if (!subject) {
                continue;
            }
            switch (subject->GetType()) {
                case EObjectType::User:
                    forGroup->RecursiveUserIds().insert(subject->GetId());
                    break;
                case EObjectType::Group:
                    ComputeRecursiveUsers(forGroup, subject->AsGroup(), visitedGroups);
                    break;
                default:
                    Y_UNREACHABLE();
            }
        }
    }

    std::optional<std::tuple<EAccessControlAction, TObjectId>> ApplyAce(
        const NClient::NApi::NProto::TAccessControlEntry& ace,
        EAccessControlPermission permission,
        const TObjectId& userId)
    {
        if (!ContainsPermission(ace, permission)) {
            return std::nullopt;
        }

        for (const auto& subjectId : ace.subjects()) {
            auto* subject = FindSubject(subjectId);
            if (!subject) {
                continue;
            }

            switch (subject->GetType()) {
                case EObjectType::User:
                    if (subjectId == userId) {
                        return std::make_tuple(static_cast<EAccessControlAction>(ace.action()), subjectId);
                    }
                    break;
                case EObjectType::Group: {
                    auto* group = subject->AsGroup();
                    if (group->ContainsUser(userId)) {
                        return std::make_tuple(static_cast<EAccessControlAction>(ace.action()), subjectId);
                    }
                    break;
                }
                default:
                    Y_UNREACHABLE();
            }
        }

        return std::nullopt;
    }
};

DEFINE_REFCOUNTED_TYPE(TClusterSnapshot)

////////////////////////////////////////////////////////////////////////////////

class TAccessControlManager::TImpl
    : public TRefCounted
{
public:
    TImpl(
        NMaster::TBootstrap* bootstrap,
        TAccessControlManagerConfigPtr config)
        : Bootstrap_(bootstrap)
        , Config_(std::move(config))
        , ClusterStateUpdateExecutor_(New<TPeriodicExecutor>(
            Bootstrap_->GetControlInvoker(),
            BIND(&TImpl::OnUpdateClusterState, MakeWeak(this)),
            Config_->ClusterStateUpdatePeriod))
    { }

    void Initialize()
    {
        const auto& ytConnector = Bootstrap_->GetYTConnector();
        ytConnector->SubscribeConnected(BIND(&TImpl::OnConnected, MakeWeak(this)));
        ytConnector->SubscribeDisconnected(BIND(&TImpl::OnDisconnected, MakeWeak(this)));
    }

    TPermissionCheckResult CheckPermission(
        const TObjectId& subjectId,
        TObject* object,
        EAccessControlPermission permission)
    {
        return CheckPermission(
            subjectId,
            object,
            permission,
            GetClusterSnapshot());
    }

    TUserIdList GetObjectAccessAllowedFor(
        TObject* object,
        EAccessControlPermission permission)
    {
        auto snapshot = GetClusterSnapshot();

        THashSet<TObjectId> allowedForUserIds;
        THashSet<TObjectId> deniedForUserIds;
        InvokeForAccessControlHierarchy(
            object,
            [&] (auto* object) {
                const auto& acl = object->Acl().Load();
                for (const auto& ace : acl) {
                    if (!ContainsPermission(ace, permission)) {
                        continue;
                    }

                    auto handleUserId = [&] (const auto& userId) {
                        switch (ace.action()) {
                            case NClient::NApi::NProto::ACA_ALLOW:
                                allowedForUserIds.insert(userId);
                                break;
                            case NClient::NApi::NProto::ACA_DENY:
                                deniedForUserIds.insert(userId);
                                break;
                            default:
                                Y_UNREACHABLE();
                        }
                    };

                    for (const auto& subjectId : ace.subjects()) {
                        auto* subject = snapshot->FindSubject(subjectId);
                        if (!subject) {
                            continue;
                        }
                        switch (subject->GetType()) {
                            case EObjectType::User:
                                handleUserId(subjectId);
                                break;

                            case EObjectType::Group:
                                for (const auto& userId : subject->AsGroup()->RecursiveUserIds()) {
                                    handleUserId(userId);
                                }
                                break;

                            default:
                                Y_UNREACHABLE();
                        }
                    }
                }
                return true;
            });

        const auto* superusersGroup = snapshot->GetSuperusersGroup();
        if (superusersGroup) {
            for (const auto& userId : superusersGroup->RecursiveUserIds()) {
                allowedForUserIds.insert(userId);
                deniedForUserIds.erase(userId);
            }
        }

        std::vector<TObjectId> result;
        result.reserve(allowedForUserIds.size());
        for (const auto& id : allowedForUserIds) {
            if (deniedForUserIds.count(id) == 0) {
                result.push_back(id);
            }
        }

        return result;
    }

    std::vector<TObject*> GetUserAccessAllowedTo(
        const TTransactionPtr& transaction,
        NObjects::TObject* user,
        NObjects::EObjectType objectType,
        EAccessControlPermission permission)
    {
        auto objects = transaction->SelectObjects(objectType);
        auto snapshot = GetClusterSnapshot();
        objects.erase(
            std::remove_if(
                objects.begin(),
                objects.end(),
                [&] (TObject* object) {
                    auto permissionCheckResult = CheckPermission(
                        user->GetId(),
                        object,
                        permission,
                        snapshot);
                    return permissionCheckResult.Action == EAccessControlAction::Deny;
                }),
            objects.end());
        return objects;
    }

    void SetAuthenticatedUser(const TObjectId& userId)
    {
        auto snapshot = GetClusterSnapshot();
        auto* subject = snapshot->FindSubject(userId);
        if (!subject) {
            THROW_ERROR_EXCEPTION(
                NClient::NApi::EErrorCode::AuthenticationError,
                "Authenticated user %Qv is not registered",
                userId);
        }
        if (subject->GetType() != EObjectType::User) {
            THROW_ERROR_EXCEPTION(
                NClient::NApi::EErrorCode::AuthenticationError,
                "Authenticated user %Qv is registered as %Qlv",
                userId,
                subject->GetType());
        }
        const auto* user = subject->AsUser();
        if (user->Spec().banned()) {
            THROW_ERROR_EXCEPTION(
                NClient::NApi::EErrorCode::UserBanned,
                "Authenticated user %Qv is banned",
                userId);
        }

        *AuthenticatedUserId_ = userId;
    }

    void ResetAuthenticatedUser()
    {
        AuthenticatedUserId_->reset();
    }

    bool HasAuthenticatedUser()
    {
        return static_cast<bool>(*AuthenticatedUserId_);
    }

    TObjectId GetAuthenticatedUser()
    {
        auto userId = *AuthenticatedUserId_;
        if (!userId) {
            THROW_ERROR_EXCEPTION(
                NClient::NApi::EErrorCode::AuthenticationError,
                "User is not authenticated");
        }
        return *userId;
    }

    void ValidatePermission(TObject* object, EAccessControlPermission permission)
    {
        auto userId = GetAuthenticatedUser();
        auto result = CheckPermission(userId, object, permission);
        if (result.Action == EAccessControlAction::Deny) {
            TError error;
            if (result.ObjectId && result.SubjectId) {
                error = TError(
                    NClient::NApi::EErrorCode::AuthorizationError,
                    "Access denied: %Qlv permission for %v %v is denied for %Qv by ACE at %v %v",
                    permission,
                    GetLowercaseHumanReadableTypeName(object->GetType()),
                    GetObjectDisplayName(object),
                    result.SubjectId,
                    GetLowercaseHumanReadableTypeName(result.ObjectType),
                    result.ObjectId);
            } else {
                error = TError(
                    NClient::NApi::EErrorCode::AuthorizationError,
                    "Access denied: %Qlv permission for %v %v is not allowed by any matching ACE",
                    permission,
                    GetLowercaseHumanReadableTypeName(object->GetType()),
                    GetObjectDisplayName(object));
            }
            error.Attributes().Set("permission", permission);
            error.Attributes().Set("user_id", userId);
            error.Attributes().Set("object_type", object->GetType());
            error.Attributes().Set("object_id", object->GetId());
            if (result.ObjectId) {
                error.Attributes().Set("denied_by_id", result.ObjectId);
                error.Attributes().Set("denied_by_type", result.ObjectType);
            }
            if (result.SubjectId) {
                error.Attributes().Set("denied_for", result.SubjectId);
            }
            THROW_ERROR(error);
        }
    }

    void ValidateSuperuser()
    {
        auto userId = GetAuthenticatedUser();
        auto snapshot = GetClusterSnapshot();
        if (!snapshot->IsSuperuser(userId)) {
            THROW_ERROR_EXCEPTION(
                NClient::NApi::EErrorCode::AuthorizationError,
                "User %Qv must be a superuser to do that",
                userId)
                << TErrorAttribute("user_id", userId);
        }
    }

private:
    NMaster::TBootstrap* const Bootstrap_;
    const TAccessControlManagerConfigPtr Config_;

    const TPeriodicExecutorPtr ClusterStateUpdateExecutor_;

    TReaderWriterSpinLock ClusterSnapshotLock_;
    TClusterSnapshotPtr ClusterSnapshot_;

    static NConcurrency::TFls<std::optional<TObjectId>> AuthenticatedUserId_;

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);

private:
    template <class F>
    void InvokeForAccessControlHierarchy(
        TObject* object,
        F func)
    {
        while (object) {
            if (!func(object)) {
                break;
            }

            if (!object->InheritAcl().Load()) {
                break;
            }

            auto* typeHandler = object->GetTypeHandler();
            object = typeHandler->GetAccessControlParent(object);
        }
    }

    TPermissionCheckResult CheckPermission(
        const TObjectId& subjectId,
        TObject* object,
        EAccessControlPermission permission,
        TClusterSnapshotPtr snapshot)
    {
        TPermissionCheckResult result;
        result.Action = EAccessControlAction::Deny;

        if (snapshot->IsSuperuser(subjectId)) {
            result.Action = EAccessControlAction::Allow;
            return result;
        }

        InvokeForAccessControlHierarchy(
            object,
            [&] (auto* object) {
                const auto& acl = object->Acl().Load();
                auto subresult = snapshot->ApplyAcl(acl, permission, subjectId);
                if (subresult) {
                    result.ObjectId = object->GetId();
                    result.ObjectType = object->GetType();
                    result.SubjectId = std::get<1>(*subresult);
                    switch (std::get<0>(*subresult)) {
                        case EAccessControlAction::Allow:
                            if (result.Action == EAccessControlAction::Deny) {
                                result.Action = EAccessControlAction::Allow;
                                result.SubjectId = std::get<1>(*subresult);
                            }
                            break;
                        case EAccessControlAction::Deny:
                            result.Action = EAccessControlAction::Deny;
                            return false;
                        default:
                            Y_UNREACHABLE();
                    }
                }
                return true;
            });

        return result;
    }

    TClusterSnapshotPtr GetClusterSnapshot()
    {
        TReaderGuard guard(ClusterSnapshotLock_);
        if (!ClusterSnapshot_) {
            THROW_ERROR_EXCEPTION(
                NRpc::EErrorCode::Unavailable,
                "Cluster access control state is not loaded yet");
        }
        return ClusterSnapshot_;
    }

    void SetClusterSnapshot(TClusterSnapshotPtr snapshot)
    {
        TWriterGuard guard(ClusterSnapshotLock_);
        std::swap(ClusterSnapshot_, snapshot);
    }

    void OnConnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ClusterStateUpdateExecutor_->Start();
    }

    void OnDisconnected()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        ClusterStateUpdateExecutor_->Stop();
    }

    void OnUpdateClusterState()
    {
        VERIFY_THREAD_AFFINITY(ControlThread);

        try {
            YT_LOG_DEBUG("Started loading cluster snapshot");

            YT_LOG_DEBUG("Starting snapshot transaction");

            const auto& transactionManager = Bootstrap_->GetTransactionManager();
            auto transaction = WaitFor(transactionManager->StartReadOnlyTransaction())
                .ValueOrThrow();

            YT_LOG_DEBUG("Snapshot transaction started (Timestamp: %llx)",
                transaction->GetStartTimestamp());

            int userCount = 0;
            int groupCount = 0;

            auto snapshot = New<TClusterSnapshot>();

            auto* session = transaction->GetSession();

            {
                session->ScheduleLoad(
                    [&] (ILoadContext* context) {
                        context->ScheduleSelect(
                            GetUserQueryString(),
                            [&] (const IUnversionedRowsetPtr& rowset) {
                                YT_LOG_DEBUG("Parsing users");
                                for (auto row : rowset->GetRows()) {
                                    ++userCount;
                                    ParseUserFromRow(snapshot, row);
                                }
                            });
                    });

                YT_LOG_DEBUG("Querying users");
                session->FlushLoads();
            }

            {
                session->ScheduleLoad(
                    [&] (ILoadContext* context) {
                        context->ScheduleSelect(
                            GetGroupQueryString(),
                            [&] (const IUnversionedRowsetPtr& rowset) {
                                YT_LOG_DEBUG("Parsing groups");
                                for (auto row : rowset->GetRows()) {
                                    ++groupCount;
                                    ParseGroupFromRow(snapshot, row);
                                }
                            });
                    });

                YT_LOG_DEBUG("Querying groups");
                session->FlushLoads();
            }

            snapshot->Prepare();
            SetClusterSnapshot(std::move(snapshot));

            YT_LOG_DEBUG("Finished loading cluster snapshot (UserCount: %v, GroupCount: %v)",
                userCount,
                groupCount);
        } catch (const std::exception& ex) {
            YT_LOG_WARNING(ex, "Error loading cluster snapshot");
        }
    }

    TString GetUserQueryString()
    {
        const auto& ytConnector = Bootstrap_->GetYTConnector();
        return Format(
            "[%v], [%v] from [%v] where is_null([%v])",
            UsersTable.Fields.Meta_Id.Name,
            UsersTable.Fields.Spec.Name,
            ytConnector->GetTablePath(&UsersTable),
            UsersTable.Fields.Meta_RemovalTime.Name);
    }

    TString GetGroupQueryString()
    {
        const auto& ytConnector = Bootstrap_->GetYTConnector();
        return Format(
            "[%v], [%v] from [%v] where is_null([%v])",
            GroupsTable.Fields.Meta_Id.Name,
            GroupsTable.Fields.Spec.Name,
            ytConnector->GetTablePath(&GroupsTable),
            GroupsTable.Fields.Meta_RemovalTime.Name);
    }

    void ParseUserFromRow(const TClusterSnapshotPtr& snapshot, TUnversionedRow row)
    {
        TObjectId userId;
        NClient::NApi::NProto::TUserSpec spec;
        FromUnversionedRow(
            row,
            &userId,
            &spec);

        auto user = std::make_unique<TUser>(std::move(userId), std::move(spec));
        snapshot->AddSubject(std::move(user));
    }

    void ParseGroupFromRow(const TClusterSnapshotPtr& snapshot, TUnversionedRow row)
    {
        TObjectId groupId;
        NClient::NApi::NProto::TGroupSpec spec;
        FromUnversionedRow(
            row,
            &groupId,
            &spec);

        auto group = std::make_unique<TGroup>(std::move(groupId), std::move(spec));
        snapshot->AddSubject(std::move(group));
    }
};

NConcurrency::TFls<std::optional<TObjectId>> TAccessControlManager::TImpl::AuthenticatedUserId_;

////////////////////////////////////////////////////////////////////////////////

TAccessControlManager::TAccessControlManager(
    NMaster::TBootstrap* bootstrap,
    TAccessControlManagerConfigPtr config)
    : Impl_(New<TImpl>(bootstrap, std::move(config)))
{ }

void TAccessControlManager::Initialize()
{
    Impl_->Initialize();
}

TPermissionCheckResult TAccessControlManager::CheckPermission(
    const TObjectId& subjectId,
    TObject* object,
    EAccessControlPermission permission)
{
    return Impl_->CheckPermission(
        subjectId,
        object,
        permission);
}

TUserIdList TAccessControlManager::GetObjectAccessAllowedFor(
    TObject* object,
    EAccessControlPermission permission)
{
    return Impl_->GetObjectAccessAllowedFor(
        object,
        permission);
}

std::vector<TObject*> TAccessControlManager::GetUserAccessAllowedTo(
    const TTransactionPtr& transaction,
    NObjects::TObject* user,
    NObjects::EObjectType objectType,
    EAccessControlPermission permission)
{
    return Impl_->GetUserAccessAllowedTo(
        transaction,
        user,
        objectType,
        permission);
}

void TAccessControlManager::SetAuthenticatedUser(const TObjectId& userId)
{
    Impl_->SetAuthenticatedUser(userId);
}

void TAccessControlManager::ResetAuthenticatedUser()
{
    Impl_->ResetAuthenticatedUser();
}

TObjectId TAccessControlManager::GetAuthenticatedUser()
{
    return Impl_->GetAuthenticatedUser();
}

bool TAccessControlManager::HasAuthenticatedUser()
{
    return Impl_->HasAuthenticatedUser();
}

void TAccessControlManager::ValidatePermission(TObject* object, EAccessControlPermission permission)
{
    Impl_->ValidatePermission(object, permission);
}

void TAccessControlManager::ValidateSuperuser()
{
    return Impl_->ValidateSuperuser();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NAccessControl

