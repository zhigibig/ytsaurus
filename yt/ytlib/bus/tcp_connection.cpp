#include "stdafx.h"
#include "tcp_connection.h"
#include "tcp_dispatcher_impl.h"
#include "server.h"
#include "config.h"

#include <util/system/error.h>

#include <errno.h>

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger = BusLogger;
static NProfiling::TProfiler& Profiler = BusProfiler;

static const size_t ReadChunkSize = 16384;
static const size_t FragmentCountThreshold = 64;

static NProfiling::TAggregateCounter ReceiveTime("/receive_time");
static NProfiling::TAggregateCounter InHandlerTime("/in_handler_time");
static NProfiling::TRateCounter InThroughputCounter("/in_throughput");
static NProfiling::TRateCounter InCounter("/in_rate");

static NProfiling::TAggregateCounter SendTime("/send_time");
static NProfiling::TAggregateCounter OutHandlerTime("/out_handler_time");
static NProfiling::TRateCounter OutThroughputCounter("/out_throughput");
static NProfiling::TRateCounter OutCounter("/out_rate");
static NProfiling::TAggregateCounter PendingOutCounter("/pending_out_count");
static NProfiling::TAggregateCounter PendingOutSize("/pending_out_size");

////////////////////////////////////////////////////////////////////////////////

TTcpConnection::TTcpConnection(
    EConnectionType type,
    const TConnectionId& id,
    int socket,
    const Stroka& address,
    IMessageHandlerPtr handler)
    : Type(type)
    , Id(id)
    , Socket(socket)
    , Address(address)
    , Handler(handler)
    , State(EState::Opening)
    , ReadBuffer(ReadChunkSize)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YASSERT(handler);

    // Typically there are more than FragmentCountThreshold fragments.
    // This looks like a reasonable estimate.
    SendVector.reserve(FragmentCountThreshold * 2);

#ifdef _WIN32
    Fd = _open_osfhandle(Socket, 0);
#else
    Fd = Socket;
#endif

    UpdateConnectionCount(+1);
}

TTcpConnection::~TTcpConnection()
{
    CloseSocket();
    Cleanup();
}

void TTcpConnection::Cleanup()
{
    while (!QueuedPackets.empty()) {
        auto* packet = QueuedPackets.front();
        UpdatePendingOut(-1, -packet->Size);
        delete packet;
        QueuedPackets.pop();
    }

    while (!EncodedPackets.empty()) {
        auto* packet = EncodedPackets.front();
        UpdatePendingOut(-1, -packet->Packet->Size);
        delete packet->Packet;
        delete packet;
        EncodedPackets.pop();
    }

    EncodedFragments.clear();
}

void TTcpConnection::SyncInitialize()
{
    VERIFY_THREAD_AFFINITY(EventLoop);
    YASSERT(State == EState::Opening);

    const auto& eventLoop = TTcpDispatcher::TImpl::Get()->GetEventLoop();
    
    TerminationWatcher.Reset(new ev::async(eventLoop));
    TerminationWatcher->set<TTcpConnection, &TTcpConnection::OnTerminated>(this);
    TerminationWatcher->start();

    SocketWatcher.Reset(new ev::io(eventLoop));
    SocketWatcher->set<TTcpConnection, &TTcpConnection::OnSocket>(this);
    SocketWatcher->start(Fd, ev::READ|ev::WRITE);

    OutcomingMessageWatcher.Reset(new ev::async(eventLoop));
    OutcomingMessageWatcher->set<TTcpConnection, &TTcpConnection::OnOutcomingMessage>(this);
    OutcomingMessageWatcher->start();

    if (Type == EConnectionType::Server) {
        SyncOpen();
    }
}

void TTcpConnection::SyncFinalize()
{
    SyncClose(TError("Bus terminated"));
}

Stroka TTcpConnection::GetLoggingId() const
{
    return Sprintf("ConnectionId: %s", ~Id.ToString());
}

TTcpDispatcherStatistics& TTcpConnection::Statistics()
{
    return TTcpDispatcher::TImpl::Get()->Statistics();
}

