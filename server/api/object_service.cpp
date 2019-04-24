#include "object_service.h"
#include "private.h"

#include <yp/server/master/bootstrap.h>
#include <yp/server/master/yt_connector.h>
#include <yp/server/master/service_detail.h>

#include <yp/server/objects/transaction_manager.h>
#include <yp/server/objects/transaction.h>
#include <yp/server/objects/object.h>
#include <yp/server/objects/object_manager.h>
#include <yp/server/objects/type_handler.h>
#include <yp/server/objects/helpers.h>

#include <yp/server/access_control/access_control_manager.h>

#include <yp/client/api/object_service_proxy.h>

#include <yt/ytlib/auth/authentication_manager.h>

#include <yt/core/ytree/convert.h>

#include <yt/core/concurrency/async_semaphore.h>

#include <contrib/libs/protobuf/io/zero_copy_stream_impl_lite.h>

namespace NYP::NServer::NApi {

using namespace NMaster;
using namespace NObjects;
using namespace NAccessControl;
using namespace NYT::NRpc;
using namespace NYT::NYson;
using namespace NYT::NYTree;
using namespace NYT::NConcurrency;

using NYT::FromProto;

static const TYsonString NullYsonString{"#"};

////////////////////////////////////////////////////////////////////////////////

class TObjectService
    : public NMaster::TServiceBase
{
public:
    explicit TObjectService(TBootstrap* bootstrap)
        : TServiceBase(
            bootstrap,
            NClient::NApi::TObjectServiceProxy::GetDescriptor(),
            NApi::Logger,
            bootstrap->GetAuthenticationManager()->GetRpcAuthenticator())
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GenerateTimestamp));

        RegisterMethod(RPC_SERVICE_METHOD_DESC(StartTransaction));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(CommitTransaction));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(AbortTransaction));

        RegisterMethod(RPC_SERVICE_METHOD_DESC(CreateObject));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(CreateObjects));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(RemoveObject));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(RemoveObjects));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(UpdateObject));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(UpdateObjects));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetObject));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetObjects));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(SelectObjects));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(CheckObjectPermissions));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetObjectAccessAllowedFor));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GetUserAccessAllowedTo));
    }

