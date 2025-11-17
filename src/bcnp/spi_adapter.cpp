#include "bcnp/spi_adapter.h"

namespace bcnp {

SpiStreamAdapter::SpiStreamAdapter(ReceiveChunkFn receive, SendBytesFn send, StreamParser& parser)
    : m_receive(std::move(receive)), m_send(std::move(send)), m_parser(parser) {
    m_encodeScratch.reserve(kMaxPacketSize);
}

void SpiStreamAdapter::Poll() {
    if (!m_receive) {
        return;
    }

    uint8_t buffer[256];
    std::size_t received = m_receive(buffer, sizeof(buffer));
    while (received > 0) {
        PushChunk(buffer, received);
        received = m_receive(buffer, sizeof(buffer));
    }
}

void SpiStreamAdapter::PushChunk(const uint8_t* data, std::size_t length) {
    m_parser.Push(data, length);
}

bool SpiStreamAdapter::SendPacket(const Packet& packet) {
    if (!m_send) {
        return false;
    }
    if (!EncodePacket(packet, m_encodeScratch)) {
        return false;
    }
    return m_send(m_encodeScratch.data(), m_encodeScratch.size());
}

} // namespace bcnp
