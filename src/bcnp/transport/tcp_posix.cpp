/**
 * @file tcp_posix.cpp
 * @brief TCP transport adapter implementation for POSIX systems.
 * 
 * Provides reliable, stream-oriented BCNP transport over TCP sockets.
 * Supports both server mode (listen/accept) and client mode (connect) with
 * automatic reconnection, non-blocking I/O, and V3 schema handshake validation.
 * 
 * @note This implementation is for Linux/POSIX systems only. For Windows,
 *       use a separate Winsock implementation.
 * 
 * 
 * @see TcpPosixAdapter
 */
#include "bcnp/transport/tcp_posix.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bcnp {

namespace {
/// @brief Interval between reconnection attempts for client mode.
constexpr auto kReconnectInterval = std::chrono::milliseconds(500);

/// @brief Minimum interval between error log messages to prevent spam.
constexpr auto kLogThrottle = std::chrono::seconds(1);
} // namespace

/**
 * @brief Constructs a TCP adapter in server or client mode.
 * 
 * @param listenPort Port to bind for server mode. Pass 0 for client mode.
 * @param targetIp Target IP address for client mode (ignored in server mode).
 * @param targetPort Target port for client mode (ignored in server mode).
 * 
 * Server mode (listenPort > 0):
 * Creates a listening socket on the specified port
 * Accepts one client at a time
 * Auto-disconnects zombie clients after timeout
 * 
 * Client mode (listenPort == 0, targetIp/targetPort set):
 * Initiates connect to the target
 * Automatically reconnects on disconnect
 */
TcpPosixAdapter::TcpPosixAdapter(uint16_t listenPort, const char* targetIp, uint16_t targetPort) {
    m_txBuffer = std::make_unique<uint8_t[]>(kTxBufferCapacity);

    if (listenPort > 0) {
        m_isServer = true;
        if (!CreateBaseSocket()) {
            return;
        }

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(listenPort);
        bindAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(m_socket, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0) {
            LogError("bind");
            ::close(m_socket);
            m_socket = -1;
            return;
        }

        if (listen(m_socket, 1) < 0) {
            LogError("listen");
            ::close(m_socket);
            m_socket = -1;
            return;
        }
    } else if (targetIp && targetPort > 0) {
        m_isServer = false;
        sockaddr_in targetAddr{};
        targetAddr.sin_family = AF_INET;
        targetAddr.sin_port = htons(targetPort);
        if (inet_pton(AF_INET, targetIp, &targetAddr.sin_addr) <= 0) {
            LogError("inet_pton (invalid target IP)");
            return;
        }

        m_peerAddr = targetAddr;
        m_peerAddrValid = true;
        BeginClientConnect(true);
    }
}

/**
 * @brief Destructor. Closes all open sockets.
 */
TcpPosixAdapter::~TcpPosixAdapter() {
    if (m_clientSocket >= 0) {
        ::close(m_clientSocket);
    }
    if (m_socket >= 0) {
        ::close(m_socket);
    }
}

/**
 * @brief Creates or recreates the base socket.
 * 
 * Closes any existing socket and creates a new one with SO_REUSEADDR enabled.
 * Used for server mode initialization and client reconnection.
 * 
 * @return true if socket was created successfully, false otherwise.
 */
bool TcpPosixAdapter::CreateBaseSocket() {
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }

    m_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        LogError("socket");
        return false;
    }

    int yes = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        LogError("setsockopt(SO_REUSEADDR)");
    }

    if (!ConfigureSocket(m_socket)) {
        ::close(m_socket);
        m_socket = -1;
        return false;
    }
    m_isConnected = false;
    m_connectInProgress = false;
    return true;
}

/**
 * @brief Initiates a non-blocking connect to the configured peer (client mode only).
 * 
 * Implements reconnection backoff: won't attempt reconnect until kReconnectInterval
 * has passed since the last attempt unless forceImmediate is set.
 * 
 * @param forceImmediate If true, bypass the reconnection interval throttle.
 */
void TcpPosixAdapter::BeginClientConnect(bool forceImmediate) {
    if (m_isServer || !m_peerAddrValid) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!forceImmediate && now < m_nextReconnectAttempt) {
        return;
    }
    m_nextReconnectAttempt = now + kReconnectInterval;

    if (!CreateBaseSocket()) {
        return;
    }

    if (connect(m_socket, reinterpret_cast<sockaddr*>(&m_peerAddr), sizeof(m_peerAddr)) < 0) {
        if (errno == EINPROGRESS || errno == EALREADY) {
            m_connectInProgress = true;
            return;
        }
        LogError("connect");
        ::close(m_socket);
        m_socket = -1;
        m_connectInProgress = false;
        m_isConnected = false;
        return;
    }

    // Synchronous connect succeeded (rare for non-blocking socket)
    m_isConnected = true;
    m_connectInProgress = false;
}

