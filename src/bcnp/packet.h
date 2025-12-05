#pragma once

/**
 * @file packet.h
 * @brief BCNP packet structures, encoding, and decoding utilities.
 * 
 * This header defines the core packet format for the BCNP (Binary Control Network Protocol)
 * including packet headers, typed packet containers, message iterators, and encoding/decoding
 * functions. Supports both heap-allocated and stack-allocated (real-time safe) storage.
 * 
 */

#include <bcnp/message_types.h>
#include "bcnp/static_vector.h"
#include "bcnp/packet_storage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bcnp {

/**
 * @defgroup ProtocolConstants Protocol Constants (V3)
 * @brief Wire format constants for BCNP protocol version 3.
 * @{
 */

/** @brief Size of CRC32 checksum in bytes. */
constexpr std::size_t kChecksumSize = 4;

/** @brief Maximum number of messages allowed in a single packet. */
constexpr std::size_t kMaxMessagesPerPacket = 65535;

/** @brief Packet flag: Clear command queue before processing this packet. */
constexpr uint8_t kFlagClearQueue = 0x01;

/** @brief Current protocol major version. */
constexpr uint8_t kProtocolMajor = kProtocolMajorV3;

/** @brief Current protocol minor version. */
constexpr uint8_t kProtocolMinor = kProtocolMinorV3;

/** @brief Size of packet header in bytes. */
constexpr std::size_t kHeaderSize = kHeaderSizeV3;

/** @} */ // end of ProtocolConstants

/**
 * @defgroup HeaderOffsets Header Byte Offsets
 * @brief Byte indices for header fields in wire format.
 * @{
 */

/** @brief Byte offset of major version in header. */
constexpr std::size_t kHeaderMajorIndex = 0;

/** @brief Byte offset of minor version in header. */
constexpr std::size_t kHeaderMinorIndex = 1;

/** @brief Byte offset of flags byte in header. */
constexpr std::size_t kHeaderFlagsIndex = 2;

// kHeaderMsgTypeIndex = 3, kHeaderMsgCountIndex = 5 (from generated header)
/** @} */ // end of HeaderOffsets

/**
 * @brief Parsed packet header structure.
 * 
 * Contains all metadata from the packet header including version info,
 * flags, message type, and count. Used both for decoding received packets
 * and constructing outbound packets.
 */
struct PacketHeader {
    uint8_t major{kProtocolMajorV3};          ///< Protocol major version
    uint8_t minor{kProtocolMinorV3};          ///< Protocol minor version
    uint8_t flags{0};                          ///< Packet flags (e.g., kFlagClearQueue)
    MessageTypeId messageType{MessageTypeId::Unknown}; ///< Type ID of messages in payload
    uint16_t messageCount{0};                  ///< Number of messages in payload
};

/**
 * @brief Forward iterator for zero-copy message access from packet payload.
 * 
 * Allows iterating over messages in a PacketView without copying them to
 * intermediate storage. Each dereference decodes the message on-the-fly
 * from the raw wire bytes.
 * 
 * @tparam MsgType The message struct type (must have Decode() and kWireSize)
 * 
 * @code{cpp}
 * for (auto it = view.begin_as<DriveCmd>(); it != view.end_as<DriveCmd>(); ++it) {
 *     DriveCmd cmd = *it;  // Decoded on access
 *     // Process cmd...
 * }
 * @endcode
 */
template<typename MsgType>
class MessageIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = MsgType;
    using difference_type = std::ptrdiff_t;
    using pointer = const MsgType*;
    using reference = MsgType;

    /**
     * @brief Construct iterator at a position in the payload.
     * @param ptr Pointer to the first message byte
     * @param count Number of messages remaining from this position
     */
    MessageIterator(const uint8_t* ptr, std::size_t count) 
        : m_ptr(ptr), m_count(count) {}

    /**
     * @brief Decode and return the current message.
     * @return Decoded message, or default-constructed MsgType on decode failure
     */
    MsgType operator*() const {
        auto result = MsgType::Decode(m_ptr, MsgType::kWireSize);
        return result.value_or(MsgType{});
    }

    /** @brief Advance to the next message (pre-increment). */
    MessageIterator& operator++() {
        if (m_count > 0) {
            m_ptr += MsgType::kWireSize;
            m_count--;
        }
        if (m_count == 0) {
            m_ptr = nullptr;
        }
        return *this;
    }

    /** @brief Advance to the next message (post-increment). */
    MessageIterator operator++(int) {
        MessageIterator tmp = *this;
        ++(*this);
        return tmp;
    }

    /** @brief Equality comparison (end iterators compare equal). */
    bool operator==(const MessageIterator& other) const {
        if (m_count == 0 && other.m_count == 0) return true;
        return m_ptr == other.m_ptr && m_count == other.m_count;
    }

    /** @brief Inequality comparison. */
    bool operator!=(const MessageIterator& other) const {
        return !(*this == other);
    }

