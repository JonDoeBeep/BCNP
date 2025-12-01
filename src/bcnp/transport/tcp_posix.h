#pragma once

#include "bcnp/packet.h"
#include "bcnp/transport/adapter.h"
#include <bcnp/message_types.h>

#include <chrono>
#include <memory>
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
    bool IsConnected() const { return m_isConnected && m_handshakeComplete; }
    
    // V3 Schema handshake
    bool IsHandshakeComplete() const { return m_handshakeComplete && m_schemaValidated; }
    bool SendHandshake();
    uint32_t GetRemoteSchemaHash() const { return m_remoteSchemaHash; }
    
    /// Override expected schema hash (for testing with custom schemas)
    void SetExpectedSchemaHash(uint32_t hash) { m_expectedSchemaHash = hash; }

private:
    bool CreateBaseSocket();
    bool ConfigureSocket(int sock);
    void BeginClientConnect(bool forceImmediate);
    void PollConnection();
    void HandleConnectionLoss();
    void TryFlushTxBuffer(int targetSock);
    bool EnqueueTx(const uint8_t* data, std::size_t length);
    void DropPendingTx();
    void LogError(const char* message);
    bool ProcessHandshake(const uint8_t* data, std::size_t length);
    uint32_t GetExpectedSchemaHash() const;

    int m_socket{-1};
    int m_clientSocket{-1}; // For server mode, the connected client
    bool m_isServer{false};
    bool m_isConnected{false};
    bool m_connectInProgress{false};
    bool m_handshakeComplete{false};
    bool m_handshakeSent{false};
    bool m_schemaValidated{false};
    uint32_t m_remoteSchemaHash{0};
    uint32_t m_expectedSchemaHash{0};  // 0 = use kSchemaHash from generated header
    sockaddr_in m_peerAddr{};
    bool m_peerAddrValid{false};
    std::chrono::steady_clock::time_point m_nextReconnectAttempt{};
    std::chrono::steady_clock::time_point m_lastServerRx{};
    std::chrono::milliseconds m_serverClientTimeout{5000}; // 5 second zombie timeout
    // Max packet size: header + largest reasonable message payload + CRC
    // 64KB should be sufficient for most use cases
    static constexpr std::size_t kMaxPacketSize = 65536;
    static constexpr std::size_t kTxBufferCapacity = kMaxPacketSize * 8; // Real-time: limit buffering
    std::unique_ptr<uint8_t[]> m_txBuffer;
    std::size_t m_txHead{0};
    std::size_t m_txTail{0};
    std::size_t m_txSize{0};
    std::chrono::steady_clock::time_point m_lastErrorLog{};
    
    // Handshake receive buffer
    uint8_t m_handshakeBuffer[kHandshakeSize]{};
    std::size_t m_handshakeReceived{0};
};

} // namespace bcnp