/**
 * @brief Polls connection state and handles accept/connect operations.
 * 
 * Server mode:
 * Checks for zombie client timeout
 * Accepts new client connections
 * 
 * Client mode:
 * Checks async connect completion
 * Initiates reconnection if disconnected
 */
void TcpPosixAdapter::PollConnection() {
    if (m_isServer) {
        if (m_socket < 0) {
            return;
        }

        if (m_clientSocket >= 0) {
            // Check for zombie client timeout
            const auto now = std::chrono::steady_clock::now();
            if (m_lastServerRx != std::chrono::steady_clock::time_point{} &&
                now - m_lastServerRx > m_serverClientTimeout) {
                ::close(m_clientSocket);
                m_clientSocket = -1;
                m_isConnected = false;
                m_lastServerRx = {};
                return;
            }
            m_isConnected = true;
            return;
        }

        sockaddr_in clientAddr{};
        socklen_t len = sizeof(clientAddr);
        int clientSock = ::accept(m_socket, reinterpret_cast<sockaddr*>(&clientAddr), &len);
        if (clientSock >= 0) {
            if (!ConfigureSocket(clientSock)) {
                ::close(clientSock);
                return;
            }
            m_clientSocket = clientSock;
            m_isConnected = true;
            m_lastServerRx = std::chrono::steady_clock::now();
            TryFlushTxBuffer(m_clientSocket);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LogError("accept");
        }
        return;
    }

    if (m_socket < 0) {
        BeginClientConnect(false);
        return;
    }

    if (!m_isConnected && m_connectInProgress) {
        int err = 0;
        socklen_t errLen = sizeof(err);
        if (getsockopt(m_socket, SOL_SOCKET, SO_ERROR, &err, &errLen) < 0) {
            LogError("getsockopt(SO_ERROR)");
            return;
        }

        if (err == 0) {
            m_isConnected = true;
            m_connectInProgress = false;
            TryFlushTxBuffer(m_socket);
            return;
        }

        if (err == EINPROGRESS || err == EALREADY) {
            return;
        }

        errno = err;
        LogError("connect (async)");
        ::close(m_socket);
        m_socket = -1;
        m_connectInProgress = false;
        BeginClientConnect(false);
        return;
    }

    if (!m_isConnected) {
        BeginClientConnect(false);
    }
}

/**
 * @brief Handles connection loss cleanup and reconnection initiation.
 * 
 * Resets handshake state, drops pending TX data, and closes sockets.
 * In client mode, triggers immediate reconnection attempt.
 */
void TcpPosixAdapter::HandleConnectionLoss() {
    m_isConnected = false;
    m_handshakeComplete = false;
    m_handshakeSent = false;
    m_schemaValidated = false;
    m_handshakeReceived = 0;
    m_remoteSchemaHash = 0;
    DropPendingTx();

    if (m_isServer) {
        if (m_clientSocket >= 0) {
            ::close(m_clientSocket);
            m_clientSocket = -1;
        }
        m_lastServerRx = {};
        return;
    }

    // Client mode - close broken socket and trigger reconnection
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }
    m_connectInProgress = false;
    BeginClientConnect(true);
}

/**
 * @brief Sends bytes through the TCP connection.
 * 
 * Data is buffered and sent asynchronously to prevent blocking on slow connections.
 * Implements congestion control by rejecting new packets when buffer exceeds 50% capacity.
 * 
 * @param data Pointer to byte buffer to send.
 * @param length Number of bytes to send.
 * @return true if data was accepted for sending, false if buffer full or not connected.
 */
bool TcpPosixAdapter::SendBytes(const uint8_t* data, std::size_t length) {
    if (!data || length == 0) {
        return true;
    }

    PollConnection();

    int targetSock = m_isServer ? m_clientSocket : m_socket;
    if (targetSock < 0 || !m_isConnected) {
        return false;
    }

    if (length > kTxBufferCapacity) {
        LogError("send payload exceeds tx buffer capacity");
        return false;
    }

    if (!EnqueueTx(data, length)) {
        return false;
    }

    TryFlushTxBuffer(targetSock);
    return true;
}

