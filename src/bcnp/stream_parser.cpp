#include "bcnp/stream_parser.h"

#include <algorithm>
#include <cstring>

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
    std::memcpy(m_buffer.data() + insertionBegin, data, length);

    while ((m_buffer.size() - m_head) >= kHeaderSize) {
        const uint8_t* headPtr = m_buffer.data() + m_head;
        const uint8_t commandCount = headPtr[kHeaderCountIndex];
        if (commandCount > kMaxCommandsPerPacket) {
            EmitError(PacketError::TooManyCommands);
            Reset();
            return;
        }

        const std::size_t expected = kHeaderSize + (commandCount * kCommandSize);
        if (expected > kMaxPacketSize) {
            EmitError(PacketError::TooManyCommands);
            Reset();
            return;
        }

        if ((m_buffer.size() - m_head) < expected) {
            break;
        }

        auto result = DecodePacket(headPtr, expected);
        if (!result.packet) {
            EmitError(result.error);
        } else {
            EmitPacket(*result.packet);
        }

        const std::size_t consumed = result.bytesConsumed ? result.bytesConsumed : expected;
        m_head += consumed;
    }

    if (m_head == m_buffer.size()) {
        m_buffer.clear();
        m_head = 0;
    } else if (m_head > 0 && (m_head > m_buffer.size() / 2 || m_head > kMaxPacketSize)) {
        const std::size_t remaining = m_buffer.size() - m_head;
        std::memmove(m_buffer.data(), m_buffer.data() + m_head, remaining);
        m_buffer.resize(remaining);
        m_head = 0;
    }
}

void StreamParser::Reset() {
    m_buffer.clear();
    m_head = 0;
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
