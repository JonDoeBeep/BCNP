#include "bcnp/stream_parser.h"

#include <algorithm>

namespace bcnp {

StreamParser::StreamParser(PacketCallback onPacket, ErrorCallback onError)
    : m_onPacket(std::move(onPacket)), m_onError(std::move(onError)) {
    m_buffer.reserve(kMaxPacketSize * 2);
}

void StreamParser::Push(const uint8_t* data, std::size_t length) {
    if (length == 0 || !data) {
        return;
    }

    const auto insertionBegin = m_buffer.size();
    m_buffer.resize(m_buffer.size() + length);
    std::copy(data, data + length, m_buffer.begin() + insertionBegin);

    while (m_buffer.size() >= kHeaderSize) {
        const uint8_t commandCount = m_buffer[kHeaderCountIndex];
        if (commandCount > kMaxCommandsPerPacket) {
            EmitError(PacketError::TooManyCommands);
            m_buffer.clear();
            return;
        }

        const std::size_t expected = kHeaderSize + (commandCount * kCommandSize);
        if (expected > kMaxPacketSize) {
            EmitError(PacketError::TooManyCommands);
            m_buffer.clear();
            return;
        }

        if (m_buffer.size() < expected) {
            break;
        }

        auto result = DecodePacket(m_buffer.data(), expected);
        if (!result.packet) {
            EmitError(result.error);
        } else {
            EmitPacket(*result.packet);
        }

        const std::size_t consumed = result.bytesConsumed ? result.bytesConsumed : expected;
        m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
}

void StreamParser::Reset() {
    m_buffer.clear();
}

void StreamParser::EmitPacket(const Packet& packet) {
    if (m_onPacket) {
        m_onPacket(packet);
    }
}

void StreamParser::EmitError(PacketError error) {
    if (m_onError) {
        m_onError(error);
    }
}

} // namespace bcnp
