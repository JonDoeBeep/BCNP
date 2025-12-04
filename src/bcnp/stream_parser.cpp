#include "bcnp/stream_parser.h"

#include <algorithm>
#include <cstring>

namespace bcnp {

StreamParser::StreamParser(PacketCallback onPacket, ErrorCallback onError, std::size_t bufferSize)
    : m_onPacket(std::move(onPacket)), m_onError(std::move(onError)) {
        if (bufferSize < kHeaderSize + kChecksumSize) {
            bufferSize = kHeaderSize + kChecksumSize;
        }
        m_buffer.resize(bufferSize);
        m_decodeScratch.resize(bufferSize);
    }

void StreamParser::Push(const uint8_t* data, std::size_t length) {
    if (length == 0 || !data) {
        return;
    }

    std::size_t iterationBudget = kMaxParseIterationsPerPush;
    std::size_t remaining = length;

    while (remaining > 0) {
        if (m_size == m_buffer.size()) {
            ParseBuffer(iterationBudget);
        }

        if (m_size == m_buffer.size()) {
            const auto errorOffset = m_streamOffset;
            EmitError(PacketError::TooManyCommands, errorOffset);
            m_streamOffset += m_size;
            m_head = 0;
            m_size = 0;
            return;
        }

        if (iterationBudget == 0) {
            return;
        }

        const std::size_t writable = std::min(remaining, m_buffer.size() - m_size);
        if (writable == 0) {
            break;
        }

        const std::size_t chunkOffset = length - remaining;
        WriteToBuffer(data + chunkOffset, writable);
        remaining -= writable;

        ParseBuffer(iterationBudget);
        if (iterationBudget == 0 && remaining > 0) {
            return;
        }
    }

    if (iterationBudget > 0) {
        ParseBuffer(iterationBudget);
    }
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
    const std::size_t tailIndex = (m_head + m_size) % m_buffer.size();
    const std::size_t firstChunk = std::min(length, m_buffer.size() - tailIndex);
    std::memcpy(&m_buffer[tailIndex], data, firstChunk);
    
    const std::size_t remaining = length - firstChunk;
    if (remaining > 0) {
        std::memcpy(&m_buffer[0], data + firstChunk, remaining);
    }
    m_size += length;
}

void StreamParser::CopyOut(std::size_t offset, std::size_t length, uint8_t* dest) const {
    const std::size_t startIndex = (m_head + offset) % m_buffer.size();
    const std::size_t firstChunk = std::min(length, m_buffer.size() - startIndex);
    std::memcpy(dest, &m_buffer[startIndex], firstChunk);
    
    const std::size_t remaining = length - firstChunk;
    if (remaining > 0) {
        std::memcpy(dest + firstChunk, &m_buffer[0], remaining);
    }
}

void StreamParser::Discard(std::size_t count) {
    if (count == 0 || m_size == 0) {
        return;
    }
    if (count > m_size) {
        count = m_size;
    }
    m_head = (m_head + count) % m_buffer.size();
    m_size -= count;
    m_streamOffset += count;
}

void StreamParser::ParseBuffer(std::size_t& iterationBudget) {
    while (iterationBudget > 0 && m_size >= kHeaderSizeV3) {
        --iterationBudget;

        CopyOut(0, kHeaderSizeV3, m_decodeScratch.data());

        // V3 version check
        if (m_decodeScratch[kHeaderMajorIndex] != kProtocolMajorV3 ||
            m_decodeScratch[kHeaderMinorIndex] != kProtocolMinorV3) {
            const auto offset = m_streamOffset;
            EmitError(PacketError::UnsupportedVersion, offset);
            const std::size_t skip = FindNextHeaderCandidate();
            Discard(skip > 0 ? skip : 1);
            continue;
        }

        // Parse message type and count from V3 header (big-endian)
        const uint16_t msgTypeId = detail::LoadU16(&m_decodeScratch[kHeaderMsgTypeIndex]);
        const uint16_t messageCount = detail::LoadU16(&m_decodeScratch[kHeaderMsgCountIndex]);

        // Lookup message type to get wire size
        const std::size_t wireSize = LookupWireSize(static_cast<MessageTypeId>(msgTypeId));
        if (wireSize == 0) {
            const auto offset = m_streamOffset;
            EmitError(PacketError::UnknownMessageType, offset);
            Discard(1);
            continue;
        }

        if (messageCount > kMaxMessagesPerPacket) {
            const auto offset = m_streamOffset;
            EmitError(PacketError::TooManyCommands, offset);
            Discard(1);
            continue;
        }

        const std::size_t expected = kHeaderSizeV3 + (messageCount * wireSize) + kChecksumSize;

        const std::size_t available = std::min(expected, m_size);
        CopyOut(0, available, m_decodeScratch.data());
        auto result = DecodePacketViewWithSize(m_decodeScratch.data(), available, wireSize);

        if (result.error == PacketError::Truncated) {
            break;
        }

        if (!result.view) {
            const auto offset = m_streamOffset;
            EmitError(result.error, offset);
            // Poison packet: only discard 1 byte to resync, not the entire calculated frame
            if (result.error == PacketError::ChecksumMismatch || result.error == PacketError::InvalidFloat) {
                Discard(1);
            } else {
                const std::size_t consumed = result.bytesConsumed > 0 ? result.bytesConsumed : 1;
                Discard(consumed);
            }
            continue;
        }

        EmitPacket(*result.view);
        m_consecutiveErrors = 0;
        Discard(result.bytesConsumed);
    }
}

std::size_t StreamParser::FindNextHeaderCandidate() const {
    if (m_size <= 1) {
        return m_size == 0 ? 0 : 1;
    }

    for (std::size_t offset = 1; offset < m_size; ++offset) {
        const std::size_t firstIndex = (m_head + offset) % m_buffer.size();
        if (m_buffer[firstIndex] != kProtocolMajorV3) {
            continue;
        }

        if (offset + 1 >= m_size) {
            return offset;
        }

        const std::size_t secondIndex = (m_head + offset + 1) % m_buffer.size();
        if (m_buffer[secondIndex] == kProtocolMinorV3) {
            return offset;
        }
    }
    return 1;
}

std::size_t StreamParser::LookupWireSize(MessageTypeId typeId) const {
    // Use custom lookup if provided
    if (m_wireSizeLookup) {
        return m_wireSizeLookup(typeId);
    }
    // Fall back to global registry
    auto info = GetMessageInfo(typeId);
    return info ? info->wireSize : 0;
}

void StreamParser::EmitPacket(const PacketView& packet) {
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

// todo improe coments