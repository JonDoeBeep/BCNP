/**
 * @file packet.cpp
 * @brief Implementation of BCNP packet encoding and decoding functions.
 * 
 * Contains the CRC32 computation, packet view decoding, and payload size
 * calculation. Encoding is handled by template functions in the header.
 */

#include "bcnp/packet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace bcnp {
namespace {

/**
 * @brief Generate CRC32 lookup table at compile time.
 * 
 * Uses the standard CRC32 polynomial (reversed: 0xEDB88320).
 * The table is computed once at compile time using constexpr.
 * 
 * @return 256-entry lookup table for byte-at-a-time CRC computation
 */
constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (uint32_t bit = 0; bit < 8; ++bit) {
            if (crc & 1U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
        table[i] = crc;
    }
    return table;
}

/// @brief Compile-time CRC32 lookup table.
constexpr auto kCrc32Table = MakeCrcTable();

} // namespace

uint32_t ComputeCrc32(const uint8_t* data, std::size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < length; ++i) {
        const uint8_t index = static_cast<uint8_t>((crc ^ data[i]) & 0xFFU);
        crc = (crc >> 8U) ^ kCrc32Table[index];
    }
    return crc ^ 0xFFFFFFFFU;
}

std::size_t PacketView::GetPayloadSize() const {
    auto info = GetMessageInfo(header.messageType);
    if (!info) return 0;
    return info->wireSize * header.messageCount;
}


/**
 * @brief Decode a packet with explicitly provided message wire size.
 * 
 * This is the core decoding function used by all other decode variants.
 * Validates the header, checks CRC, and constructs a PacketView on success.
 * 
 * @param data Pointer to raw packet bytes
 * @param length Available bytes in buffer
 * @param wireSize Size of each message in bytes (from schema or lookup)
 * @return DecodeViewResult with validation status and optional view
 */
DecodeViewResult DecodePacketViewWithSize(const uint8_t* data, std::size_t length, std::size_t wireSize) {
    DecodeViewResult result{};

    if (length < kHeaderSizeV3) {
        result.error = PacketError::TooSmall;
        return result;
    }

    // Parse header
    PacketHeader header;
    header.major = data[kHeaderMajorIndex];
    header.minor = data[kHeaderMinorIndex];
    header.flags = data[kHeaderFlagsIndex];
    header.messageType = static_cast<MessageTypeId>(detail::LoadU16(&data[kHeaderMsgTypeIndex]));
    header.messageCount = detail::LoadU16(&data[kHeaderMsgCountIndex]);

    // Version check
    if (header.major != kProtocolMajorV3 || header.minor != kProtocolMinorV3) {
        result.error = PacketError::UnsupportedVersion;
        result.bytesConsumed = 1;
        return result;
    }

    if (header.messageCount > kMaxMessagesPerPacket) {
        result.error = PacketError::TooManyMessages;
        result.bytesConsumed = 1;
        return result;
    }

    // Calculate sizes based on provided wire size
    const std::size_t payloadSize = kHeaderSizeV3 + (header.messageCount * wireSize);
    const std::size_t expectedSize = payloadSize + kChecksumSize;
    if (length < expectedSize) {
        result.error = PacketError::Truncated;
        return result;
    }

    // CRC validation
    const uint32_t transmittedCrc = detail::LoadU32(&data[payloadSize]);
    const uint32_t computedCrc = ComputeCrc32(data, payloadSize);
    if (transmittedCrc != computedCrc) {
        result.error = PacketError::ChecksumMismatch;
        result.bytesConsumed = expectedSize;
        return result;
    }

    PacketView view;
    view.header = header;
    // Calculate payload size: messageCount * wireSize
    const std::size_t payloadBytes = header.messageCount * wireSize;
    view.payload = crab::Slice<const uint8_t>(data + kHeaderSizeV3, payloadBytes);
    
    result.view = crab::Some(view);
    result.error = PacketError::None;
    result.bytesConsumed = expectedSize;
    return result;
}

/**
 * @brief Decode a packet using the global message type registry.
 * 
 * Extracts the message type ID from the header and looks up the wire size
 * from GetMessageInfo(). Returns UnknownMessageType error if the type is
 * not registered.
 * 
 * @param data Pointer to raw packet bytes
 * @param length Available bytes in buffer
 * @return DecodeViewResult with validation status and optional view
 */
DecodeViewResult DecodePacketView(const uint8_t* data, std::size_t length) {
    DecodeViewResult result{};

    if (length < kHeaderSizeV3) {
        result.error = PacketError::TooSmall;
        return result;
    }

    // Parse header to get message type
    const auto messageType = static_cast<MessageTypeId>(detail::LoadU16(&data[kHeaderMsgTypeIndex]));
    
    // Look up wire size from registry
    auto msgInfo = GetMessageInfo(messageType);
    if (!msgInfo) {
        result.error = PacketError::UnknownMessageType;
        result.bytesConsumed = 1;
        return result;
    }

    return DecodePacketViewWithSize(data, length, msgInfo->wireSize);
}

} // namespace bcnp