void TTcpConnection::UpdateConnectionCount(int delta)
{
    switch (Type) {
        case EConnectionType::Client: {
            int value = (Statistics().ClientConnectionCount += delta);
            Profiler.Enqueue("/client_connection_count", value);
            break;
        }

        case EConnectionType::Server: {
            int value = (Statistics().ServerConnectionCount += delta);
            Profiler.Enqueue("/server_connection_count", value);
            break;
        }

        default:
            YUNREACHABLE();
    }
}

void TTcpConnection::UpdatePendingOut(int countDelta, i64 sizeDelta)
{
    {
        int value = (Statistics().PendingOutCount += countDelta);
        Profiler.Aggregate(PendingOutCounter, value);
    }
    {
        size_t value = (Statistics().PendingOutSize += sizeDelta);
        Profiler.Aggregate(PendingOutSize, value);
    }
}

const TConnectionId& TTcpConnection::GetId() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return Id;
}

void TTcpConnection::SyncOpen()
{
    {
        TGuard<TSpinLock> guard(SpinLock);
        State = EState::Open;
    }

    LOG_INFO("Connection established (ConnectionId: %s, Address: %s)",
        ~Id.ToString(),
        ~Address);

    // Flush messages that were enqueued when the connection was still opening.
    OnOutcomingMessage(*OutcomingMessageWatcher, 0);

    UpdateSocketWatcher();
}

void TTcpConnection::SyncClose(const TError& error)
{
    VERIFY_THREAD_AFFINITY(EventLoop);
    YCHECK(!error.IsOK());

    // Check for second close attempt.
    {
        TGuard<TSpinLock> guard(SpinLock);
        if (State == EState::Closed) {
            return;
        }
        State = EState::Closed;
    }

    // Stop all watchers.
    TerminationWatcher.Destroy();
    SocketWatcher.Destroy();
    OutcomingMessageWatcher.Destroy();

    // Close the socket.
    CloseSocket();

    // Mark all unacked messages as failed.
    while (!UnackedMessages.empty()) {
        UnackedMessages.front().Promise.Set(ESendResult(ESendResult::Failed));
        UnackedMessages.pop();
    }

    // Mark all queued messages as failed.
    {
        TQueuedMessage queuedMessage;
        while (QueuedMessages.Dequeue(&queuedMessage)) {
            queuedMessage.Promise.Set(ESendResult(ESendResult::Failed));
        }
    }
    
    // Release memory.
    Cleanup();

    // Invoke user callback.
    PROFILE_TIMING ("/terminate_handler_time") {
        Terminated_.Fire(error);
    }
    Terminated_.Clear();

    LOG_INFO("Connection closed (ConnectionId: %s)\n%s",
        ~Id.ToString(),
        ~error.ToString());

    UpdateConnectionCount(-1);

    TTcpDispatcher::TImpl::Get()->AsyncUnregister(this);
}

void TTcpConnection::CloseSocket()
{
    if (Fd != INVALID_SOCKET) {
        close(Fd);
    }
    Socket = INVALID_SOCKET;
    Fd = INVALID_SOCKET;
}

IBus::TSendResult TTcpConnection::Send(IMessagePtr message)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TQueuedMessage queuedMessage(message);
    QueuedMessages.Enqueue(queuedMessage);

    // We perform state check _after_ the message is already enqueued.
    // Other option would be to call |Enqueue| under spinlock, but this
    // ruins the idea to be lock-free.
    {
        TGuard<TSpinLock> guard(SpinLock);
        switch (State) {
            case EState::Open:
                OutcomingMessageWatcher->send();
                break;

            case EState::Closed:
                guard.Release();
                // Try to remove the message. 
                // This might not be the exact same message we've just enqueued
                // but still enables to keep the queue empty.
                QueuedMessages.Dequeue(&queuedMessage);
                LOG_DEBUG("Outcoming message via closed bus is dropped (ConnectionId: %s)",
                    ~Id.ToString());
                return MakeFuture(ESendResult(ESendResult::Failed));
        }
    }
   
    LOG_DEBUG("Outcoming message enqueued (ConnectionId: %s, PacketId: %s)",
        ~Id.ToString(),
        ~queuedMessage.PacketId.ToString());

    return queuedMessage.Promise;
}

