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

private:
    int m_socket{-1};
    sockaddr_in m_bind{};
    sockaddr_in m_lastPeer{};
    bool m_hasPeer{false};
};

} // namespace bcnp
