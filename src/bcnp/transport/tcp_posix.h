#pragma once

#include "bcnp/packet.h"
#include "bcnp/transport/adapter.h"

#include <array>
#include <chrono>
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

    bool IsValid() const { return m_socket >= 0 || (!m_isServer && m_peerAddrValid); }
    bool IsConnected() const { return m_isConnected; }

private:
    bool CreateBaseSocket();
    void BeginClientConnect(bool forceImmediate);
    void PollConnection();
    void HandleConnectionLoss();
    void TryFlushTxBuffer(int targetSock);
    bool EnqueueTx(const uint8_t* data, std::size_t length);
    void DropPendingTx();

    int m_socket{-1};
    int m_clientSocket{-1}; // For server mode, the connected client
    bool m_isServer{false};
    bool m_isConnected{false};
    bool m_connectInProgress{false};
    sockaddr_in m_peerAddr{};
    bool m_peerAddrValid{false};
    std::chrono::steady_clock::time_point m_nextReconnectAttempt{};
    std::chrono::steady_clock::time_point m_lastServerRx{};
    std::chrono::milliseconds m_serverClientTimeout{5000}; // 5 second zombie timeout
    static constexpr std::size_t kTxBufferCapacity = kMaxPacketSize * 128;
    std::array<uint8_t, kTxBufferCapacity> m_txBuffer{};
    std::size_t m_txHead{0};
    std::size_t m_txTail{0};
    std::size_t m_txSize{0};
};

} // namespace bcnp