private:
    class TTransactionWrapper
    {
    public:
        TTransactionWrapper(
            const TTransactionId& id,
            bool mustNotOwn,
            TBootstrap* bootstrap)
        {
            const auto& transactionManager = bootstrap->GetTransactionManager();
            if (id) {
                Owned_ = false;

                auto transaction = transactionManager->GetTransactionOrThrow(id);
                if (transaction->GetState() != ETransactionState::Active) {
                    THROW_ERROR_EXCEPTION(
                        NClient::NApi::EErrorCode::InvalidTransactionState,
                        "Transaction %v is in %Qlv state",
                        id,
                        transaction->GetState());
                }

                Transaction_ = std::move(transaction);
                LockGuard_ = Transaction_->AcquireLock();
            } else {
                if (mustNotOwn) {
                    THROW_ERROR_EXCEPTION(
                        NClient::NApi::EErrorCode::InvalidTransactionId,
                        "Null transaction id is not allowed");
                }
                Owned_ = true;
                Transaction_ = WaitFor(transactionManager->StartReadWriteTransaction())
                    .ValueOrThrow();
            }
        }

        const TTransactionPtr& Unwrap() const
        {
            return Transaction_;
        }

        void CommitIfOwned()
        {
            if (Owned_) {
                WaitFor(Transaction_->Commit())
                    .ThrowOnError();
            }
        }

    private:
        bool Owned_ = false;
        TTransactionPtr Transaction_;
        TAsyncSemaphoreGuard LockGuard_;
    };


    TAuthenticatedUserGuard MakeAuthenticatedUserGuard(const NRpc::IServiceContextPtr& context)
    {
        return TAuthenticatedUserGuard(Bootstrap_->GetAccessControlManager(), context->GetUser());
    }

    // COMPAT(babenko): YP-752
    template <class T>
    EObjectType CheckedEnumCastToObjectType(T type)
    {
        if (type == NClient::NApi::NProto::OT_NODE2) {
            type = NClient::NApi::NProto::OT_NODE;
        }
        return CheckedEnumCast<EObjectType>(type);
    }



    template <class TContextPtr>
    void LogDeprecatedPayloadFormat(const TContextPtr& context)
    {
        YT_LOG_DEBUG("Deprecated payload format (RequestId: %v, User: %v)",
            context->GetRequestId(),
            context->GetUser());
    }

    static const TProtobufMessageType* GetMessageTypeByYPath(
        const TProtobufMessageType* rootType,
        const NYPath::TYPath& path)
    {
        auto result = ResolveProtobufElementByYPath(rootType, path);
        auto* messageElement = std::get_if<std::unique_ptr<TProtobufMessageElement>>(&result.Element);
        if (!messageElement) {
            THROW_ERROR_EXCEPTION("Attribute %v is not a protobuf message",
                result.HeadPath);
        }
        return (*messageElement)->Type;
    }

    TYsonString PayloadToYsonString(
        const NClient::NApi::NProto::TPayload& payload,
        EObjectType type,
        const TYPath& path)
    {
        if (payload.has_yson()) {
            return payload.yson() ? TYsonString(payload.yson()) : TYsonString();
        } else if (payload.has_protobuf()) {
            const auto& objectManager = Bootstrap_->GetObjectManager();
            auto* typeHandler = objectManager->GetTypeHandler(type);
            const auto* rootType = typeHandler->GetRootProtobufType();
            const auto* payloadType = GetMessageTypeByYPath(rootType, path);
            google::protobuf::io::ArrayInputStream protobufInputStream(payload.protobuf().data(), payload.protobuf().length());
            TString yson;
            TStringOutput ysonOutputStream(yson);
            TYsonWriter writer(&ysonOutputStream);
            ParseProtobuf(&writer, &protobufInputStream, payloadType);
            return TYsonString(std::move(yson));
        } else {
            return TYsonString();
        }
    }

    NClient::NApi::NProto::TPayload YsonStringToPayload(
        const TYsonString& ysonString,
        EObjectType type,
        const TYPath& path,
        NClient::NApi::NProto::EPayloadFormat format)
    {
        NClient::NApi::NProto::TPayload payload;
        if (!ysonString) {
            payload.set_null(true);
            return payload;
        }
        switch (format) {
            case NClient::NApi::NProto::PF_YSON:
                payload.set_yson(ysonString.GetData());
                break;

            case NClient::NApi::NProto::PF_PROTOBUF: {
                const auto& objectManager = Bootstrap_->GetObjectManager();
                auto* typeHandler = objectManager->GetTypeHandler(type);
                const auto* rootType = typeHandler->GetRootProtobufType();
                const auto* payloadType = GetMessageTypeByYPath(rootType, path);
                google::protobuf::io::StringOutputStream protobufStream(payload.mutable_protobuf());
                auto protobufWriter = CreateProtobufWriter(&protobufStream, payloadType);
                ParseYsonStringBuffer(ysonString.GetData(), EYsonType::Node, protobufWriter.get());
                break;
            }

            default:
                Y_UNREACHABLE();
        }
        return payload;
    }

    void MoveAttributesToProto(
        NClient::NApi::NProto::EPayloadFormat format,
        EObjectType objectType,
        const TAttributeSelector& selector,
        TAttributeValueList* object,
        NClient::NApi::NProto::TAttributeValueList* protoResult)
    {
        if (format == NClient::NApi::NProto::PF_NONE) {
            // COMPAT(babenko)
            auto* responseValues = protoResult->mutable_values();
            for (const auto& value : object->Values) {
                *responseValues->Add() = value.GetData();
            }
        } else {
            auto* responseValuePayloads = protoResult->mutable_value_payloads();
            YCHECK(object->Values.size() == selector.Paths.size());
            for (size_t index = 0; index < object->Values.size(); ++index) {
                *responseValuePayloads->Add() = YsonStringToPayload(
                    object->Values[index],
                    objectType,
                    selector.Paths[index],
                    format);
            }
        }
        object->Values.clear();
        object->Values.shrink_to_fit();
    }

    TUpdateRequest ParseRemoveUpdate(const NClient::NApi::NProto::TRemoveUpdate& protoUpdate)
    {
        return TRemoveUpdateRequest{
            protoUpdate.path()
        };
    }

    template <class TContextPtr>
    TUpdateRequest ParseSetUpdate(
        const TContextPtr& context,
        EObjectType type,
        const NClient::NApi::NProto::TSetUpdate& protoUpdate,
        bool* deprecatedPayloadFormatLogged)
    {
        const auto& path = protoUpdate.path();
        TYsonString value;
        if (protoUpdate.has_value()) {
            value = TYsonString(protoUpdate.value());
            if (!*deprecatedPayloadFormatLogged) {
                LogDeprecatedPayloadFormat(context);
                *deprecatedPayloadFormatLogged = true;
            }
        } else if (protoUpdate.has_value_payload()) {
            value = PayloadToYsonString(protoUpdate.value_payload(), type, path);
            if (!value) {
                value = NullYsonString;
            }
        } else {
            THROW_ERROR_EXCEPTION("Neither \"value\" nor \"value_payload\" is given");
        }
        return TSetUpdateRequest{
            path,
            ConvertToNode(value),
            protoUpdate.recursive()
        };
    }


    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, GenerateTimestamp)
    {
        Y_UNUSED(request);
        context->SetRequestInfo();

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto timestamp = WaitFor(transactionManager->GenerateTimestamp())
            .ValueOrThrow();

        response->set_timestamp(timestamp);
        context->SetResponseInfo("Timestamp: %llx", timestamp);
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, StartTransaction)
    {
        Y_UNUSED(request);
        context->SetRequestInfo();

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto transaction = WaitFor(transactionManager->StartReadWriteTransaction())
            .ValueOrThrow();

        ToProto(response->mutable_transaction_id(), transaction->GetId());
        response->set_start_timestamp(transaction->GetStartTimestamp());
        context->SetResponseInfo("TransactionId: %v", transaction->GetId());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, CommitTransaction)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        context->SetRequestInfo("TransactionId: %v", transactionId);

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        TTransactionWrapper transactionWrapper(transactionId, true, Bootstrap_);
        const auto& transaction = transactionWrapper.Unwrap();

        auto result = WaitFor(transaction->Commit())
            .ValueOrThrow();

        response->set_commit_timestamp(result.CommitTimestamp);
        context->SetResponseInfo("CommitTimestamp: %llx", result.CommitTimestamp);
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, AbortTransaction)
    {
        Y_UNUSED(response);
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        context->SetRequestInfo("TransactionId: %v", transactionId);

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        TTransactionWrapper transactionWrapper(transactionId, true, Bootstrap_);
        const auto& transaction = transactionWrapper.Unwrap();

        transaction->Abort();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, CreateObject)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto objectType = CheckedEnumCastToObjectType(request->object_type());

        context->SetRequestInfo("TransactionId: %v, ObjectType: %v",
            transactionId,
            objectType);

        IMapNodePtr attributes;
        if (request->has_attributes()) {
            LogDeprecatedPayloadFormat(context);
            attributes = ConvertTo<IMapNodePtr>(TYsonString(request->attributes()));
        } else if (request->has_attributes_payload()) {
            attributes = ConvertTo<IMapNodePtr>(PayloadToYsonString(
                request->attributes_payload(),
                objectType,
                TYPath()));
        } else {
            attributes = GetEphemeralNodeFactory()->CreateMap();
        }

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        TTransactionWrapper transactionWrapper(transactionId, false, Bootstrap_);
        const auto& transaction = transactionWrapper.Unwrap();

        auto* object = transaction->CreateObject(objectType, attributes);

        transactionWrapper.CommitIfOwned();

        ToProto(response->mutable_object_id(), object->GetId());
        context->SetResponseInfo("ObjectId: %v", object->GetId());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, CreateObjects)
    {
        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        struct TSubrequest
        {
            EObjectType Type;
            NYT::NYTree::IMapNodePtr Attributes;
        };

        std::vector<TSubrequest> subrequests;
        bool deprecatedPayloadFormatLogged = false;
        subrequests.reserve(request->subrequests_size());
        for (const auto& protoSubrequest : request->subrequests()) {
            TSubrequest subrequest;
            subrequest.Type = CheckedEnumCastToObjectType(protoSubrequest.object_type());
            if (protoSubrequest.has_attributes()) {
                if (!deprecatedPayloadFormatLogged) {
                    LogDeprecatedPayloadFormat(context);
                    deprecatedPayloadFormatLogged = true;
                }
                subrequest.Attributes = ConvertTo<IMapNodePtr>(TYsonString(protoSubrequest.attributes()));
            } else if (protoSubrequest.has_attributes_payload()) {
                subrequest.Attributes = ConvertTo<IMapNodePtr>(PayloadToYsonString(
                    protoSubrequest.attributes_payload(),
                    subrequest.Type,
                    TYPath()));
            } else {
                subrequest.Attributes = GetEphemeralNodeFactory()->CreateMap();
            }
            subrequests.push_back(std::move(subrequest));
        }

        context->SetRequestInfo("TransactionId: %v, Subrequests: %v",
            transactionId,
            MakeFormattableView(subrequests, [] (auto* builder, const auto& subrequest) {
                builder->AppendFormat("{ObjectType: %v}",
                    subrequest.Type);
            }));

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        TTransactionWrapper transactionWrapper(transactionId, false, Bootstrap_);
        const auto& transaction = transactionWrapper.Unwrap();

        std::vector<TObject*> objects;
        objects.reserve(subrequests.size());

        auto updateContext = transaction->CreateUpdateContext();
        for (const auto& subrequest : subrequests) {
            auto* object = transaction->CreateObject(subrequest.Type, subrequest.Attributes, updateContext.get());
            objects.push_back(object);
        }

        updateContext->Commit();
        transactionWrapper.CommitIfOwned();

        for (auto* object : objects) {
            auto* subresponse = response->add_subresponses();
            ToProto(subresponse->mutable_object_id(), object->GetId());
        }

        context->SetResponseInfo("ObjectIds: %v",
            MakeFormattableView(objects, [] (auto* builder, auto* object) {
                builder->AppendFormat("%v",
                    object->GetId());
            }));
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, RemoveObject)
    {
        Y_UNUSED(response);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto objectType = CheckedEnumCastToObjectType(request->object_type());
        auto objectId = FromProto<TObjectId>(request->object_id());

        context->SetRequestInfo("TransactionId: %v, ObjectType: %v, ObjectId: %v",
            transactionId,
            objectType,
            objectId);

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        TTransactionWrapper transactionWrapper(transactionId, false, Bootstrap_);
        const auto& transaction = transactionWrapper.Unwrap();

        auto* object = transaction->GetObject(objectType, objectId);
        transaction->RemoveObject(object);

        transactionWrapper.CommitIfOwned();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, RemoveObjects)
    {
        Y_UNUSED(response);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        struct TSubrequest
        {
            EObjectType Type;
            TObjectId Id;
        };

        std::vector<TSubrequest> subrequests;
        subrequests.reserve(request->subrequests_size());
        for (const auto& subrequest : request->subrequests()) {
            subrequests.push_back({
                CheckedEnumCastToObjectType(subrequest.object_type()),
                FromProto<TObjectId>(subrequest.object_id())
            });
        }

        context->SetRequestInfo("TransactionId: %v, Subrequests: %v",
            transactionId,
            MakeFormattableView(subrequests, [] (auto* builder, const auto& subrequest) {
                builder->AppendFormat("{ObjectType: %v, ObjectId: %v}",
                    subrequest.Type,
                    subrequest.Id);
            }));

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        TTransactionWrapper transactionWrapper(transactionId, false, Bootstrap_);
        const auto& transaction = transactionWrapper.Unwrap();

        std::vector<TObject*> objects;
        objects.reserve(subrequests.size());
        for (const auto& subrequest : subrequests) {
            auto* object = transaction->GetObject(subrequest.Type, subrequest.Id);
            objects.push_back(object);
        }

        auto updateContext = transaction->CreateUpdateContext();
        for (auto* object : objects) {
            transaction->RemoveObject(object, updateContext.get());
        }
        updateContext->Commit();

        transactionWrapper.CommitIfOwned();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, UpdateObject)
    {
        Y_UNUSED(response);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());
        auto objectType = CheckedEnumCastToObjectType(request->object_type());
        auto objectId = FromProto<TObjectId>(request->object_id());

        bool deprecatedPayloadFormatLogged = false;
        std::vector<TUpdateRequest> updates;
        updates.reserve(request->set_updates().size() + request->remove_updates().size());
        for (const auto& update : request->set_updates()) {
            updates.push_back(ParseSetUpdate(context, objectType, update, &deprecatedPayloadFormatLogged));
        }
        for (const auto& update : request->remove_updates()) {
            updates.push_back(ParseRemoveUpdate(update));
        }

        context->SetRequestInfo("TransactionId: %v, ObjectType: %v, ObjectId: %v, UpdateCount: %v",
            transactionId,
            objectType,
            objectId,
            updates.size());

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        TTransactionWrapper transactionWrapper(transactionId, false, Bootstrap_);
        const auto& transaction = transactionWrapper.Unwrap();

        auto* object = transaction->GetObject(objectType, objectId);
        transaction->UpdateObject(object, updates);

        transactionWrapper.CommitIfOwned();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, UpdateObjects)
    {
        Y_UNUSED(response);

        auto transactionId = FromProto<TTransactionId>(request->transaction_id());

        struct TSubrequest
        {
            EObjectType Type;
            TObjectId Id;
            std::vector<TUpdateRequest> Updates;
        };

        bool deprecatedPayloadFormatLogged = false;
        std::vector<TSubrequest> subrequests;
        subrequests.reserve(request->subrequests_size());
        for (const auto& subrequest : request->subrequests()) {
            auto objectType = CheckedEnumCastToObjectType(subrequest.object_type());
            auto objectId = FromProto<TObjectId>(subrequest.object_id());
            std::vector<TUpdateRequest> updates;
            updates.reserve(subrequest.set_updates_size() + subrequest.remove_updates_size());
            for (const auto& update : subrequest.set_updates()) {
                updates.push_back(ParseSetUpdate(context, objectType, update, &deprecatedPayloadFormatLogged));
            }
            for (const auto& update : subrequest.remove_updates()) {
                updates.push_back(ParseRemoveUpdate(update));
            }
            subrequests.push_back({objectType, std::move(objectId), std::move(updates)});
        }

        context->SetRequestInfo("TransactionId: %v, Subrequests: %v",
            transactionId,
            MakeFormattableView(subrequests, [] (auto* builder, const auto& subrequest) {
                builder->AppendFormat("{ObjectType: %v, ObjectId: %v, UpdateCount: %v}",
                    subrequest.Type,
                    subrequest.Id,
                    subrequest.Updates.size());
            }));

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        TTransactionWrapper transactionWrapper(transactionId, false, Bootstrap_);
        const auto& transaction = transactionWrapper.Unwrap();

        std::vector<TObject*> objects;
        objects.reserve(subrequests.size());
        for (const auto& subrequest : subrequests) {
            objects.push_back(transaction->GetObject(subrequest.Type, subrequest.Id));
        }

        auto updateContext = transaction->CreateUpdateContext();
        for (size_t index = 0; index < subrequests.size(); ++index) {
            const auto& subrequest = subrequests[index];
            auto* object = objects[index];
            transaction->UpdateObject(object, subrequest.Updates, updateContext.get());
        }

        updateContext->Commit();
        transactionWrapper.CommitIfOwned();

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, GetObject)
    {
        auto objectId = FromProto<TObjectId>(request->object_id());
        auto objectType = CheckedEnumCastToObjectType(request->object_type());
        auto timestamp = request->timestamp();
        TAttributeSelector selector{
            FromProto<std::vector<TString>>(request->selector().paths())
        };

        context->SetRequestInfo("ObjectId: %v, ObjectType: %v, Timestamp: %llx, Selector: %v",
            objectId,
            objectType,
            timestamp,
            selector.Paths);

        auto format = request->format();
        if (format == NClient::NApi::NProto::PF_NONE) {
            LogDeprecatedPayloadFormat(context);
        }

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto transaction = WaitFor(transactionManager->StartReadOnlyTransaction(timestamp))
            .ValueOrThrow();

        auto result = transaction->ExecuteGetQuery(
            objectType,
            {objectId},
            selector);

        auto& object = result.Objects[0];
        YCHECK(object.has_value());
        MoveAttributesToProto(format, objectType, selector, &(*object), response->mutable_result());

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, GetObjects)
    {
        auto objectType = CheckedEnumCastToObjectType(request->object_type());
        auto timestamp = request->timestamp();
        TAttributeSelector selector{
            FromProto<std::vector<TString>>(request->selector().paths())
        };

        std::vector<TObjectId> objectIds;
        objectIds.reserve(request->subrequests().size());
        for (const auto& subrequest : request->subrequests()) {
            objectIds.emplace_back(subrequest.object_id());
        }

        context->SetRequestInfo("ObjectIds: %v, ObjectType: %v, Timestamp: %llx, Selector: %v",
            objectIds,
            objectType,
            timestamp,
            selector.Paths);

        auto format = request->format();
        if (format == NClient::NApi::NProto::PF_NONE) {
            LogDeprecatedPayloadFormat(context);
        }

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto transaction = WaitFor(transactionManager->StartReadOnlyTransaction(timestamp))
            .ValueOrThrow();

        auto result = transaction->ExecuteGetQuery(
            objectType,
            objectIds,
            selector);

        response->mutable_subresponses()->Reserve(result.Objects.size());
        for (auto& object : result.Objects) {
            YCHECK(object.has_value());
            auto* subresponse = response->add_subresponses();
            MoveAttributesToProto(format, objectType, selector, &(*object), subresponse->mutable_result());
        }
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, SelectObjects)
    {
        auto objectType = CheckedEnumCastToObjectType(request->object_type());
        auto timestamp = request->timestamp();

        auto filter = request->has_filter()
            ? std::make_optional(TObjectFilter{request->filter().query()})
            : std::nullopt;

        TSelectQueryOptions options;
        TAttributeSelector selector{
            FromProto<std::vector<TString>>(request->selector().paths())
        };

        options.Offset = request->has_offset()
            ? std::make_optional(request->offset().value())
            : std::nullopt;
        options.Limit = request->has_limit()
            ? std::make_optional(request->limit().value())
            : std::nullopt;

        context->SetRequestInfo("ObjectType: %v, Timestamp: %llx, Filter: %v, Selector: %v, Offset: %v, Limit: %v",
            objectType,
            timestamp,
            filter,
            selector,
            options.Offset,
            options.Limit);

        auto format = request->format();
        if (format == NClient::NApi::NProto::PF_NONE) {
            LogDeprecatedPayloadFormat(context);
        }

        auto authenticatedUserGuard = MakeAuthenticatedUserGuard(context);

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto transaction = WaitFor(transactionManager->StartReadOnlyTransaction(timestamp))
            .ValueOrThrow();

        auto result = transaction->ExecuteSelectQuery(
            objectType,
            filter,
            selector,
            options);

        response->mutable_results()->Reserve(result.Objects.size());
        for (auto& object : result.Objects) {
            auto* protoResult = response->add_results();
            MoveAttributesToProto(format, objectType, selector, &object, protoResult);
        }
        context->SetResponseInfo("Count: %v", result.Objects.size());
        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, CheckObjectPermissions)
    {
        auto timestamp = request->timestamp();

        context->SetRequestInfo("Timestamp: %llx, SubrequestCount: %v",
            timestamp,
            request->subrequests_size());

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto transaction = WaitFor(transactionManager->StartReadOnlyTransaction(timestamp))
            .ValueOrThrow();

        std::vector<TObject*> objects;
        for (const auto& subrequest : request->subrequests()) {
            auto objectType = CheckedEnumCastToObjectType(subrequest.object_type());
            const auto& objectId = subrequest.object_id();
            objects.push_back(transaction->GetObject(objectType, objectId));
        }

        const auto& accessControlManager = Bootstrap_->GetAccessControlManager();
        for (int index = 0; index < request->subrequests_size(); ++index) {
            const auto& subrequest = request->subrequests(index);
            const auto& subjectId = subrequest.subject_id();
            auto permission = CheckedEnumCast<EAccessControlPermission>(subrequest.permission());
            auto* object = objects[index];
            auto result = accessControlManager->CheckPermission(subjectId, object, permission);
            auto* subresponse = response->add_subresponses();
            subresponse->set_action(static_cast<NClient::NApi::NProto::EAccessControlAction>(result.Action));
            subresponse->set_object_id(result.ObjectId);
            subresponse->set_object_type(static_cast<NClient::NApi::NProto::EObjectType>(result.ObjectType));
            subresponse->set_subject_id(result.SubjectId);
        }

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, GetObjectAccessAllowedFor)
    {
        auto timestamp = request->timestamp();

        context->SetRequestInfo(
            "Timestamp: %llx, SubrequestCount: %v",
            timestamp,
            request->subrequests_size());

        const auto& transactionManager = Bootstrap_->GetTransactionManager();
        auto transaction = WaitFor(transactionManager->StartReadOnlyTransaction(timestamp))
            .ValueOrThrow();

        std::vector<TObject*> objects;
        for (const auto& subrequest : request->subrequests()) {
            auto objectType = CheckedEnumCastToObjectType(subrequest.object_type());
            const auto& objectId = subrequest.object_id();
            objects.push_back(transaction->GetObject(objectType, objectId));
        }

        const auto& accessControlManager = Bootstrap_->GetAccessControlManager();
        for (int index = 0; index < request->subrequests_size(); ++index) {
            const auto& subrequest = request->subrequests(index);
            auto permission = static_cast<EAccessControlPermission>(subrequest.permission());
            auto* object = objects[index];
            auto userIds = accessControlManager->GetObjectAccessAllowedFor(object, permission);
            auto* subresponse = response->add_subresponses();
            ToProto(subresponse->mutable_user_ids(), userIds);
        }

        context->Reply();
    }

    DECLARE_RPC_SERVICE_METHOD(NClient::NApi::NProto, GetUserAccessAllowedTo)
    {
        context->SetRequestInfo("SubrequestCount: %v", request->subrequests_size());

        const auto& accessControlManager = Bootstrap_->GetAccessControlManager();
        for (const auto& subrequest : request->subrequests()) {
            auto objectType = CheckedEnumCastToObjectType(subrequest.object_type());
            auto permission = CheckedEnumCast<NAccessControl::EAccessControlPermission>(
                subrequest.permission());
            auto objectIds = accessControlManager->GetUserAccessAllowedTo(
                subrequest.user_id(),
                objectType,
                permission);
            ToProto(response->add_subresponses()->mutable_object_ids(), objectIds);
        }

        context->Reply();
    }
};

IServicePtr CreateObjectService(TBootstrap* bootstrap)
{
    return New<TObjectService>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYP::NServer::NApi

