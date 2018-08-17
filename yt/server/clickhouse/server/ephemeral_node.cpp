#include "ephemeral_node.h"

#include "attributes_helpers.h"
#include "backoff.h"
#include "private.h"

#include <yt/core/ytree/convert.h>
#include <yt/ytlib/object_client/public.h>

#include <yt/client/api/client.h>
#include <yt/client/api/transaction.h>

#include <util/generic/guid.h>

namespace NYT {
namespace NClickHouse {

using namespace NYT::NApi;
using namespace NYT::NConcurrency;
using namespace NYT::NYTree;

namespace {

////////////////////////////////////////////////////////////////////////////////

bool IsNodeNotFound(const TError& error)
{
   return error.GetCode() == NYTree::EErrorCode::ResolveError;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TEphemeralNodeKeeper);

class TEphemeralNodeKeeper
    : public TRefCounted
{
private:
    TString DirectoryPath;
    TString NameHint;
    TString Content;
    TDuration SessionTimeout;

    IClientPtr Client;

    TString NodePath;

    TBackoff Backoff;

    const NLogging::TLogger& Logger = ServerLogger;

public:
    TEphemeralNodeKeeper(
        TString directoryPath,
        TString nameHint,
        TString content,
        TDuration lifetime,
        IClientPtr client);

private:
    void CreateNode();
    TString GenerateUniqueNodeName() const;
    TErrorOr<void> TryCreateNode(const TString& nodePath);
    void CreateNodeLater(const TDuration& delay);

    void TouchNode();
    TErrorOr<void> TryTouchNode();
    void TouchNodeLater(const TDuration& delay);

    TInstant GetExpirationTimeFromNow() const;

    TDuration GetNextTouchDelay() const;
};

DEFINE_REFCOUNTED_TYPE(TEphemeralNodeKeeper);

////////////////////////////////////////////////////////////////////////////////

TEphemeralNodeKeeper::TEphemeralNodeKeeper(
    TString directoryPath,
    TString nameHint,
    TString content,
    TDuration sessionTimeout,
    IClientPtr client)
    : DirectoryPath(std::move(directoryPath))
    , NameHint(std::move(nameHint))
    , Content(std::move(content))
    , SessionTimeout(sessionTimeout)
    , Client(std::move(client))
{
    CreateNode();
}

void TEphemeralNodeKeeper::CreateNode()
{
    auto nodeName = GenerateUniqueNodeName();
    auto nodePath = TString::Join(DirectoryPath, '/', nodeName);

    auto result = TryCreateNode(nodePath);
    if (result.IsOK()) {
        Backoff.Reset();
        NodePath = nodePath;
        LOG_DEBUG("Ephemeral node %Qv created", nodePath);
        TouchNodeLater(GetNextTouchDelay());
    } else {
        LOG_WARNING("Cannot create ephemeral node %Qv in %Qv, retry later...", NameHint, DirectoryPath);
        CreateNodeLater(Backoff.GetNextPause());
    }
}

TString TEphemeralNodeKeeper::GenerateUniqueNodeName() const
{
    return TString::Join(NameHint, '-', CreateGuidAsString());
}

TErrorOr<void> TEphemeralNodeKeeper::TryCreateNode(const TString& nodePath)
{
    auto startTxResult = WaitFor(Client->StartTransaction(
        NTransactionClient::ETransactionType::Master));

    if (!startTxResult.IsOK()) {
        return TError(startTxResult);
    }

    auto txClient = startTxResult.ValueOrThrow();

    TCreateNodeOptions createOptions;
    createOptions.Recursive = false;
    createOptions.IgnoreExisting = false;

    auto nodeAttributes = CreateEphemeralAttributes();
    nodeAttributes->Set("expiration_time", GetExpirationTimeFromNow());
    createOptions.Attributes = std::move(nodeAttributes);

    auto createResult = WaitFor(txClient->CreateNode(
        nodePath,
        NObjectClient::EObjectType::StringNode,
        createOptions));

    if (!createResult.IsOK()) {
        return TError(createResult);
    }

    auto setResult = WaitFor(txClient->SetNode(
        nodePath,
        ConvertToYsonString(Content)));

    if (!setResult.IsOK()) {
        return TError(setResult);
    }

    return WaitFor(txClient->Commit());
}

void TEphemeralNodeKeeper::CreateNodeLater(const TDuration& delay)
{
    TDelayedExecutor::Submit(
        BIND(&TEphemeralNodeKeeper::CreateNode, MakeWeak(this)),
        delay);
}

void TEphemeralNodeKeeper::TouchNode()
{
    auto result = TryTouchNode();
    if (result.IsOK()) {
        Backoff.Reset();
        LOG_DEBUG("Ephemeral node %Qv touched", NodePath);
        TouchNodeLater(GetNextTouchDelay());
    } else if (IsNodeNotFound(result)) {
        LOG_WARNING("Ephemeral node %Qv (%Qv) lost, recreate it", NodePath, NameHint);
        CreateNode();
    } else {
        LOG_WARNING(result);
        TouchNodeLater(Backoff.GetNextPause());
    }
}

TErrorOr<void> TEphemeralNodeKeeper::TryTouchNode()
{
    return WaitFor(Client->SetNode(
        TString::Join(NodePath, "/@expiration_time"),
        ConvertToYsonString(GetExpirationTimeFromNow())));
}

void TEphemeralNodeKeeper::TouchNodeLater(const TDuration& delay)
{
    TDelayedExecutor::Submit(
        BIND(&TEphemeralNodeKeeper::TouchNode, MakeWeak(this)),
        delay);
}

TInstant TEphemeralNodeKeeper::GetExpirationTimeFromNow() const
{
    return Now() + SessionTimeout;
}

TDuration TEphemeralNodeKeeper::GetNextTouchDelay() const
{
    return AddJitter(SessionTimeout * 0.5, /*jitter=*/ 0.2);
}

////////////////////////////////////////////////////////////////////////////////

class TEphemeralNodeKeeperHolder
    : public NInterop::IEphemeralNodeKeeper
{
public:
    TEphemeralNodeKeeperHolder(
        TEphemeralNodeKeeperPtr nodeKeeper)
        : NodeKeeper(std::move(nodeKeeper))
    {}

    void Release() override
    {
        NodeKeeper.Reset();
    }

private:
    TEphemeralNodeKeeperPtr NodeKeeper;
};

////////////////////////////////////////////////////////////////////////////////

NInterop::IEphemeralNodeKeeperPtr CreateEphemeralNodeKeeper(
    NApi::IClientPtr client,
    TString directoryPath,
    TString nameHint,
    TString content,
    TDuration sessionTimeout)
{
    auto nodeKeeper = New<TEphemeralNodeKeeper>(
        std::move(directoryPath),
        std::move(nameHint),
        std::move(content),
        sessionTimeout,
        std::move(client));

    return std::make_unique<TEphemeralNodeKeeperHolder>(
       std::move(nodeKeeper));
}

}   // namespace NClickHouse
}   // namespace NYT
