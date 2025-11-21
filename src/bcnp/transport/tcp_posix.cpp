#include "bcnp/transport/tcp_posix.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bcnp {

namespace {
constexpr int kSendWaitTimeoutMs = 20;
constexpr int kSendWaitMaxAttempts = 200;
constexpr auto kReconnectInterval = std::chrono::milliseconds(500);

void LogErr(const char* message) {
    std::cerr << "TCP adapter error: " << message << " errno=" << errno << std::endl;
}

void ConfigureSocket(int sock) {
    int yes = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&yes, sizeof(int)) < 0) {
        LogErr("setsockopt(TCP_NODELAY)");
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0) {
        LogErr("fcntl(O_NONBLOCK)");
    }
}
} // namespace

TcpPosixAdapter::TcpPosixAdapter(uint16_t listenPort, const char* targetIp, uint16_t targetPort) {
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
            LogErr("bind");
            ::close(m_socket);
            m_socket = -1;
            return;
        }

        if (listen(m_socket, 1) < 0) {
            LogErr("listen");
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
            LogErr("inet_pton (invalid target IP)");
            return;
        }

        m_peerAddr = targetAddr;
        m_peerAddrValid = true;
        BeginClientConnect(true);
    }
}

TcpPosixAdapter::~TcpPosixAdapter() {
    if (m_clientSocket >= 0) {
        ::close(m_clientSocket);
    }
    if (m_socket >= 0) {
        ::close(m_socket);
    }
}

bool TcpPosixAdapter::CreateBaseSocket() {
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }

    m_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        LogErr("socket");
        return false;
    }

    int yes = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        LogErr("setsockopt(SO_REUSEADDR)");
    }

    ConfigureSocket(m_socket);
    m_isConnected = false;
    m_connectInProgress = false;
    return true;
}

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
        LogErr("connect");
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

TcpPosixAdapter::PollStatus TcpPosixAdapter::WaitForWritable(int sock, int timeoutMs) const {
    pollfd descriptor{};
    descriptor.fd = sock;
    descriptor.events = POLLOUT;

    while (true) {
        int result = ::poll(&descriptor, 1, timeoutMs);
        if (result > 0) {
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return PollStatus::Error;
            }
            if (descriptor.revents & POLLOUT) {
                return PollStatus::Ready;
            }
            return PollStatus::Error;
        }
        if (result == 0) {
            return PollStatus::Timeout;
        }
        if (errno == EINTR) {
            continue;
        }
        LogErr("poll");
        return PollStatus::Error;
    }
}

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
                // Client hasn't sent data in timeout period - close to allow new connection
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
            ConfigureSocket(clientSock);
            m_clientSocket = clientSock;
            m_isConnected = true;
            m_lastServerRx = std::chrono::steady_clock::now();
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LogErr("accept");
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
            LogErr("getsockopt(SO_ERROR)");
            return;
        }

        if (err == 0) {
            m_isConnected = true;
            m_connectInProgress = false;
            return;
        }

        if (err == EINPROGRESS || err == EALREADY) {
            return;
        }

        errno = err;
        LogErr("connect (async)");
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

void TcpPosixAdapter::HandleConnectionLoss() {
    m_isConnected = false;

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
    // Force immediate reconnection attempt after connection loss
    BeginClientConnect(true);
}

bool TcpPosixAdapter::SendBytes(const uint8_t* data, std::size_t length) {
    if (!data || length == 0) {
        return true;
    }

    PollConnection();

    int targetSock = m_isServer ? m_clientSocket : m_socket;
    if (targetSock < 0 || !m_isConnected) {
        return false;
    }

    std::size_t totalSent = 0;
    // Retry loop: Handles partial sends by waiting for socket writability (EAGAIN/EWOULDBLOCK). Must send complete message to avoid stream corruption.
    while (totalSent < length) {
        ssize_t sent = ::send(targetSock, data + totalSent, length - totalSent, MSG_NOSIGNAL);
        if (sent > 0) {
            totalSent += static_cast<std::size_t>(sent);
            continue;
        }

        if (sent == 0) {
            // Connection closed
            HandleConnectionLoss();
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            auto pollResult = WaitForWritable(targetSock, kSendWaitTimeoutMs);
            if (pollResult == PollStatus::Ready) {
                continue;
            } else if (pollResult == PollStatus::Timeout) {
                // Timeout waiting for socket - this is a send failure
                LogErr("send timeout waiting for writability");
                return false;
            } else {
                HandleConnectionLoss();
                return false;
            }
        }

        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
            HandleConnectionLoss();
        } else {
            LogErr("send");
        }
        return false;
    }

    return totalSent == length;
}

std::size_t TcpPosixAdapter::ReceiveChunk(uint8_t* buffer, std::size_t maxLength) {
    if (m_socket < 0 || !buffer || maxLength == 0) {
        return 0;
    }

    PollConnection();

    int targetSock = m_isServer ? m_clientSocket : m_socket;
    if (targetSock < 0 || !m_isConnected) {
        return 0;
    }

    ssize_t received;
    do {
        received = ::recv(targetSock, buffer, maxLength, 0);
    } while (received < 0 && errno == EINTR);

    if (received > 0) {
        if (m_isServer) {
            m_lastServerRx = std::chrono::steady_clock::now();
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
            LogErr("recv");
        }
        return 0;
    }
}

} // namespace bcnp