/**
 * @brief Receives bytes from the TCP connection.
 * 
 * Performs non-blocking receive. Handles V3 handshake protocol transparently -
 * handshake bytes are consumed internally and not returned to the caller.
 * 
 * @param buffer Destination buffer for received data.
 * @param maxLength Maximum bytes to receive.
 * @return Number of bytes received (0 if no data or error).
 */
std::size_t TcpPosixAdapter::ReceiveChunk(uint8_t* buffer, std::size_t maxLength) {
    if (m_socket < 0 || !buffer || maxLength == 0) {
        return 0;
    }

    PollConnection();

    int targetSock = m_isServer ? m_clientSocket : m_socket;
    if (targetSock < 0 || !m_isConnected) {
        return 0;
    }

    TryFlushTxBuffer(targetSock);
    
    // Send handshake if connected but not sent yet
    if (!m_handshakeSent) {
        SendHandshake();
    }

    ssize_t received;
    do {
        received = ::recv(targetSock, buffer, maxLength, 0);
    } while (received < 0 && errno == EINTR);

    if (received > 0) {
        if (m_isServer) {
            m_lastServerRx = std::chrono::steady_clock::now();
        }
        
        // Process handshake if not complete
        if (!m_handshakeComplete) {
            std::size_t consumed = std::min(static_cast<std::size_t>(received), 
                                            kHandshakeSize - m_handshakeReceived);
            ProcessHandshake(buffer, consumed);
            
            // If handshake consumed all data, return 0
            if (consumed >= static_cast<std::size_t>(received)) {
                return 0;
            }
            
            // Move remaining data to start of buffer
            std::size_t remaining = static_cast<std::size_t>(received) - consumed;
            std::memmove(buffer, buffer + consumed, remaining);
            return remaining;
        }
        
        return static_cast<std::size_t>(received);
    } else if (received == 0) {
        HandleConnectionLoss();
        return 0;
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno == ENOTCONN || errno == ECONNRESET) {
            HandleConnectionLoss();
        } else {
            LogError("recv");
        }
        return 0;
    }
}

/**
 * @brief Attempts to flush pending TX data to the socket.
 * 
 * Called internally after enqueuing data and during receive operations.
 * Uses MSG_NOSIGNAL to prevent SIGPIPE on broken connections.
 * 
 * @param targetSock Socket file descriptor to send to.
 */
void TcpPosixAdapter::TryFlushTxBuffer(int targetSock) {
    uint8_t* buffer = m_txBuffer.get();
    if (!buffer) {
        return;
    }

    while (m_txSize > 0 && targetSock >= 0 && m_isConnected) {
        const std::size_t contiguous = std::min(m_txSize, kTxBufferCapacity - m_txHead);
        ssize_t sent = ::send(targetSock, buffer + m_txHead, contiguous, MSG_NOSIGNAL);
        if (sent > 0) {
            const std::size_t consumed = static_cast<std::size_t>(sent);
            m_txHead = (m_txHead + consumed) % kTxBufferCapacity;
            m_txSize -= consumed;
            continue;
        }

        if (sent == 0) {
            HandleConnectionLoss();
            DropPendingTx();
            return;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
            HandleConnectionLoss();
        } else {
            LogError("send");
        }
        DropPendingTx();
        return;
    }
}

/**
 * @brief Enqueues data to the circular TX buffer.
 * 
 * Implements real-time congestion control: rejects new packets when buffer
 * exceeds 50% capacity to prevent runaway buffering and mid-packet corruption.
 * 
 * @param data Pointer to data to enqueue.
 * @param length Number of bytes to enqueue.
 * @return true if data was enqueued, false if buffer congested.
 */
bool TcpPosixAdapter::EnqueueTx(const uint8_t* data, std::size_t length) {
    if (!data || length == 0) {
        return true;
    }

    // Real-time control: reject new packets when buffer > 50% to prevent runaway buffering
    // This avoids mid-packet corruption that would occur if we dropped the buffer during flush
    if (m_txSize > kTxBufferCapacity / 2) {
        LogError("tx buffer congested - rejecting new packet");
        return false;
    }

    if (length > kTxBufferCapacity - m_txSize) {
        LogError("tx buffer full - dropping packet");
        return false;
    }

    const auto* src = data;
    const std::size_t firstChunk = std::min(length, kTxBufferCapacity - m_txTail);
    std::memcpy(m_txBuffer.get() + m_txTail, src, firstChunk);

    const std::size_t remaining = length - firstChunk;
    if (remaining > 0) {
        std::memcpy(m_txBuffer.get(), src + firstChunk, remaining);
    }

    m_txTail = (m_txTail + length) % kTxBufferCapacity;
    m_txSize += length;
    return true;
}
/**
 * @brief Configures socket options for BCNP transport.
 * 
 * Enables TCP_NODELAY to minimize latency (important for real-time control)
 * and O_NONBLOCK for non-blocking I/O.
 * 
 * @param sock Socket file descriptor to configure.
 * @return true if configuration succeeded, false on error.
 */