void TTcpConnection::Terminate(const TError& error)
{
    VERIFY_THREAD_AFFINITY_ANY();
    YCHECK(!error.IsOK());

    {
        // Check if the connection is already closed or
        // another termination request is already in progress.
        TGuard<TSpinLock> guard(SpinLock);
        if (State == EState::Closed) {
            return;
        }
        if (!TerminationError.IsOK()) {
            return;
        }
        TerminationError = error;
        if (State == EState::Open) {
            TerminationWatcher->send();
        }
    }

    LOG_DEBUG("Bus termination requested (ConnectionId: %s)", ~Id.ToString());
}

void TTcpConnection::OnSocket(ev::io&, int revents)
{
    VERIFY_THREAD_AFFINITY(EventLoop);
    YASSERT(State != EState::Closed);
    
    if (revents & ev::ERROR) {
        SyncClose(TError("Socket failed"));
        return;
    }

    if (revents & ev::WRITE) {
        OnSocketWrite();
    }

    if (revents & ev::READ) {
        OnSocketRead();
    }

    UpdateSocketWatcher();
}

void TTcpConnection::OnSocketRead() 
{
    if (State == EState::Closed) {
        return;
    }

    LOG_TRACE("Started serving read request");
    size_t bytesReadTotal = 0;

    while (true) {
        // Check if the decoder is expecting a chunk of large enough size.
        auto decoderChunk = Decoder.GetChunk();
        LOG_TRACE("Decoder needs %" PRISZT " bytes", decoderChunk.Size());
        if (decoderChunk.Size() >= ReadChunkSize) {
            // Read directly into the decoder buffer.
            LOG_TRACE("Reading %" PRISZT " bytes into decoder", decoderChunk.Size());
            size_t bytesRead;
            if (!ReadSocket(decoderChunk.Begin(), decoderChunk.Size(), &bytesRead)) {
                break;
            }
            bytesReadTotal += bytesRead;

            if (!AdvanceDecoder(bytesRead)) {
                return;
            }
        } else {
            // Read a chunk into the read buffer.
            LOG_TRACE("Reading %" PRISZT " bytes into buffer", ReadBuffer.size());
            size_t bytesRead;
            if (!ReadSocket(&*ReadBuffer.begin(), ReadBuffer.size(), &bytesRead)) {
                break;
            }
            bytesReadTotal += bytesRead;

            // Feed the read buffer to the decoder.
            const char* recvBegin = &*ReadBuffer.begin();
            const char* recvEnd = recvBegin + bytesRead;
            size_t recvRemaining = bytesRead;
            while (recvRemaining != 0) {
                decoderChunk = Decoder.GetChunk();
                size_t advanceSize = std::min(recvRemaining, decoderChunk.Size());
                LOG_TRACE("Decoder chunk size is %" PRISZT " bytes, advancing %" PRISZT " bytes",
                    decoderChunk.Size(),
                    advanceSize);
                std::copy(recvBegin, recvBegin + advanceSize, decoderChunk.Begin());
                if (!AdvanceDecoder(advanceSize)) {
                    return;
                }
                recvBegin += advanceSize;
                recvRemaining -= advanceSize;
            }
            LOG_TRACE("Buffer exhausted");
        }
    }

    LOG_TRACE("Finished serving read request, %" PRISZT " bytes read total", bytesReadTotal);
}

bool TTcpConnection::ReadSocket(char* buffer, size_t size, size_t* bytesRead)
{
    ssize_t result;
    PROFILE_AGGREGATED_TIMING (ReceiveTime) {
        result = recv(Socket, buffer, size, 0);
    }

    if (!CheckReadError(result)) {
        return false;
    }

    Profiler.Increment(InThroughputCounter, result);
    *bytesRead = result;
    
    LOG_TRACE("%" PRISZT " bytes read", *bytesRead);
    
    return true;
}

