#include "bcnp/transport/tcp_posix.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bcnp {

namespace {
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
    m_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        LogErr("socket");
        return;
    }

    int yes = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        LogErr("setsockopt(SO_REUSEADDR)");
    }

    ConfigureSocket(m_socket);

    if (listenPort > 0) {
        // Server mode
        m_isServer = true;
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
        // Client mode
        m_isServer = false;
        sockaddr_in targetAddr{};
        targetAddr.sin_family = AF_INET;
        targetAddr.sin_port = htons(targetPort);
        if (inet_pton(AF_INET, targetIp, &targetAddr.sin_addr) <= 0) {
            LogErr("inet_pton (invalid target IP)");
            ::close(m_socket);
            m_socket = -1;
            return;
        }

        if (connect(m_socket, reinterpret_cast<sockaddr*>(&targetAddr), sizeof(targetAddr)) < 0) {
            if (errno != EINPROGRESS) {
                LogErr("connect");
                ::close(m_socket);
                m_socket = -1;
                return;
            }
            // EINPROGRESS is expected for non-blocking connect
        }
        // For client, we consider it "connected" once the socket is created and connect() called.
        // Actual connection state might need to be checked via select/poll or first send/recv.
        // But for simplicity here, we'll assume it's attempting.
        m_isConnected = true; 
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

bool TcpPosixAdapter::SendBytes(const uint8_t* data, std::size_t length) {
    if (!data || length == 0) {
        return true;
    }

    int targetSock = m_isServer ? m_clientSocket : m_socket;
    if (targetSock < 0) {
        return false;
    }

    // Check connection status for client if needed, but send() will fail if not connected.
    
    ssize_t sent = ::send(targetSock, data, length, MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false; // Buffer full
        }
        if (errno == EPIPE || errno == ECONNRESET) {
            m_isConnected = false;
            if (m_isServer) {
                ::close(m_clientSocket);
                m_clientSocket = -1;
            }
        }
        return false;
    }
    return static_cast<std::size_t>(sent) == length;
}

std::size_t TcpPosixAdapter::ReceiveChunk(uint8_t* buffer, std::size_t maxLength) {
    if (m_socket < 0 || !buffer || maxLength == 0) {
        return 0;
    }

    if (m_isServer) {
        if (m_clientSocket < 0) {
            // Try to accept
            sockaddr_in clientAddr{};
            socklen_t len = sizeof(clientAddr);
            int clientSock = ::accept(m_socket, reinterpret_cast<sockaddr*>(&clientAddr), &len);
            if (clientSock >= 0) {
                m_clientSocket = clientSock;
                m_isConnected = true;
                ConfigureSocket(m_clientSocket);
                // Fall through to read
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LogErr("accept");
                }
                return 0;
            }
        }
    }

    int targetSock = m_isServer ? m_clientSocket : m_socket;
    if (targetSock < 0) return 0;

    ssize_t received = ::recv(targetSock, buffer, maxLength, 0);
    if (received > 0) {
        return static_cast<std::size_t>(received);
    } else if (received == 0) {
        // Peer closed
        m_isConnected = false;
        if (m_isServer) {
            ::close(m_clientSocket);
            m_clientSocket = -1;
        }
        return 0;
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        // Error or not connected yet (for client)
        if (!m_isServer && !m_isConnected) {
             // Check if connected?
             // For now, just assume error means not connected or broken pipe
        }
        return 0;
    }
}

} // namespace bcnp
