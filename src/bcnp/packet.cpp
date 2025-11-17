#include "bcnp/packet.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bcnp {

namespace {
uint32_t LoadU32(const uint8_t* data) {
    return (uint32_t(data[0]) << 24) |
           (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8) |
           uint32_t(data[3]);
}

uint16_t LoadU16(const uint8_t* data) {
    return (uint16_t(data[0]) << 8) | uint16_t(data[1]);
}

void StoreU32(uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(value & 0xFF);
}

void StoreU16(uint16_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(value & 0xFF);
}
} // namespace

bool EncodePacket(const Packet& packet, std::vector<uint8_t>& output) {
    if (packet.commands.size() > kMaxCommandsPerPacket) {
        return false;
    }

    output.resize(kHeaderSize + packet.commands.size() * kCommandSize);
    output[kHeaderMajorIndex] = packet.header.major;
    output[kHeaderMinorIndex] = packet.header.minor;
    output[kHeaderFlagsIndex] = packet.header.flags;
    output[kHeaderCountIndex] = static_cast<uint8_t>(packet.commands.size());

    std::size_t offset = kHeaderSize;
    for (const auto& cmd : packet.commands) {
        uint32_t vxBits;
        uint32_t omegaBits;
        std::memcpy(&vxBits, &cmd.vx, sizeof(float));
        std::memcpy(&omegaBits, &cmd.omega, sizeof(float));
        StoreU32(vxBits, &output[offset]);
        StoreU32(omegaBits, &output[offset + 4]);
        StoreU16(cmd.durationMs, &output[offset + 8]);
        offset += kCommandSize;
    }

    return true;
}

DecodeResult DecodePacket(const uint8_t* data, std::size_t length) {
    DecodeResult result{};

    if (length < kHeaderSize) {
        result.error = PacketError::TooSmall;
        return result;
    }

    Packet packet{};
    packet.header.major = data[kHeaderMajorIndex];
    packet.header.minor = data[kHeaderMinorIndex];
    packet.header.flags = data[kHeaderFlagsIndex];
    packet.header.commandCount = data[kHeaderCountIndex];

    if (packet.header.major != kProtocolMajor || packet.header.minor != kProtocolMinor) {
        result.error = PacketError::UnsupportedVersion;
        result.bytesConsumed = kHeaderSize;
        return result;
    }

    if (packet.header.commandCount > kMaxCommandsPerPacket) {
        result.error = PacketError::TooManyCommands;
        result.bytesConsumed = kHeaderSize;
        return result;
    }

    const std::size_t expectedSize = kHeaderSize + (packet.header.commandCount * kCommandSize);
    if (length < expectedSize) {
        result.error = PacketError::Truncated;
        return result;
    }

    packet.commands.reserve(packet.header.commandCount);

    std::size_t offset = kHeaderSize;
    for (std::size_t i = 0; i < packet.header.commandCount; ++i) {
        const uint32_t vxBits = LoadU32(&data[offset]);
        const uint32_t omegaBits = LoadU32(&data[offset + 4]);
        const uint16_t duration = LoadU16(&data[offset + 8]);

        float vx;
        float omega;
        std::memcpy(&vx, &vxBits, sizeof(float));
        std::memcpy(&omega, &omegaBits, sizeof(float));

        if (!std::isfinite(vx) || !std::isfinite(omega)) {
            result.error = PacketError::InvalidFloat;
            result.bytesConsumed = expectedSize;
            return result;
        }

        packet.commands.push_back(Command{vx, omega, duration});
        offset += kCommandSize;
    }

    result.packet = std::move(packet);
    result.error = PacketError::None;
    result.bytesConsumed = expectedSize;
    return result;
}

} // namespace bcnp