private:
    const uint8_t* m_ptr;   ///< Current position in payload buffer
    std::size_t m_count;     ///< Remaining messages from current position
};

/**
 * @brief Zero-copy view into a decoded packet buffer.
 * 
 * PacketView provides read-only access to a validated packet without
 * copying the message data. Use begin_as<T>/end_as<T> to iterate over
 * messages with type checking, or GetPayload() for raw access.
 * 
 * @note The view is only valid while the underlying buffer exists.
 *       Do not store views beyond the lifetime of the source buffer.
 */
struct PacketView {
    PacketHeader header{};              ///< Parsed header information
    const uint8_t* payloadStart{nullptr}; ///< Pointer to first message in buffer
    
    /**
     * @brief Get the message type ID for this packet.
     * @return The MessageTypeId from the header
     */
    MessageTypeId GetMessageType() const { return header.messageType; }
    
    /**
     * @brief Get type-safe iterator to the first message.
     * 
     * Returns an empty iterator if MsgType::kTypeId doesn't match the packet's
     * message type, providing compile-time type safety with runtime validation.
     * 
     * @tparam MsgType The expected message type
     * @return Iterator to first message, or end iterator on type mismatch
     */
    template<typename MsgType>
    MessageIterator<MsgType> begin_as() const {
        if (MsgType::kTypeId != header.messageType) {
            return MessageIterator<MsgType>(nullptr, 0);
        }
        return MessageIterator<MsgType>(payloadStart, header.messageCount);
    }
    
    /**
     * @brief Get end iterator for type-safe message iteration.
     * @tparam MsgType The message type (must match begin_as call)
     * @return End sentinel iterator
     */
    template<typename MsgType>
    MessageIterator<MsgType> end_as() const {
        return MessageIterator<MsgType>(nullptr, 0);
    }
    
    /**
     * @brief Get raw pointer to payload for manual parsing.
     * @return Pointer to first byte after header
     */
    const uint8_t* GetPayload() const { return payloadStart; }
    
    /**
     * @brief Calculate total payload size in bytes.
     * @return messageCount * wireSize for the message type
     */
    std::size_t GetPayloadSize() const;
};

/**
 * @brief Generic packet containing messages of a specific type.
 * 
 * @tparam MsgType The message struct type (must have kTypeId, kWireSize, Encode/Decode)
 * @tparam Storage Container type for messages (default: std::vector<MsgType>)
 * 
 * Storage options:
 * std::vector<MsgType>: Heap allocation, unlimited size (default)
 * StaticVector<MsgType, N>: Stack allocation, fixed capacity N (real-time safe)
 * 
 * @code{cpp}
 *   // Heap-allocated (large batches, trajectory uploads)
 *   TypedPacket<DriveCmd> heapPacket;
 *   
 *   // Stack-allocated (control loop, telemetry - 64 message default)
 *   StaticTypedPacket<DriveCmd> stackPacket;
 *   
 *   // Custom capacity
 *   TypedPacket<DriveCmd, StaticVector<DriveCmd, 128>> customPacket;
 * @endcode
 */
template<typename MsgType, typename Storage = std::vector<MsgType>>
struct TypedPacket {
    static_assert(IsValidPacketStorage_v<Storage>, 
        "Storage must be a valid packet storage container (vector-like interface)");
    
    PacketHeader header{};
    Storage messages{};
    
    TypedPacket() {
        header.messageType = MsgType::kTypeId;
    }
};

/// Convenience alias for stack-allocated real-time packets (default 64 messages)
template<typename MsgType, std::size_t Capacity = 64>
using StaticTypedPacket = TypedPacket<MsgType, StaticVector<MsgType, Capacity>>;

/** @brief Convenience alias for heap-allocated packets (backward compatible). */
template<typename MsgType>
using DynamicTypedPacket = TypedPacket<MsgType, std::vector<MsgType>>;

/**
 * @brief Error codes returned by packet decoding operations.
 * 
 * These errors indicate various failure modes during packet parsing,
 * from simple size issues to protocol mismatches and data corruption.
 */