bool TcpPosixAdapter::ConfigureSocket(int sock) {
    int yes = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&yes, sizeof(int)) < 0) {
        LogError("setsockopt(TCP_NODELAY)");
        return false;
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
        LogError("fcntl(O_NONBLOCK)");
        return false;
    }
    return true;
}

/**
 * @brief Logs an error message with throttling.
 * 
 * Prevents log spam by only logging once per kLogThrottle interval.
 * 
 * @param message Error description to log.
 */
void TcpPosixAdapter::LogError(const char* message) {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastErrorLog < kLogThrottle) {
        return;
    }
    m_lastErrorLog = now;
    std::cerr << "TCP adapter error: " << message << " errno=" << errno << std::endl;
}

/**
 * @brief Discards all pending TX data.
 * 
 * Called on connection loss to prevent stale data from being sent
 * on reconnection.
 */
void TcpPosixAdapter::DropPendingTx() {
    m_txHead = 0;
    m_txTail = 0;
    m_txSize = 0;
}

/**
 * @brief Sends the V3 protocol handshake to the peer.
 * 
 * The handshake contains the protocol magic bytes and schema hash.
 * Peers must have matching schema hashes for full interoperability.
 * 
 * @return true if handshake was queued for sending, false on error.
 */
bool TcpPosixAdapter::SendHandshake() {
    int targetSock = m_isServer ? m_clientSocket : m_socket;
    if (targetSock < 0 || !m_isConnected) {
        return false;
    }
    
    uint8_t handshake[kHandshakeSize];
    // Use custom hash if set, otherwise use default
    if (m_expectedSchemaHash != 0) {
        if (!EncodeHandshakeWithHash(handshake, sizeof(handshake), m_expectedSchemaHash)) {
            return false;
        }
    } else {
        if (!EncodeHandshake(handshake, sizeof(handshake))) {
            return false;
        }
    }
    
    // Queue handshake for sending
    if (!EnqueueTx(handshake, sizeof(handshake))) {
        return false;
    }
    
    TryFlushTxBuffer(targetSock);
    m_handshakeSent = true;
    return true;
}

/**
 * @brief Returns the expected schema hash for handshake validation.
 * 
 * @return Custom hash if SetExpectedSchemaHash() was called, otherwise kSchemaHash.
 */
uint32_t TcpPosixAdapter::GetExpectedSchemaHash() const {
    return m_expectedSchemaHash != 0 ? m_expectedSchemaHash : kSchemaHash;
}

/**
 * @brief Processes incoming handshake bytes.
 * 
 * Accumulates bytes until a complete handshake is received, then validates
 * the remote schema hash against the expected value. Sets m_schemaValidated
 * on match, logs a warning on mismatch.
 * 
 * @param data Received bytes (may be partial handshake).
 * @param length Number of bytes in data.
 * @return true if handshake is complete (valid or invalid), false if more bytes needed.
 */
bool TcpPosixAdapter::ProcessHandshake(const uint8_t* data, std::size_t length) {
    // Accumulate handshake bytes
    std::size_t toRead = std::min(length, kHandshakeSize - m_handshakeReceived);
    std::memcpy(m_handshakeBuffer + m_handshakeReceived, data, toRead);
    m_handshakeReceived += toRead;
    
    if (m_handshakeReceived < kHandshakeSize) {
        return false; // Not complete yet
    }
    
    // Extract and validate against expected hash
    m_remoteSchemaHash = ExtractSchemaHash(m_handshakeBuffer, kHandshakeSize);
    const uint32_t expected = GetExpectedSchemaHash();
    
    if (m_remoteSchemaHash != expected) {
        std::cerr << "TCP adapter: Schema mismatch! Local=0x" << std::hex << expected 
                  << " Remote=0x" << m_remoteSchemaHash << std::dec << std::endl;
        m_schemaValidated = false;
        m_handshakeComplete = true;
        return true;
    }
    
    m_schemaValidated = true;
    m_handshakeComplete = true;
    
    if (!m_handshakeSent) {
        SendHandshake();
    }
    
    return true;
}

} // namespace bcnp