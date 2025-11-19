#pragma once

#include "bcnp/transport/adapter.h"

#include <netinet/in.h>

namespace bcnp {

class TcpPosixAdapter : public DuplexAdapter {
public:
    // Server mode: provide listenPort. targetIp/targetPort are ignored.
    // Client mode: provide targetIp/targetPort. listenPort is ignored (0).
    explicit TcpPosixAdapter(uint16_t listenPort, const char* targetIp = nullptr, uint16_t targetPort = 0);
    ~TcpPosixAdapter() override;

    bool SendBytes(const uint8_t* data, std::size_t length) override;
    std::size_t ReceiveChunk(uint8_t* buffer, std::size_t maxLength) override;

    bool IsValid() const { return m_socket >= 0; }
    bool IsConnected() const { return m_isConnected; }

private:
    int m_socket{-1};
    int m_clientSocket{-1}; // For server mode, the connected client
    bool m_isServer{false};
    bool m_isConnected{false};
    sockaddr_in m_peerAddr{};
};

} // namespace bcnp