bool TTcpConnection::CheckReadError(ssize_t result)
{
    if (result == 0) {
        SyncClose(TError("Socket was closed"));
        return false;
    }

    if (result < 0) {
        int error = LastSystemError();
        if (IsSocketError(error)) {
            LOG_WARNING("Socket read error (ConnectionId: %s, ErrorCode: %d)\n%s",
                ~Id.ToString(),
                error,
                LastSystemErrorText(error));
            SyncClose(TError("Socket read error"));
        }
        return false;
    }

    return true;
}

bool TTcpConnection::AdvanceDecoder(size_t size)
{
    if (!Decoder.Advance(size)) {
        SyncClose(TError("Error decoding incoming packet"));
        return false;
    }

    if (Decoder.IsFinished()) {
        bool result = OnPacketReceived();
        Decoder.Restart();
        return result;
    }

    return true;
}

bool TTcpConnection::OnPacketReceived()
{
    Profiler.Increment(InCounter);
    switch (Decoder.GetPacketType()) {
        case EPacketType::Ack:
            return OnAckPacketReceived();
        case EPacketType::Message:
            return OnMessagePacketReceived();
        default:
            YUNREACHABLE();
    }
}

bool TTcpConnection::OnAckPacketReceived()
{
    if (UnackedMessages.empty()) {
        LOG_ERROR("Unexpected ack received (ConnectionId: %s)", ~Id.ToString());
        SyncClose(TError("Unexpected ack received"));
        return false;
    }

    auto& unackedMessage = UnackedMessages.front();

    if (Decoder.GetPacketId() != unackedMessage.PacketId) {
        LOG_ERROR("Ack for invalid packet ID received: expected %s, found %s (ConnectionId: %s)",
            ~unackedMessage.PacketId.ToString(),
            ~Decoder.GetPacketId().ToString(),
            ~Id.ToString());
        SyncClose(TError("Ack for invalid packet ID received"));
        return false;
    }

    LOG_DEBUG("Ack received (ConnectionId: %s, PacketId: %s)",
        ~Id.ToString(),
        ~Decoder.GetPacketId().ToString());

    PROFILE_AGGREGATED_TIMING (OutHandlerTime) {
        unackedMessage.Promise.Set(ESendResult::OK);
    }

    UnackedMessages.pop();

    return true;
}

bool TTcpConnection::OnMessagePacketReceived()
{
    LOG_DEBUG("Incoming message received (ConnectionId: %s, PacketId: %s, PacketSize: %" PRISZT ")",
        ~Id.ToString(),
        ~Decoder.GetPacketId().ToString(),
        Decoder.GetPacketSize());

    EnqueuePacket(EPacketType::Ack, Decoder.GetPacketId());

    auto message = Decoder.GetMessage();
    PROFILE_AGGREGATED_TIMING (InHandlerTime) {
        Handler->OnMessage(message, this);
    }

    return true;
}

void TTcpConnection::EnqueuePacket(EPacketType type, const TPacketId& packetId, IMessagePtr message)
{
    size_t size = TPacketEncoder::GetPacketSize(type, message);
    QueuedPackets.push(new TQueuedPacket(type, packetId, message, size));
    UpdatePendingOut(+1, +size);
    EncodeMoreFragments();
}

void TTcpConnection::OnSocketWrite()
{
    if (State == EState::Closed) {
        return;
    }

    // For client sockets the first write notification means that
    // connection was established (either successfully or not).
    if (Type == EConnectionType::Client && State == EState::Opening) {
        // Check if connection was established successfully.
        int error = GetSocketError();
        if (error != 0) {
            LOG_ERROR("Failed to connect to %s (ConnectionId: %s, ErrorCode: %d)\n%s",
                ~Address,
                ~Id.ToString(),
                error,
                LastSystemErrorText(error));

            // We're currently in event loop context, so calling |SyncClose| is safe.
            SyncClose(TError("Failed to connect to %s (ErrorCode: %d)\n%s",
                ~Address,
                error,
                LastSystemErrorText(error)));

            return;
        }
        SyncOpen();
    }

    LOG_TRACE("Started serving write request");
    size_t bytesWrittenTotal = 0;

    while (HasUnsentData()) {
        size_t bytesWritten;
        bool success = WriteFragments(&bytesWritten);
        bytesWrittenTotal += bytesWritten;
        FlushWrittenFragments(bytesWritten);
        EncodeMoreFragments();
        if (!success) {
            break;
        }
    }

    LOG_TRACE("Finished serving write request, %" PRISZT " bytes written total", bytesWrittenTotal);
}

