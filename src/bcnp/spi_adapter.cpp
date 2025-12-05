/**
 * @file spi_adapter.cpp
 * @brief Implementation of the deprecated SPI transport adapter.
 */

#include "bcnp/spi_adapter.h"

namespace bcnp {

SpiStreamAdapter::SpiStreamAdapter(ReceiveChunkFn receive, SendBytesFn send, StreamParser& parser)
    : m_receive(std::move(receive)), m_send(std::move(send)), m_parser(parser) {
}

void SpiStreamAdapter::Poll() {
    if (!m_receive) {
        return;
    }

    // Read chunks until no more data available
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

// SendPacket<T> is implemented as a template in the header

} // namespace bcnp
