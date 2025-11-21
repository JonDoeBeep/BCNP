#include "bcnp/stream_parser.h"

#include <algorithm>
#include <cstring>

namespace bcnp {

StreamParser::StreamParser(PacketCallback onPacket, ErrorCallback onError)
    : m_onPacket(std::move(onPacket)), m_onError(std::move(onError)) {}

void StreamParser::Push(const uint8_t* data, std::size_t length) {
    if (length == 0 || !data) {
        return;
    }

    std::size_t remaining = length;
    while (remaining > 0) {
        if (m_size == kMaxBufferSize) {
            ParseBuffer();
        }

        if (m_size == kMaxBufferSize) {
            const auto errorOffset = m_streamOffset;
            EmitError(PacketError::TooManyCommands, errorOffset);
            m_streamOffset += m_size;
            m_head = 0;
            m_size = 0;
            return;
        }

        const std::size_t writable = std::min(remaining, kMaxBufferSize - m_size);
        const std::size_t chunkOffset = length - remaining;
        WriteToBuffer(data + chunkOffset, writable);
        remaining -= writable;
    }

    ParseBuffer();
}

void StreamParser::Reset(bool resetErrorState) {
    m_head = 0;
    m_size = 0;
    if (resetErrorState) {
        m_consecutiveErrors = 0;
        m_streamOffset = 0;
    }
}

void StreamParser::WriteToBuffer(const uint8_t* data, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) {
        const std::size_t slot = (m_head + m_size + i) % kMaxBufferSize;
        m_buffer[slot] = data[i];
    }
    m_size += length;
}

void StreamParser::CopyOut(std::size_t offset, std::size_t length, uint8_t* dest) const {
    std::size_t index = (m_head + offset) % kMaxBufferSize;
    for (std::size_t i = 0; i < length; ++i) {
        dest[i] = m_buffer[index];
        index = (index + 1) % kMaxBufferSize;
    }
}

void StreamParser::Discard(std::size_t count) {
    if (count == 0 || m_size == 0) {
        return;
    }
    if (count > m_size) {
        count = m_size;
    }
    m_head = (m_head + count) % kMaxBufferSize;
    m_size -= count;
    m_streamOffset += count;
}

void StreamParser::ParseBuffer() {
    while (m_size >= kHeaderSize) {
        CopyOut(0, kHeaderSize, m_decodeScratch.data());
        const uint8_t commandCount = m_decodeScratch[kHeaderCountIndex];

        if (commandCount > kMaxCommandsPerPacket) {
            const auto offset = m_streamOffset;
            EmitError(PacketError::TooManyCommands, offset);
            Discard(1);
            continue;
        }

        const std::size_t expected = kHeaderSize + (commandCount * kCommandSize) + kChecksumSize;
        if (expected > kMaxPacketSize) {
            const auto offset = m_streamOffset;
            EmitError(PacketError::TooManyCommands, offset);
            Discard(1);
            continue;
        }

        const std::size_t available = std::min(expected, m_size);
        CopyOut(0, available, m_decodeScratch.data());
        auto result = DecodePacket(m_decodeScratch.data(), available);

        if (result.error == PacketError::Truncated) {
            break;
        }

        if (!result.packet) {
            const auto offset = m_streamOffset;
            EmitError(result.error, offset);
            const std::size_t consumed = result.bytesConsumed > 0 ? result.bytesConsumed : 1;
            Discard(consumed);
            continue;
        }

        EmitPacket(*result.packet);
        m_consecutiveErrors = 0;
        Discard(result.bytesConsumed);
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