bool TTcpConnection::HasUnsentData() const
{
    return !EncodedFragments.empty();
}

bool TTcpConnection::WriteFragments(size_t* bytesWritten)
{
    int fragmentCount = static_cast<int>(EncodedFragments.size());
    SendVector.resize(fragmentCount);
    LOG_TRACE("Writing %d fragments", fragmentCount);

#ifdef _WIN32
    WSABUF* wsabuf = &*SendVector.begin();
    WSABUF* wsabufIt = wsabuf;
#else
    struct iovec* iovec = &*SendVector.begin();
    struct iovec* iovecIt = iovec;
#endif

    auto fragmentIt = EncodedFragments.begin();
    auto fragmentEnd = EncodedFragments.end();
    while (fragmentIt != fragmentEnd) {
#ifdef _WIN32
        wsabufIt->buf = fragmentIt->Data.Begin();
        wsabufIt->len = static_cast<ULONG>(fragmentIt->Data.Size());
        ++wsabufIt;
#else
        iovecIt->iov_base = fragmentIt->Data.Begin();
        iovecIt->iov_len = fragmentIt->Data.Size();
        ++iovecIt;
#endif
        ++fragmentIt;
    }

    ssize_t result;
#ifdef _WIN32
    DWORD bytesWritten_ = 0;
    PROFILE_AGGREGATED_TIMING (SendTime) {
        result = WSASend(Socket, wsabuf, fragmentCount, &bytesWritten_, 0, NULL, NULL);
    }
    *bytesWritten = static_cast<size_t>(bytesWritten_);
#else
    PROFILE_AGGREGATED_TIMING (SendTime) {
        result = writev(Socket, iovec, fragmentCount);
    }
    *bytesWritten = result >= 0 ? result : 0;
#endif

    Profiler.Increment(OutThroughputCounter, *bytesWritten);
    LOG_TRACE("%" PRISZT " bytes written", *bytesWritten);

    return CheckWriteError(result);
}

void TTcpConnection::FlushWrittenFragments(size_t bytesWritten)
{
    size_t bytesToFlush = bytesWritten;
    LOG_TRACE("Flushing %" PRISZT " written bytes", bytesWritten);

    while (bytesToFlush != 0) {
        YASSERT(!EncodedFragments.empty());
        auto& fragment = EncodedFragments.front();

        auto& data = fragment.Data;
        if (data.Size() > bytesToFlush) {
            size_t bytesRemaining = data.Size() - bytesToFlush;
            LOG_TRACE("Partial write (Size: %" PRISZT ", RemainingSize: %" PRISZT ")",
                data.Size(),
                bytesRemaining);
            fragment.Data = TRef(data.End() - bytesRemaining, bytesRemaining);
            break;
        }

        LOG_TRACE("Full write (Size: %" PRISZT ")", data.Size());

        if (fragment.IsLastInPacket) {
            OnPacketSent();
        }

        bytesToFlush -= data.Size();
        EncodedFragments.pop_front();
    }
}