enum class PacketError {
    None,                   ///< No error - packet decoded successfully
    TooSmall,               ///< Buffer too small to contain header
    UnsupportedVersion,     ///< Protocol version mismatch
    TooManyMessages,        ///< Message count exceeds kMaxMessagesPerPacket
    TooManyCommands = TooManyMessages,  ///< @deprecated Legacy alias for TooManyMessages
    Truncated,              ///< Buffer ends before expected packet length
    InvalidFloat,           ///< NaN or Inf detected in float field
    ChecksumMismatch,       ///< CRC32 validation failed
    UnknownMessageType,     ///< Message type ID not in registry
    HandshakeRequired,      ///< Connection requires handshake first
    SchemaMismatch          ///< Client/server schema hash mismatch
};

/**
 * @brief Result of decoding a packet from raw bytes.
 * 
 * Contains either a valid PacketView or an error code. Always check
 * `error` before accessing `view`. The `bytesConsumed` field indicates
 * how many bytes were processed (useful for stream parsing).
 */
struct DecodeViewResult {
    std::optional<PacketView> view;     ///< Decoded view (valid if error == None)
    PacketError error{PacketError::None}; ///< Error code if decode failed
    std::size_t bytesConsumed{0};        ///< Bytes consumed from input buffer
};

/**
 * @brief Compute CRC32 checksum for data integrity verification.
 * 
 * Uses the standard CRC32 polynomial (0xEDB88320) with initial value
 * of 0xFFFFFFFF and final XOR. Compatible with common CRC32 implementations.
 * 
 * @param data Pointer to data buffer
 * @param length Number of bytes to checksum
 * @return 32-bit CRC checksum
 */
uint32_t ComputeCrc32(const uint8_t* data, std::size_t length);

/**
 * @brief Encode a typed packet to a pre-allocated buffer.
 * 
 * Serializes the packet header and all messages to wire format, appending
 * a CRC32 checksum. Works with any storage type (vector, StaticVector, etc.).
 * 
 * @tparam MsgType Message struct type with Encode() and kWireSize
 * @tparam Storage Container type holding the messages
 * @param packet The packet to encode
 * @param output Destination buffer (must have sufficient capacity)
 * @param capacity Size of output buffer in bytes
 * @param[out] bytesWritten Number of bytes written on success
 * @return true if encoding succeeded, false if capacity insufficient or encoding failed
 */
template<typename MsgType, typename Storage>
bool EncodeTypedPacket(const TypedPacket<MsgType, Storage>& packet, uint8_t* output, 
                       std::size_t capacity, std::size_t& bytesWritten) {
    bytesWritten = 0;
    if (packet.messages.size() > kMaxMessagesPerPacket || !output) {
        return false;
    }

    const std::size_t payloadSize = kHeaderSizeV3 + packet.messages.size() * MsgType::kWireSize;
    const std::size_t required = payloadSize + kChecksumSize;
    if (capacity < required) {
        return false;
    }

    // V3 Header
    output[kHeaderMajorIndex] = packet.header.major;
    output[kHeaderMinorIndex] = packet.header.minor;
    output[kHeaderFlagsIndex] = packet.header.flags;
    detail::StoreU16(static_cast<uint16_t>(MsgType::kTypeId), &output[kHeaderMsgTypeIndex]);
    detail::StoreU16(static_cast<uint16_t>(packet.messages.size()), &output[kHeaderMsgCountIndex]);

    // Encode messages
    std::size_t offset = kHeaderSizeV3;
    for (const auto& msg : packet.messages) {
        if (!msg.Encode(&output[offset], MsgType::kWireSize)) {
            return false;
        }
        offset += MsgType::kWireSize;
    }

    // CRC32
    const uint32_t crc = ComputeCrc32(output, payloadSize);
    detail::StoreU32(crc, &output[payloadSize]);

    bytesWritten = required;
    return true;
}

/**
 * @brief Encode a typed packet to a dynamically-sized vector.
 * 
 * Convenience overload that allocates the required buffer automatically.
 * Suitable for non-real-time code paths where allocation is acceptable.
 * 
 * @tparam MsgType Message struct type with Encode() and kWireSize
 * @tparam Storage Container type holding the messages
 * @param packet The packet to encode
 * @param[out] output Vector to receive encoded bytes (resized automatically)
 * @return true if encoding succeeded, false on encoding failure
 */
