#pragma once

#include <bcnp/message_types.h>
#include "bcnp/static_vector.h"
#include "bcnp/packet_storage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bcnp {

// Primary constants from message_types.h:
// kProtocolMajorV3, kProtocolMinorV3, kSchemaHash, kHeaderSizeV3,
// kHeaderMsgTypeIndex, kHeaderMsgCountIndex

// Common constants
constexpr std::size_t kChecksumSize = 4;
constexpr std::size_t kMaxMessagesPerPacket = 65535;
constexpr uint8_t kFlagClearQueue = 0x01;

// Use V3 as the active protocol
constexpr uint8_t kProtocolMajor = kProtocolMajorV3;
constexpr uint8_t kProtocolMinor = kProtocolMinorV3;
constexpr std::size_t kHeaderSize = kHeaderSizeV3;

// Header field indices
constexpr std::size_t kHeaderMajorIndex = 0;
constexpr std::size_t kHeaderMinorIndex = 1;
constexpr std::size_t kHeaderFlagsIndex = 2;
// kHeaderMsgTypeIndex = 3 (from generated)
// kHeaderMsgCountIndex = 5 (from generated)

// Packet Header

struct PacketHeader {
    uint8_t major{kProtocolMajorV3};
    uint8_t minor{kProtocolMinorV3};
    uint8_t flags{0};
    MessageTypeId messageType{MessageTypeId::Unknown};
    uint16_t messageCount{0};
};

// Message Iterators (Zero-Copy Parsing)

/// Generic iterator for typed messages in a packet view
template<typename MsgType>
class MessageIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = MsgType;
    using difference_type = std::ptrdiff_t;
    using pointer = const MsgType*;
    using reference = MsgType;

    MessageIterator(const uint8_t* ptr, std::size_t count) 
        : m_ptr(ptr), m_count(count) {}

    MsgType operator*() const {
        auto result = MsgType::Decode(m_ptr, MsgType::kWireSize);
        return result.value_or(MsgType{});
    }

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

    MessageIterator operator++(int) {
        MessageIterator tmp = *this;
        ++(*this);
        return tmp;
    }

    bool operator==(const MessageIterator& other) const {
        if (m_count == 0 && other.m_count == 0) return true;
        return m_ptr == other.m_ptr && m_count == other.m_count;
    }

    bool operator!=(const MessageIterator& other) const {
        return !(*this == other);
    }

private:
    const uint8_t* m_ptr;
    std::size_t m_count;
};

// Packet View (Zero-Copy)

struct PacketView {
    PacketHeader header{};
    const uint8_t* payloadStart{nullptr};
    
    /// Get message type ID
    MessageTypeId GetMessageType() const { return header.messageType; }
    
    /// Type-safe iteration for any message type
    template<typename MsgType>
    MessageIterator<MsgType> begin_as() const {
        if (MsgType::kTypeId != header.messageType) {
            return MessageIterator<MsgType>(nullptr, 0);
        }
        return MessageIterator<MsgType>(payloadStart, header.messageCount);
    }
    
    template<typename MsgType>
    MessageIterator<MsgType> end_as() const {
        return MessageIterator<MsgType>(nullptr, 0);
    }
    
    /// Get raw payload for manual parsing
    const uint8_t* GetPayload() const { return payloadStart; }
    std::size_t GetPayloadSize() const;
};

// ============================================================================
// TypedPacket with Configurable Storage
// ============================================================================

/**
 * @brief Generic packet containing messages of a specific type.
 * 
 * @tparam MsgType The message struct type (must have kTypeId, kWireSize, Encode/Decode)
 * @tparam Storage Container type for messages (default: std::vector<MsgType>)
 * 
 * Storage options:
 * - std::vector<MsgType>: Heap allocation, unlimited size (default, backward compatible)
 * - StaticVector<MsgType, N>: Stack allocation, fixed capacity N (real-time safe)
 * 
 * Example:
 *   // Heap-allocated (large batches, trajectory uploads)
 *   TypedPacket<DriveCmd> heapPacket;
 *   
 *   // Stack-allocated (control loop, telemetry - 64 message default)
 *   StaticTypedPacket<DriveCmd> stackPacket;
 *   
 *   // Custom capacity
 *   TypedPacket<DriveCmd, StaticVector<DriveCmd, 128>> customPacket;
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

/// Convenience alias for heap-allocated packets (backward compatible)
template<typename MsgType>
using DynamicTypedPacket = TypedPacket<MsgType, std::vector<MsgType>>;

enum class PacketError {
    None,
    TooSmall,
    UnsupportedVersion,
    TooManyMessages,
    TooManyCommands = TooManyMessages,  // Legacy alias
    Truncated,
    InvalidFloat,
    ChecksumMismatch,
    UnknownMessageType,
    HandshakeRequired,
    SchemaMismatch
};

struct DecodeViewResult {
    std::optional<PacketView> view;
    PacketError error{PacketError::None};
    std::size_t bytesConsumed{0};
};

// CRC32 Utility - declared first so template functions can use it
uint32_t ComputeCrc32(const uint8_t* data, std::size_t length);

// ============================================================================
// Encoding Functions (Template-based, header-only)
// ============================================================================

/// Encode a typed packet to buffer (works with any storage type)
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

/// Encode a typed packet to vector (allocates) - works with any storage type
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

/// Decode to typed packet from PacketView (default: heap-allocated storage)
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

/// Decode to typed packet with custom storage type
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

// ============================================================================
// Decoding Functions (Implemented in packet.cpp)
// ============================================================================

/// Decode packet view with explicit wire size (when message type is known)
DecodeViewResult DecodePacketViewWithSize(const uint8_t* data, std::size_t length, std::size_t wireSize);

/// Decode packet view using registry lookup (requires registered message types)
DecodeViewResult DecodePacketView(const uint8_t* data, std::size_t length);

/// Type-safe packet view decode (uses message type's kWireSize)
template<typename MsgType>
DecodeViewResult DecodePacketViewAs(const uint8_t* data, std::size_t length) {
    return DecodePacketViewWithSize(data, length, MsgType::kWireSize);
}

} // namespace bcnp
