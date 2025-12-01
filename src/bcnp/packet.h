#pragma once

#include "message_types.h"
#include "bcnp/static_vector.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bcnp {

// Primary constants from message_types.h:
// kProtocolMajorV3, kProtocolMinorV3, kSchemaHash, kHeaderSizeV3,
// kHeaderMsgTypeIndex, kHeaderMsgCountIndex, kDriveCmdSize, etc.

// Common constants
constexpr std::size_t kChecksumSize = 4;
constexpr std::size_t kMaxMessagesPerPacket = 65535;
constexpr uint8_t kFlagClearQueue = 0x01;

// Max packet size (header + max payload + CRC)
// For DriveCmd: 7 + (10 * 65535) + 4 = 655361 bytes
constexpr std::size_t kMaxPayloadSize = kHeaderSizeV3 + (kDriveCmdSize * kMaxMessagesPerPacket);
constexpr std::size_t kMaxPacketSize = kMaxPayloadSize + kChecksumSize;

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

// Convenience aliases
constexpr std::size_t kMaxCommandsPerPacket = kMaxMessagesPerPacket;
constexpr std::size_t kCommandSize = kDriveCmdSize;

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

// Command type alias (DriveCmd is the primary command type)
using Command = DriveCmd;
using CommandIterator = MessageIterator<DriveCmd>;

// Packet View (Zero-Copy)

struct PacketView {
    PacketHeader header{};
    const uint8_t* payloadStart{nullptr};
    
    /// Get message type ID
    MessageTypeId GetMessageType() const { return header.messageType; }
    
    /// Iterate as DriveCmd
    MessageIterator<DriveCmd> begin() const { 
        return MessageIterator<DriveCmd>(payloadStart, header.messageCount); 
    }
    MessageIterator<DriveCmd> end() const { 
        return MessageIterator<DriveCmd>(nullptr, 0); 
    }
    
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

/// Generic packet containing messages of a specific type
template<typename MsgType>
struct TypedPacket {
    PacketHeader header{};
    std::vector<MsgType> messages{};
    
    TypedPacket() {
        header.messageType = MsgType::kTypeId;
    }
};

// DriveCmd packet (most common)
struct Packet {
    PacketHeader header{};
    std::vector<DriveCmd> commands{};
    
    Packet() {
        header.messageType = MessageTypeId::DriveCmd;
    }
};

enum class PacketError {
    None,
    TooSmall,
    UnsupportedVersion,
    TooManyCommands,  // Legacy name
    TooManyMessages = TooManyCommands,
    Truncated,
    InvalidFloat,
    ChecksumMismatch,
    UnknownMessageType,
    HandshakeRequired,
    SchemaMismatch
};

struct DecodeResult {
    std::optional<Packet> packet;
    PacketError error{PacketError::None};
    std::size_t bytesConsumed{0};
};

struct DecodeViewResult {
    std::optional<PacketView> view;
    PacketError error{PacketError::None};
    std::size_t bytesConsumed{0};
};

// Encoding Functions

/// Encode a typed packet to buffer
template<typename MsgType>
bool EncodeTypedPacket(const TypedPacket<MsgType>& packet, uint8_t* output, 
                       std::size_t capacity, std::size_t& bytesWritten);

/// Encode a typed packet to vector (allocates)
template<typename MsgType>
bool EncodeTypedPacket(const TypedPacket<MsgType>& packet, std::vector<uint8_t>& output);

/// Encode legacy Packet (DriveCmd) - WARNING: Allocates on heap
bool EncodePacket(const Packet& packet, std::vector<uint8_t>& output);
bool EncodePacket(const Packet& packet, uint8_t* output, std::size_t capacity, std::size_t& bytesWritten);

// Decoding Functions

/// Decode packet view (zero-copy, validates header and CRC)
DecodeViewResult DecodePacketView(const uint8_t* data, std::size_t length);

/// Decode packet (allocates, copies messages)
DecodeResult DecodePacket(const uint8_t* data, std::size_t length);

/// Decode to typed packet
template<typename MsgType>
std::optional<TypedPacket<MsgType>> DecodeTypedPacket(const PacketView& view);

// CRC32 Utility (exposed for handshake)

uint32_t ComputeCrc32(const uint8_t* data, std::size_t length);

} // namespace bcnp
