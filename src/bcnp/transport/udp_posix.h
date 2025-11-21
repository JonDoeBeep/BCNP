#pragma once

#include "bcnp/transport/adapter.h"

#include <chrono>
#include <cstdint>
#include <netinet/in.h>

namespace bcnp {

class UdpPosixAdapter : public DuplexAdapter {
public:
    explicit UdpPosixAdapter(uint16_t listenPort, const char* targetIp = nullptr, uint16_t targetPort = 0);
    ~UdpPosixAdapter() override;

    bool SendBytes(const uint8_t* data, std::size_t length) override;
    std::size_t ReceiveChunk(uint8_t* buffer, std::size_t maxLength) override;

    bool IsValid() const { return m_socket >= 0; }
    
    // Peer security: when true, locks to initial peer and ignores other sources
    void SetPeerLockMode(bool locked);
    void SetPairingToken(uint32_t token);
    void UnlockPeer();

private:
    bool ProcessPairingPacket(const uint8_t* buffer, std::size_t length, const sockaddr_in& src);

    int m_socket{-1};
    sockaddr_in m_bind{};
    sockaddr_in m_lastPeer{};
    bool m_hasPeer{false};
    bool m_peerLocked{false};
    bool m_pairingComplete{false};
    bool m_requirePairing{false};
    bool m_fixedPeerConfigured{false};
    uint32_t m_pairingToken{0x42434E50U};
    sockaddr_in m_initialPeer{};
    std::chrono::steady_clock::time_point m_lastPeerRx{};
    static constexpr std::chrono::milliseconds kPeerTimeout{5000};
};

} // namespace bcnp