template<typename MsgType, typename Storage>
bool EncodeTypedPacket(const TypedPacket<MsgType, Storage>& packet, std::vector<uint8_t>& output) {
    if (packet.messages.size() > kMaxMessagesPerPacket) {
        return false;
    }
    const std::size_t required = kHeaderSizeV3 + packet.messages.size() * MsgType::kWireSize + kChecksumSize;
    output.resize(required);
    std::size_t bytesWritten = 0;
    if (!EncodeTypedPacket(packet, output.data(), output.size(), bytesWritten)) {
        return false;
    }
    output.resize(bytesWritten);
    return true;
}

/**
 * @brief Decode messages from a PacketView into a typed packet.
 * 
 * Converts a validated PacketView into a TypedPacket with heap-allocated
 * storage. Returns nullopt if the view's message type doesn't match MsgType.
 * 
 * @tparam MsgType Expected message type
 * @param view Validated packet view to decode from
 * @return TypedPacket containing decoded messages, or nullopt on type mismatch
 */
template<typename MsgType>
std::optional<TypedPacket<MsgType>> DecodeTypedPacket(const PacketView& view) {
    if (view.header.messageType != MsgType::kTypeId) {
        return std::nullopt;
    }
    
    TypedPacket<MsgType> packet;
    packet.header = view.header;
    packet.messages.reserve(view.header.messageCount);
    
    const uint8_t* ptr = view.payloadStart;
    for (std::size_t i = 0; i < view.header.messageCount; ++i) {
        auto msg = MsgType::Decode(ptr, MsgType::kWireSize);
        if (!msg) {
            return std::nullopt;
        }
        packet.messages.push_back(*msg);
        ptr += MsgType::kWireSize;
    }
    
    return packet;
}

/**
 * @brief Decode messages from a PacketView with custom storage type.
 * 
 * Allows decoding into stack-allocated or custom storage for real-time
 * applications. Use StaticVector<MsgType, N> for deterministic memory usage.
 * 
 * @tparam MsgType Expected message type
 * @tparam Storage Container type (e.g., StaticVector<MsgType, 64>)
 * @param view Validated packet view to decode from
 * @return TypedPacket with specified storage, or nullopt on type mismatch/decode failure
 */
template<typename MsgType, typename Storage>
std::optional<TypedPacket<MsgType, Storage>> DecodeTypedPacketAs(const PacketView& view) {
    if (view.header.messageType != MsgType::kTypeId) {
        return std::nullopt;
    }
    
    TypedPacket<MsgType, Storage> packet;
    packet.header = view.header;
    ReserveIfPossible(packet.messages, view.header.messageCount);
    
    const uint8_t* ptr = view.payloadStart;
    for (std::size_t i = 0; i < view.header.messageCount; ++i) {
        auto msg = MsgType::Decode(ptr, MsgType::kWireSize);
        if (!msg) {
            return std::nullopt;
        }
        packet.messages.push_back(*msg);
        ptr += MsgType::kWireSize;
    }
    
    return packet;
}

/**
 * @brief Decode packet view with explicit wire size.
 * 
 * Low-level decode function used when the message wire size is known
 * (e.g., from a custom lookup table or test fixture).
 * 
 * @param data Raw packet bytes
 * @param length Number of bytes available
 * @param wireSize Size of each message in bytes
 * @return DecodeViewResult with view and error status
 */
DecodeViewResult DecodePacketViewWithSize(const uint8_t* data, std::size_t length, std::size_t wireSize);

/**
 * @brief Decode packet view using the global message type registry.
 * 
 * Looks up the wire size from GetMessageInfo() based on the message type
 * ID in the packet header. Use this for general-purpose packet handling.
 * 
 * @param data Raw packet bytes
 * @param length Number of bytes available
 * @return DecodeViewResult with view and error status
 */
DecodeViewResult DecodePacketView(const uint8_t* data, std::size_t length);

/**
 * @brief Type-safe packet view decode using compile-time message type.
 * 
 * Template version that uses MsgType::kWireSize directly, avoiding
 * runtime registry lookup. Preferred when the expected type is known.
 * 
 * @tparam MsgType Expected message type with kWireSize constant
 * @param data Raw packet bytes
 * @param length Number of bytes available
 * @return DecodeViewResult with view and error status
 */
template<typename MsgType>
DecodeViewResult DecodePacketViewAs(const uint8_t* data, std::size_t length) {
    return DecodePacketViewWithSize(data, length, MsgType::kWireSize);
}

} // namespace bcnp
