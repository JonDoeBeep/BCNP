/**
 * @file stream_parser.cpp
 * @brief Implementation of the BCNP stream parser.
 * 
 * Handles byte stream reassembly, packet framing, CRC validation, and
 * error recovery for incoming BCNP data. Uses a ring buffer for efficient
 * handling of partial packets and network fragmentation.
 */

#include "bcnp/stream_parser.h"

#include <algorithm>
#include <cstring>

namespace bcnp {

/**
 * @brief Construct a stream parser with callbacks and buffer size.
 * 
 * @param onPacket Callback invoked for each valid packet
 * @param onError Callback invoked on parse errors (optional)
 * @param bufferSize Internal ring buffer size (minimum: header + checksum)
 */
StreamParser::StreamParser(PacketCallback onPacket, ErrorCallback onError, std::size_t bufferSize)
    : m_onPacket(std::move(onPacket)), m_onError(std::move(onError)) {
        if (bufferSize < kHeaderSize + kChecksumSize) {
            bufferSize = kHeaderSize + kChecksumSize;
        }
        m_buffer.resize(bufferSize);
        m_decodeScratch.resize(bufferSize);
    }

/**
 * @brief Push raw bytes into the parser for processing.
 * 
 * Bytes are added to the internal ring buffer and parsed for complete packets.
 * Valid packets trigger the onPacket callback; parse errors trigger onError.
 * The parser handles partial packets across multiple Push() calls.
 * 
 * @param data Pointer to incoming byte data
 * @param length Number of bytes to process
 * 
 * @note Limits iterations per call to prevent infinite loops on malformed data.
 *       Returns early if the iteration budget is exhausted.
 */
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

/**
 * @brief Reset the parser to its initial state.
 * 
 * Clears the internal buffer and optionally resets error tracking.
 * Call this when starting a new connection or after unrecoverable errors.
 * 
 * @param resetErrorState If true, also resets consecutive error count and stream offset
 */
void StreamParser::Reset(bool resetErrorState) {
    m_head = 0;
    m_size = 0;
    if (resetErrorState) {
        m_consecutiveErrors = 0;
        m_streamOffset = 0;
    }
}

/**
 * @brief Write data to the ring buffer tail.
 * 
 * Handles wrap-around when the buffer end is reached.
 * Caller must ensure sufficient space exists (m_buffer.size() - m_size >= length).
 * 
 * @param data Source data to copy
 * @param length Number of bytes to write
 */
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

/**
 * @brief Copy data from the ring buffer to a linear destination.
 * 
 * Handles wrap-around when copying crosses the buffer boundary.
 * Does not modify buffer state (head/size unchanged).
 * 
 * @param offset Logical offset from buffer head
 * @param length Number of bytes to copy
 * @param dest Destination buffer (must have capacity >= length)
 */
void StreamParser::CopyOut(std::size_t offset, std::size_t length, uint8_t* dest) const {
    const std::size_t startIndex = (m_head + offset) % m_buffer.size();
    const std::size_t firstChunk = std::min(length, m_buffer.size() - startIndex);
    std::memcpy(dest, &m_buffer[startIndex], firstChunk);
    
    const std::size_t remaining = length - firstChunk;
    if (remaining > 0) {
        std::memcpy(dest + firstChunk, &m_buffer[0], remaining);
    }
}

/**
 * @brief Discard bytes from the front of the ring buffer.
 * 
 * Advances the head pointer and updates stream offset for error reporting.
 * Used after successfully processing a packet or skipping invalid data.
 * 
 * @param count Number of bytes to discard (clamped to available data)
 */
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

/**
 * @brief Parse buffered data looking for complete packets.
 * 
 * Main parsing loop that validates headers, looks up message types,
 * verifies CRC checksums, and emits packets or errors. Continues until
 * buffer is empty, data is insufficient for a packet, or iteration budget
 * is exhausted.
 * 
 * @param iterationBudget Remaining iterations allowed (decremented on each loop)
 */
void StreamParser::ParseBuffer(std::size_t& iterationBudget) {
    while (iterationBudget > 0 && m_size >= kHeaderSizeV3) {
        --iterationBudget;

        CopyOut(0, kHeaderSizeV3, m_decodeScratch.data());

        if (m_decodeScratch[kHeaderMajorIndex] != kProtocolMajorV3 ||
            m_decodeScratch[kHeaderMinorIndex] != kProtocolMinorV3) {
            const auto offset = m_streamOffset;
            EmitError(PacketError::UnsupportedVersion, offset);
            const std::size_t skip = FindNextHeaderCandidate();
            Discard(skip > 0 ? skip : 1);
            continue;
        }

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

/**
 * @brief Find the next potential packet header in the buffer.
 * 
 * Scans for the version magic bytes (major.minor) to find a resync point
 * after encountering corrupted data. Returns offset from current position.
 * 
 * @return Byte offset to next header candidate, or 1 if none found
 */
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

/**
 * @brief Look up the wire size for a message type.
 * 
 * Uses custom lookup function if set, otherwise falls back to the
 * global message type registry.
 * 
 * @param typeId Message type ID to look up
 * @return Wire size in bytes, or 0 if type is unknown
 */
std::size_t StreamParser::LookupWireSize(MessageTypeId typeId) const {
    // Use custom lookup if provided
    if (m_wireSizeLookup) {
        return m_wireSizeLookup(typeId);
    }
    // Fall back to global registry
    auto info = GetMessageInfo(typeId);
    return info ? info->wireSize : 0;
}

/**
 * @brief Emit a successfully decoded packet to the callback.
 * @param packet The validated packet view
 */
void StreamParser::EmitPacket(const PacketView& packet) {
    if (m_onPacket) {
        m_onPacket(packet);
    }
}

/**
 * @brief Emit a parse error to the callback.
 * 
 * Increments consecutive error counter and invokes error callback if set.
 * 
 * @param error The error code
 * @param offset Stream byte offset where error occurred
 */
void StreamParser::EmitError(PacketError error, std::size_t offset) {
    if (m_onError) {
        ErrorInfo info{error, offset, ++m_consecutiveErrors};
        m_onError(info);
    }
}

} // namespace bcnp
