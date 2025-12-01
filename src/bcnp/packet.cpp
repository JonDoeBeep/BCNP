#include "bcnp/packet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace bcnp {
namespace {

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

// PacketView Helper

std::size_t PacketView::GetPayloadSize() const {
    auto info = GetMessageInfo(header.messageType);
    if (!info) return 0;
    return info->wireSize * header.messageCount;
}

// V3 Encoding

bool EncodePacket(const Packet& packet, uint8_t* output, std::size_t capacity, std::size_t& bytesWritten) {
    bytesWritten = 0;
    if (packet.commands.size() > kMaxMessagesPerPacket || !output) {
        return false;
    }

    const std::size_t payloadSize = kHeaderSizeV3 + packet.commands.size() * DriveCmd::kWireSize;
    const std::size_t required = payloadSize + kChecksumSize;
    if (capacity < required) {
        return false;
    }

    // V3 Header: Major(1) + Minor(1) + Flags(1) + MsgTypeId(2) + MsgCount(2)
    output[kHeaderMajorIndex] = packet.header.major;
    output[kHeaderMinorIndex] = packet.header.minor;
    output[kHeaderFlagsIndex] = packet.header.flags;
    detail::StoreU16(static_cast<uint16_t>(packet.header.messageType), &output[kHeaderMsgTypeIndex]);
    detail::StoreU16(static_cast<uint16_t>(packet.commands.size()), &output[kHeaderMsgCountIndex]);

    // Encode each command using generated serializer
    std::size_t offset = kHeaderSizeV3;
    for (const auto& cmd : packet.commands) {
        if (!cmd.Encode(&output[offset], DriveCmd::kWireSize)) {
            return false;
        }
        offset += DriveCmd::kWireSize;
    }

    // CRC32 over header + payload
    const uint32_t crc = ComputeCrc32(output, payloadSize);
    detail::StoreU32(crc, &output[payloadSize]);

    bytesWritten = required;
    return true;
}

bool EncodePacket(const Packet& packet, std::vector<uint8_t>& output) {
    if (packet.commands.size() > kMaxMessagesPerPacket) {
        return false;
    }
    const std::size_t required = kHeaderSizeV3 + packet.commands.size() * DriveCmd::kWireSize + kChecksumSize;
    output.resize(required);
    std::size_t bytesWritten = 0;
    if (!EncodePacket(packet, output.data(), output.size(), bytesWritten)) {
        return false;
    }
    output.resize(bytesWritten);
    return true;
}

// V3 Decoding

DecodeViewResult DecodePacketView(const uint8_t* data, std::size_t length) {
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

    // Validate message type
    auto msgInfo = GetMessageInfo(header.messageType);
    if (!msgInfo) {
        result.error = PacketError::UnknownMessageType;
        result.bytesConsumed = 1;
        return result;
    }

    if (header.messageCount > kMaxMessagesPerPacket) {
        result.error = PacketError::TooManyCommands;
        result.bytesConsumed = 1;
        return result;
    }

    // Calculate sizes based on message type
    const std::size_t payloadSize = kHeaderSizeV3 + (header.messageCount * msgInfo->wireSize);
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

    // Type-specific validation (for DriveCmd, validate floats)
    if (header.messageType == MessageTypeId::DriveCmd) {
        std::size_t offset = kHeaderSizeV3;
        for (std::size_t i = 0; i < header.messageCount; ++i) {
            auto cmd = DriveCmd::Decode(&data[offset], DriveCmd::kWireSize);
            if (!cmd) {
                result.error = PacketError::InvalidFloat;
                result.bytesConsumed = expectedSize;
                return result;
            }
            offset += DriveCmd::kWireSize;
        }
    }
    // Add validation for other message types as needed

    PacketView view;
    view.header = header;
    view.payloadStart = data + kHeaderSizeV3;
    
    result.view = view;
    result.error = PacketError::None;
    result.bytesConsumed = expectedSize;
    return result;
}

DecodeResult DecodePacket(const uint8_t* data, std::size_t length) {
    auto viewResult = DecodePacketView(data, length);
    DecodeResult result;
    result.error = viewResult.error;
    result.bytesConsumed = viewResult.bytesConsumed;
    
    if (viewResult.view) {
        Packet packet;
        packet.header = viewResult.view->header;
        
        // Only decode DriveCmd for legacy Packet structure
        if (viewResult.view->header.messageType == MessageTypeId::DriveCmd) {
            packet.commands.reserve(packet.header.messageCount);
            for (const auto& cmd : *viewResult.view) {
                packet.commands.push_back(cmd);
            }
        }
        result.packet = std::move(packet);
    }
    return result;
}

// Typed Packet Encoding (Template Instantiations)

template<typename MsgType>
bool EncodeTypedPacket(const TypedPacket<MsgType>& packet, uint8_t* output, 
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

template<typename MsgType>
bool EncodeTypedPacket(const TypedPacket<MsgType>& packet, std::vector<uint8_t>& output) {
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

// Explicit instantiation for DriveCmd (core motion command type)
// Additional message types can be instantiated in user code as needed
template bool EncodeTypedPacket<DriveCmd>(const TypedPacket<DriveCmd>&, uint8_t*, std::size_t, std::size_t&);
template bool EncodeTypedPacket<DriveCmd>(const TypedPacket<DriveCmd>&, std::vector<uint8_t>&);
template std::optional<TypedPacket<DriveCmd>> DecodeTypedPacket<DriveCmd>(const PacketView&);

} // namespace bcnp
