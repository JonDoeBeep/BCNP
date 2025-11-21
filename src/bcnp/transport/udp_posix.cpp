#include "bcnp/transport/udp_posix.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace bcnp {

namespace {
constexpr uint32_t kPairingMagic = 0x42434E50U; // "BCNP"

uint32_t LoadU32(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) |
           (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8) |
           uint32_t(data[3]);
}

void LogErr(const char* message) {
    std::cerr << "UDP adapter error: " << message << " errno=" << errno << std::endl;
}
} // namespace

UdpPosixAdapter::UdpPosixAdapter(uint16_t listenPort, const char* targetIp, uint16_t targetPort) {
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

    m_requirePairing = (listenPort > 0);

    if (targetIp && targetPort > 0) {
        m_lastPeer = {};
        m_lastPeer.sin_family = AF_INET;
        m_lastPeer.sin_port = htons(targetPort);
        if (inet_pton(AF_INET, targetIp, &m_lastPeer.sin_addr) > 0) {
            m_hasPeer = true;
            m_peerLocked = true; // Lock to this peer for security
            m_initialPeer = m_lastPeer;
            m_pairingComplete = true;
            m_fixedPeerConfigured = true;
        } else {
            LogErr("inet_pton (invalid target IP)");
        }
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

void UdpPosixAdapter::SetPeerLockMode(bool locked) {
    m_peerLocked = locked;
    if (!locked) {
        m_pairingComplete = false;
        return;
    }

    if (m_fixedPeerConfigured && m_hasPeer) {
        m_initialPeer = m_lastPeer;
        m_pairingComplete = true;
        return;
    }

    if (m_requirePairing) {
        m_pairingComplete = false;
        m_hasPeer = false;
    }
}

void UdpPosixAdapter::SetPairingToken(uint32_t token) {
    m_pairingToken = token;
    if (m_peerLocked && m_requirePairing && !m_fixedPeerConfigured) {
        m_pairingComplete = false;
        m_hasPeer = false;
    }
}

void UdpPosixAdapter::UnlockPeer() {
    if (!m_fixedPeerConfigured) {
        m_pairingComplete = false;
        m_hasPeer = false;
    }
}

std::size_t UdpPosixAdapter::ReceiveChunk(uint8_t* buffer, std::size_t maxLength) {
    if (m_socket < 0 || !buffer || maxLength == 0) {
        return 0;
    }

    // Auto-unlock peer after timeout to allow re-pairing
    const auto now = std::chrono::steady_clock::now();
    if (m_peerLocked && m_hasPeer && !m_fixedPeerConfigured &&
        m_lastPeerRx != std::chrono::steady_clock::time_point{} &&
        now - m_lastPeerRx > kPeerTimeout) {
        UnlockPeer();
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

    if (m_peerLocked) {
        if (m_requirePairing && !m_pairingComplete && !m_fixedPeerConfigured) {
            if (ProcessPairingPacket(buffer, static_cast<std::size_t>(received), src)) {
                m_lastPeerRx = now;
                return 0; // Handshake packets are not forwarded upwards
            }
            return 0;
        }

        if (m_hasPeer && (src.sin_addr.s_addr != m_initialPeer.sin_addr.s_addr ||
                          src.sin_port != m_initialPeer.sin_port)) {
            return 0;
        }
        if (!m_hasPeer) {
            m_initialPeer = src;
            m_hasPeer = true;
        }
        m_lastPeer = src;
        m_lastPeerRx = now;
    } else {
        m_lastPeer = src;
        m_hasPeer = true;
        m_lastPeerRx = now;
    }

    return static_cast<std::size_t>(received);
}

bool UdpPosixAdapter::ProcessPairingPacket(const uint8_t* buffer, std::size_t length, const sockaddr_in& src) {
    constexpr std::size_t kPairingFrameSize = 8;
    if (length != kPairingFrameSize) {
        return false;
    }

    const uint32_t magic = LoadU32(buffer);
    const uint32_t token = LoadU32(buffer + 4);
    if (magic != kPairingMagic || token != m_pairingToken) {
        return false;
    }

    m_initialPeer = src;
    m_lastPeer = src;
    m_hasPeer = true;
    m_pairingComplete = true;
    return true;
}

} // namespace bcnp
