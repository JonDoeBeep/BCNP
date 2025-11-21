#pragma once

#include "bcnp/transport/adapter.h"

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

private:
    int m_socket{-1};
    sockaddr_in m_bind{};
    sockaddr_in m_lastPeer{};
    bool m_hasPeer{false};
    bool m_peerLocked{false};
    sockaddr_in m_initialPeer{}; 
};

} // namespace bcnp
