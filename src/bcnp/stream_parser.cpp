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

    // DoS protection: Prevent unbounded buffer growth from garbage floods
    if (m_buffer.size() + length > kMaxBufferSize) {
        // Try to compact first
        if (m_head > 0) {
            const std::size_t remaining = m_buffer.size() - m_head;
            std::memmove(m_buffer.data(), m_buffer.data() + m_head, remaining);
            m_buffer.resize(remaining);
            m_streamOffset += m_head;
            m_head = 0;
        }

        if (m_buffer.size() + length > kMaxBufferSize) {
            const auto errorOffset = m_streamOffset;
            m_streamOffset += m_buffer.size();
            m_buffer.clear();
            m_head = 0;
            EmitError(PacketError::TooManyCommands, errorOffset);
        }
    }

    const auto insertionBegin = m_buffer.size();
    m_buffer.resize(m_buffer.size() + length);
    std::memcpy(m_buffer.data() + insertionBegin, data, length);

    while ((m_buffer.size() - m_head) >= kHeaderSize) {
        const uint8_t* headPtr = m_buffer.data() + m_head;
        const uint8_t commandCount = headPtr[kHeaderCountIndex];
        if (commandCount > kMaxCommandsPerPacket) {
            const auto offset = m_streamOffset + m_head;
            EmitError(PacketError::TooManyCommands, offset);
            ++m_head;
            continue;
        }

        const std::size_t expected = kHeaderSize + (commandCount * kCommandSize);
        if (expected > kMaxPacketSize) {
            const auto offset = m_streamOffset + m_head;
            EmitError(PacketError::TooManyCommands, offset);
            ++m_head;
            continue;
        }

        const std::size_t remaining = m_buffer.size() - m_head;
        auto result = DecodePacket(headPtr, remaining);
        if (result.error == PacketError::Truncated) {
            break;
        }
        if (!result.packet) {
            const auto offset = m_streamOffset + m_head;
            EmitError(result.error, offset);
        } else {
            EmitPacket(*result.packet);
            m_consecutiveErrors = 0;
        }

        std::size_t consumed = result.bytesConsumed;
        if (consumed == 0) {
            consumed = 1;
        }
        m_head += consumed;
    }

    if (m_head == m_buffer.size()) {
        m_streamOffset += m_head;
        m_buffer.clear();
        m_head = 0;
    } else if (m_head > 0 && (m_head > m_buffer.size() / 2 || m_head > kMaxPacketSize)) {
        m_streamOffset += m_head;
        const std::size_t remaining = m_buffer.size() - m_head;
        std::memmove(m_buffer.data(), m_buffer.data() + m_head, remaining);
        m_buffer.resize(remaining);
        m_head = 0;
    }
}

void StreamParser::Reset(bool resetErrorState) {
    m_buffer.clear();
    m_head = 0;
    if (resetErrorState) {
        m_consecutiveErrors = 0;
        m_streamOffset = 0;
    }
}

void StreamParser::EmitPacket(const Packet& packet) {
    if (m_onPacket) {
        m_onPacket(packet);
    }
}

void StreamParser::EmitError(PacketError error, std::size_t offset) {
    if (m_onError) {
        ErrorInfo info{error, offset, ++m_consecutiveErrors};
        m_onError(info);
    }
}

} // namespace bcnp
