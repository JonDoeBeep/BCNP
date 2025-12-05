/**
 * @file udp_posix.cpp
 * @brief UDP transport adapter implementation for POSIX systems.
 * 
 * Provides connectionless BCNP transport over UDP sockets with optional
 * peer locking for security. Supports V3 protocol handshake with schema
 * hash validation for peer pairing.
 * 
 * @note UDP does not guarantee delivery or ordering. Use TCP for reliable
 *       transport. UDP is suitable for low-latency telemetry where occasional
 *       packet loss is acceptable.
 * 
 * 
 * @see UdpPosixAdapter
 */
#include "bcnp/transport/udp_posix.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <unistd.h>

namespace bcnp {

namespace {
/// @brief Magic bytes for pairing packet validation ("BCNP").
constexpr uint32_t kPairingMagic = 0x42434E50U; // "BCNP"

/**
 * @brief Loads a big-endian 32-bit unsigned integer from a byte buffer.
 * @param data Pointer to 4-byte buffer.
 * @return Decoded uint32_t value.
 */
uint32_t LoadU32(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) |
           (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8) |
           uint32_t(data[3]);
}

/**
 * @brief Logs a UDP adapter error to stderr.
 * @param message Error description.
 */
void LogErr(const char* message) {
    std::cerr << "UDP adapter error: " << message << " errno=" << errno << std::endl;
}
} // namespace

/**
 * @brief Constructs a UDP adapter bound to the specified port.
 * 
 * @param listenPort Port to bind for receiving UDP datagrams.
 * @param targetIp Optional fixed target IP (bypasses pairing).
 * @param targetPort Optional fixed target port (requires targetIp).
 * 
 * When a fixed target is configured, the adapter is automatically locked
 * to that peer and skips the pairing handshake. Otherwise, pairing is
 * required before sending data.
 */
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

/**
 * @brief Destructor. Closes the socket.
 */
UdpPosixAdapter::~UdpPosixAdapter() {
    if (m_socket >= 0) {
        ::close(m_socket);
    }
}

/**
 * @brief Sends bytes to the current peer via UDP.
 * 
 * @param data Pointer to byte buffer to send.
 * @param length Number of bytes to send.
 * @return true if all bytes were sent, false if no peer or socket error.
 * 
 * @note UDP does not guarantee delivery. Consider using TCP for reliable transport.
 */
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

/**
 * @brief Enables or disables peer locking.
 * 
 * When locked, the adapter only accepts packets from the initial peer.
 * This provides security against spoofing in multi-device environments.
 * 
 * @param locked true to lock to current/next peer, false to accept any source.
 */
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

/**
 * @brief Sets the expected pairing token for handshake validation.
 * 
 * Both peers must use the same token for pairing to succeed.
 * Changing the token resets pairing state (unless fixed peer is configured).
 * 
 * @param token 32-bit pairing token (default is "BCNP" = 0x42434E50).
 */
void UdpPosixAdapter::SetPairingToken(uint32_t token) {
    m_pairingToken = token;
    if (m_peerLocked && m_requirePairing && !m_fixedPeerConfigured) {
        m_pairingComplete = false;
        m_hasPeer = false;
    }
}

/**
 * @brief Resets pairing state to allow re-pairing with a new peer.
 * 
 * No-op if a fixed peer is configured (must recreate adapter to change).
 */
void UdpPosixAdapter::UnlockPeer() {
    if (!m_fixedPeerConfigured) {
        m_pairingComplete = false;
        m_hasPeer = false;
    }
}

/**
 * @brief Receives a UDP datagram.
 * 
 * Performs non-blocking receive. Handles peer locking and pairing:
 * In locked mode, packets from non-paired sources are ignored
 * Pairing packets are processed internally and not returned
 * Automatic peer timeout triggers re-pairing after kPeerTimeout
 * 
 * @param buffer Destination buffer for received data.
 * @param maxLength Maximum bytes to receive.
 * @return Number of bytes received (0 if no data, filtered, or error).
 */
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

/**
 * @brief Processes a V3 pairing/handshake packet.
 * 
 * Validates the magic bytes and schema hash. On successful validation,
 * locks to the source peer and sends a handshake response.
 * 
 * @param buffer Received packet data.
 * @param length Packet length (must be kHandshakeSize).
 * @param src Source address of the packet.
 * @return true if packet was a valid handshake, false otherwise.
 */
bool UdpPosixAdapter::ProcessPairingPacket(const uint8_t* buffer, std::size_t length, const sockaddr_in& src) {
    // V3 handshake: "BCNP" (4 bytes) + schema hash (4 bytes)
    if (length != kHandshakeSize) {
        return false;
    }

    // Check magic bytes
    if (std::memcmp(buffer, kHandshakeMagic.data(), 4) != 0) {
        return false;
    }

    // Extract and validate schema hash
    m_remoteSchemaHash = LoadU32(buffer + 4);
    if (m_remoteSchemaHash != kSchemaHash) {
        std::cerr << "UDP adapter: Schema mismatch! Local=0x" << std::hex << kSchemaHash 
                  << " Remote=0x" << m_remoteSchemaHash << std::dec << std::endl;
        m_schemaValidated = false;
        // V3: Reject pairing if schema doesn't match
        return false;
    }

    m_initialPeer = src;
    m_lastPeer = src;
    m_hasPeer = true;
    m_pairingComplete = true;
    m_schemaValidated = true;
    
    // Send our handshake response
    SendHandshake();
    
    return true;
}

/**
 * @brief Sends the V3 protocol handshake to the current peer.
 * 
 * The handshake contains the protocol magic bytes and schema hash.
 * Sent automatically as a response to receiving a valid pairing packet.
 * 
 * @return true if handshake was sent, false if no peer or socket error.
 */
bool UdpPosixAdapter::SendHandshake() {
    if (!m_hasPeer || m_socket < 0) {
        return false;
    }
    
    uint8_t handshake[kHandshakeSize];
    if (!EncodeHandshake(handshake, sizeof(handshake))) {
        return false;
    }
    
    const auto sent = ::sendto(m_socket, handshake, sizeof(handshake), 0,
                               reinterpret_cast<sockaddr*>(&m_lastPeer), sizeof(m_lastPeer));
    return sent == static_cast<ssize_t>(sizeof(handshake));
}

} // namespace bcnp