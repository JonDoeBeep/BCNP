#include "bcnp/transport/udp_posix.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace bcnp {

namespace {
void LogErr(const char* message) {
    std::cerr << "UDP adapter error: " << message << " errno=" << errno << std::endl;
}
}

UdpPosixAdapter::UdpPosixAdapter(uint16_t listenPort) {
    m_socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_socket < 0) {
        LogErr("socket");
        return;
    }

    int yes = 1;
    if (setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        LogErr("setsockopt(SO_REUSEADDR)");
    }

    if (fcntl(m_socket, F_SETFL, O_NONBLOCK) < 0) {
        LogErr("fcntl(O_NONBLOCK)");
        ::close(m_socket);
        m_socket = -1;
        return;
    }

    m_bind = {};
    m_bind.sin_family = AF_INET;
    m_bind.sin_port = htons(listenPort);
    m_bind.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_socket, reinterpret_cast<sockaddr*>(&m_bind), sizeof(m_bind)) < 0) {
        LogErr("bind");
        ::close(m_socket);
        m_socket = -1;
        return;
    }
}

UdpPosixAdapter::~UdpPosixAdapter() {
    if (m_socket >= 0) {
        ::close(m_socket);
    }
}

bool UdpPosixAdapter::SendBytes(const uint8_t* data, std::size_t length) {
    if (!data || length == 0) {
        return true;
    }
    if (!m_hasPeer || m_socket < 0) {
        return false;
    }
    const auto sent = ::sendto(m_socket, data, length, 0,
                               reinterpret_cast<sockaddr*>(&m_lastPeer), sizeof(m_lastPeer));
    return sent == static_cast<ssize_t>(length);
}

std::size_t UdpPosixAdapter::ReceiveChunk(uint8_t* buffer, std::size_t maxLength) {
    if (m_socket < 0 || !buffer || maxLength == 0) {
        return 0;
    }

    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    const auto received = ::recvfrom(m_socket, buffer, maxLength, MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr*>(&src), &slen);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        LogErr("recvfrom");
        return 0;
    }

    m_lastPeer = src;
    m_hasPeer = true;
    return static_cast<std::size_t>(received);
}

} // namespace bcnp
