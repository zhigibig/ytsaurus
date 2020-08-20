#include <yt/core/test_framework/framework.h>

#include <yt/core/bus/bus.h>
#include <yt/core/bus/client.h>
#include <yt/core/bus/server.h>

#include <yt/core/bus/tcp/config.h>
#include <yt/core/bus/tcp/client.h>
#include <yt/core/bus/tcp/server.h>

#include <yt/core/net/socket.h>

#include <yt/core/concurrency/event_count.h>

#include <yt/core/misc/fs.h>

#include <library/cpp/testing/unittest/tests_data.h>

namespace NYT::NBus {
namespace {

////////////////////////////////////////////////////////////////////////////////

TSharedRefArray CreateMessage(int numParts)
{
    auto data = TSharedMutableRef::Allocate(numParts);

    std::vector<TSharedRef> parts;
    for (int i = 0; i < numParts; ++i) {
        parts.push_back(data.Slice(i, i + 1));
    }

    return TSharedRefArray(std::move(parts), TSharedRefArray::TMoveParts{});
}

TSharedRefArray Serialize(TString str)
{
    return TSharedRefArray(TSharedRef::FromString(str));
}

TString Deserialize(TSharedRefArray message)
{
    YT_ASSERT(message.Size() == 1);
    const auto& part = message[0];
    return TString(part.Begin(), part.Size());
}

////////////////////////////////////////////////////////////////////////////////

class TEmptyBusHandler
    : public IMessageHandler
{
public:
    virtual void HandleMessage(
        TSharedRefArray message,
        IBusPtr replyBus) noexcept override
    {
        Y_UNUSED(message);
        Y_UNUSED(replyBus);
    }
};

class TReplying42BusHandler
    : public IMessageHandler
{
public:
    TReplying42BusHandler(int numParts)
        : NumPartsExpecting(numParts)
    { }

    virtual void HandleMessage(
        TSharedRefArray message,
        IBusPtr replyBus) noexcept override
    {
        EXPECT_EQ(NumPartsExpecting, message.Size());
        auto replyMessage = Serialize("42");
        replyBus->Send(replyMessage, NBus::TSendOptions(EDeliveryTrackingLevel::None));
    }
private:
    int NumPartsExpecting;
};

class TChecking42BusHandler
    : public IMessageHandler
{
public:
    explicit TChecking42BusHandler(int numRepliesWaiting)
        : NumRepliesWaiting(numRepliesWaiting)
    { }

    void WaitUntilDone()
    {
        Event_.Wait();
    }

private:
    std::atomic<int> NumRepliesWaiting;
    NConcurrency::TEvent Event_;


    virtual void HandleMessage(
        TSharedRefArray message,
        IBusPtr /*replyBus*/) noexcept override
    {
        auto value = Deserialize(message);
        EXPECT_EQ("42", value);

        if (--NumRepliesWaiting == 0) {
            Event_.NotifyAll();
        }
    }

};

////////////////////////////////////////////////////////////////////////////////

class TBusTest
    : public testing::Test
{
public:
    ui16 Port;
    TString Address;
    TPortManager PortManager;

    TBusTest()
    {
#ifdef _darwin_
        // XXX(babenko): PortManager is based on SO_REUSEPORT, and I failed to make it work for MacOS and our Bus layer.
        // Must revisit this issue sometime later.
        Port = 1234;
#else
        Port = PortManager.GetPort();
#endif
        Address = Format("localhost:%v", Port);
    }

    IBusServerPtr StartBusServer(IMessageHandlerPtr handler)
    {
        auto config = TTcpBusServerConfig::CreateTcp(Port);
        auto server = CreateTcpBusServer(config);
        server->Start(handler);
        return server;
    }

    void TestReplies(int numRequests, int numParts, EDeliveryTrackingLevel level = EDeliveryTrackingLevel::Full)
    {
        auto server = StartBusServer(New<TReplying42BusHandler>(numParts));
        auto client = CreateTcpBusClient(TTcpBusClientConfig::CreateTcp(Address));
        auto handler = New<TChecking42BusHandler>(numRequests);
        auto bus = client->CreateBus(handler);
        auto message = CreateMessage(numParts);

        std::vector<TFuture<void>> results;
        for (int i = 0; i < numRequests; ++i) {
            auto result = bus->Send(message, NBus::TSendOptions(level));
            if (result) {
                results.push_back(result);
            }
        }

        for (const auto& result : results) {
            auto error = result.Get();
            EXPECT_TRUE(error.IsOK());
        }

        handler->WaitUntilDone();

        server->Stop()
            .Get()
            .ThrowOnError();
    }
};

////////////////////////////////////////////////////////////////////////////////

TEST_F(TBusTest, ConfigDefaultConstructor)
{
    auto config = New<TTcpBusClientConfig>();
}

TEST_F(TBusTest, CreateTcpBusClientConfig)
{
    auto config = TTcpBusClientConfig::CreateTcp(Address);
    EXPECT_EQ(Address, *config->Address);
    EXPECT_FALSE(config->UnixDomainSocketPath);
}

TEST_F(TBusTest, CreateUnixDomainBusClientConfig)
{
    auto config = TTcpBusClientConfig::CreateUnixDomain("unix-socket");
    EXPECT_EQ("unix-socket", *config->UnixDomainSocketPath);
}

TEST_F(TBusTest, OK)
{
    auto server = StartBusServer(New<TEmptyBusHandler>());
    auto client = CreateTcpBusClient(TTcpBusClientConfig::CreateTcp(Address));
    auto bus = client->CreateBus(New<TEmptyBusHandler>());
    auto message = CreateMessage(1);
    auto result = bus->Send(message, NBus::TSendOptions(EDeliveryTrackingLevel::Full))
        .Get();
    EXPECT_TRUE(result.IsOK());
    server->Stop()
        .Get()
        .ThrowOnError();
}

TEST_F(TBusTest, Terminate)
{
    auto server = StartBusServer(New<TEmptyBusHandler>());
    auto client = CreateTcpBusClient(TTcpBusClientConfig::CreateTcp(Address));
    auto bus = client->CreateBus(New<TEmptyBusHandler>());
    auto message = CreateMessage(1);

    auto terminated = NewPromise<void>();
    bus->SubscribeTerminated(
        BIND([&] (const TError& error) {
            terminated.Set(error);
        }));
    auto error = TError(54321, "Terminated");
    bus->Terminate(error);
    bus->Terminate(TError(12345, "Ignored"));
    EXPECT_EQ(terminated.Get().GetCode(), error.GetCode());
    bus->Terminate(TError(12345, "Ignored"));

    auto result = bus->Send(message, NBus::TSendOptions(EDeliveryTrackingLevel::Full));
    EXPECT_FALSE(result.IsSet());
    bus.Reset(); // Destructor discards message queue
    EXPECT_EQ(result.Get().GetCode(), error.GetCode());

    server->Stop()
        .Get()
        .ThrowOnError();
}

TEST_F(TBusTest, TerminateBeforeAccept)
{
    /* make blocking server socket */
    auto serverSocket = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    EXPECT_NE(serverSocket, INVALID_SOCKET);
    NNet::SetReuseAddrFlag(serverSocket);
    NNet::BindSocket(serverSocket, NNet::TNetworkAddress::CreateIPv6Loopback(Port));
    NNet::ListenSocket(serverSocket, 0);

    auto client = CreateTcpBusClient(TTcpBusClientConfig::CreateTcp(Address, "non-local"));
    auto bus = client->CreateBus(New<TEmptyBusHandler>());

    auto terminated = NewPromise<void>();
    bus->SubscribeTerminated(
        BIND([&] (const TError& error) {
            terminated.Set(error);
        }));
    auto error = TError(54321, "Terminated");
    bus->Terminate(error);
    EXPECT_FALSE(terminated.IsSet());

    NNet::TNetworkAddress clientAddress;
    auto clientSocket = NNet::AcceptSocket(serverSocket, &clientAddress);
    EXPECT_NE(clientSocket, INVALID_SOCKET);

    EXPECT_EQ(terminated.Get().GetCode(), error.GetCode());

    close(clientSocket);
    close(serverSocket);
}

TEST_F(TBusTest, Failed)
{
    auto port = PortManager.GetPort();

    auto client = CreateTcpBusClient(TTcpBusClientConfig::CreateTcp(Format("localhost:%v", port)));
    auto bus = client->CreateBus(New<TEmptyBusHandler>());
    auto message = CreateMessage(1);
    auto result = bus->Send(message, NBus::TSendOptions(EDeliveryTrackingLevel::Full)).Get();
    EXPECT_FALSE(result.IsOK());
}

TEST_F(TBusTest, BlackHole)
{
    auto server = StartBusServer(New<TEmptyBusHandler>());
    auto config = TTcpBusClientConfig::CreateTcp(Address, "non-local");

    config->ReadStallTimeout = TDuration::Seconds(1);

    auto client = CreateTcpBusClient(config);
    auto bus = client->CreateBus(New<TEmptyBusHandler>());
    auto message = CreateMessage(1);
    auto options = TSendOptions(EDeliveryTrackingLevel::Full);

    bus->Send(message, options)
        .Get()
        .ThrowOnError();

    bus->SetTosLevel(BlackHoleTosLevel);

    auto result = bus->Send(message, options).Get();
    EXPECT_FALSE(result.IsOK());

    server->Stop()
        .Get()
        .ThrowOnError();
}

TEST_F(TBusTest, OneReplyNoTracking)
{
    TestReplies(1, 1, EDeliveryTrackingLevel::None);
}

TEST_F(TBusTest, OneReplyFullTracking)
{
    TestReplies(1, 1, EDeliveryTrackingLevel::Full);
}

TEST_F(TBusTest, OneReplyErrorOnlyTracking)
{
    TestReplies(1, 1, EDeliveryTrackingLevel::ErrorOnly);
}

TEST_F(TBusTest, ManyReplies)
{
    TestReplies(1000, 100);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NBus