bool TTcpConnection::EncodeMoreFragments()
{
    while (EncodedFragments.size() < FragmentCountThreshold && !QueuedPackets.empty()) {
        // Move the packet from queued to encoded.
        auto* queuedPacket = QueuedPackets.front();
        QueuedPackets.pop();

        auto* encodedPacket = new TEncodedPacket();
        EncodedPackets.push(encodedPacket);

        encodedPacket->Packet = queuedPacket;

        // Encode the packet.
        LOG_TRACE("Started encoding packet");

        auto& encoder = encodedPacket->Encoder;
        if (!encoder.Start(queuedPacket->Type, queuedPacket->PacketId, queuedPacket->Message)) {
            SyncClose(TError("Error encoding outcoming packet"));
            return false;
        }

        TEncodedFragment fragment;
        do {
            fragment.Data = encoder.GetChunk();
            encoder.NextChunk();
            fragment.IsLastInPacket = encoder.IsFinished();
            EncodedFragments.push_back(fragment);
            LOG_TRACE("Fragment encoded (Size: %" PRISZT ", IsLast: %d)",
                fragment.Data.Size(),
                fragment.IsLastInPacket);
        } while (!fragment.IsLastInPacket);

        LOG_TRACE("Finished encoding packet");
    }
    return true;
}

bool TTcpConnection::CheckWriteError(ssize_t result)
{
    if (result < 0) {
        int error = LastSystemError();
        if (IsSocketError(error)) {
            LOG_WARNING("Socket write error (ConnectionId: %s, ErrorCode: %d)\n%s",
                ~Id.ToString(),
                error,
                LastSystemErrorText(error));
            SyncClose(TError("Socket write error"));
        }
        return false;
    }

    return true;
}

void TTcpConnection::OnPacketSent()
{
    const auto* packet = EncodedPackets.front();
    switch (packet->Packet->Type) {
        case EPacketType::Ack:
            OnAckPacketSent(*packet);
            break;

        case EPacketType::Message:
            OnMessagePacketSent(*packet);
            break;

        default:
            YUNREACHABLE();
    }


    UpdatePendingOut(-1, -packet->Packet->Size);
    Profiler.Increment(OutCounter);

    delete packet->Packet;
    delete packet;
    EncodedPackets.pop();
}

void  TTcpConnection::OnAckPacketSent(const TEncodedPacket& packet)
{
    LOG_DEBUG("Ack sent (ConnectionId: %s, PacketId: %s)",
        ~Id.ToString(),
        ~packet.Packet->PacketId.ToString());
}

void TTcpConnection::OnMessagePacketSent(const TEncodedPacket& packet)
{
    LOG_DEBUG("Outcoming message sent (ConnectionId: %s, PacketId: %s, PacketSize: %" PRISZT ")",
        ~Id.ToString(),
        ~packet.Packet->PacketId.ToString(),
        packet.Packet->Size);
}

void TTcpConnection::OnOutcomingMessage(ev::async&, int)
{
    VERIFY_THREAD_AFFINITY(EventLoop);
    YASSERT(State != EState::Closed);

    TQueuedMessage queuedMessage;
    while (QueuedMessages.Dequeue(&queuedMessage)) {
        LOG_DEBUG("Outcoming message dequeued (ConnectionId: %s, PacketId: %s)",
            ~Id.ToString(),
            ~queuedMessage.PacketId.ToString());

        EnqueuePacket(EPacketType::Message, queuedMessage.PacketId, queuedMessage.Message);

        TUnackedMessage unackedMessage(queuedMessage.PacketId, queuedMessage.Promise);
        UnackedMessages.push(unackedMessage);
    }

    UpdateSocketWatcher();
}

void TTcpConnection::UpdateSocketWatcher()
{
    if (State == EState::Open) {
        SocketWatcher->set(HasUnsentData() ? ev::READ|ev::WRITE : ev::READ);
    }
}

void TTcpConnection::OnTerminated(ev::async&, int)
{
    VERIFY_THREAD_AFFINITY(EventLoop);
    YASSERT(State != EState::Closed);

    TError error;
    {
        TGuard<TSpinLock> guard(SpinLock);
        error = TerminationError;
    }

    SyncClose(error);
}

int TTcpConnection::GetSocketError() const
{
    int error;
    socklen_t errorLen = sizeof (error);
    getsockopt(Socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorLen);
    return error;
}

bool TTcpConnection::IsSocketError(ssize_t result)
{
#ifdef _WIN32
    return result != WSAEWOULDBLOCK && result != WSAEINTR;
#else
    return result != EWOULDBLOCK && result != EAGAIN && result != EINTR;
#endif
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT

