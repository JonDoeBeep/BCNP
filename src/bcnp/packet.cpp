#include "bcnp/packet.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

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

int32_t LoadS32(const uint8_t* data) {
    return static_cast<int32_t>(LoadU32(data));
}

void StoreS32(int32_t value, uint8_t* out) {
    StoreU32(static_cast<uint32_t>(value), out);
}

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

uint32_t ComputeCrc32(const uint8_t* data, std::size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < length; ++i) {
        const uint8_t index = static_cast<uint8_t>((crc ^ data[i]) & 0xFFU);
        crc = (crc >> 8U) ^ kCrc32Table[index];
    }
    return crc ^ 0xFFFFFFFFU;
}

int32_t QuantizeScaled(float value, float scale) {
    const double scaled = static_cast<double>(value) * static_cast<double>(scale);
    const double clamped = std::clamp(scaled,
        static_cast<double>(std::numeric_limits<int32_t>::min()),
        static_cast<double>(std::numeric_limits<int32_t>::max()));
    return static_cast<int32_t>(std::llround(clamped));
}

float DequantizeScaled(int32_t fixed, float scale) {
    return static_cast<float>(static_cast<double>(fixed) / static_cast<double>(scale));
}
} // namespace

bool EncodePacket(const Packet& packet, uint8_t* output, std::size_t capacity, std::size_t& bytesWritten) {
    bytesWritten = 0;
    if (packet.commands.size() > kMaxCommandsPerPacket || !output) {
        return false;
    }

    const std::size_t payloadSize = kHeaderSize + packet.commands.size() * kCommandSize;
    const std::size_t required = payloadSize + kChecksumSize;
    if (capacity < required) {
        return false;
    }

    output[kHeaderMajorIndex] = packet.header.major;
    output[kHeaderMinorIndex] = packet.header.minor;
    output[kHeaderFlagsIndex] = packet.header.flags;
    StoreU16(static_cast<uint16_t>(packet.commands.size()), &output[kHeaderCountIndex]);

    std::size_t offset = kHeaderSize;
    for (const auto& cmd : packet.commands) {
        if (!std::isfinite(cmd.vx) || !std::isfinite(cmd.omega)) {
            return false;
        }
        const int32_t vxFixed = QuantizeScaled(cmd.vx, kLinearVelocityScale);
        const int32_t omegaFixed = QuantizeScaled(cmd.omega, kAngularVelocityScale);
        StoreS32(vxFixed, &output[offset]);
        StoreS32(omegaFixed, &output[offset + 4]);
        StoreU16(cmd.durationMs, &output[offset + 8]);
        offset += kCommandSize;
    }

    const uint32_t crc = ComputeCrc32(output, payloadSize);
    StoreU32(crc, &output[payloadSize]);

    bytesWritten = required;
    return true;
}

bool EncodePacket(const Packet& packet, std::vector<uint8_t>& output) {
    if (packet.commands.size() > kMaxCommandsPerPacket) {
        return false;
    }
    const std::size_t required = kHeaderSize + packet.commands.size() * kCommandSize + kChecksumSize;
    output.resize(required);
    std::size_t bytesWritten = 0;
    if (!EncodePacket(packet, output.data(), output.size(), bytesWritten)) {
        return false;
    }
    output.resize(bytesWritten);
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
    packet.header.commandCount = LoadU16(&data[kHeaderCountIndex]);

    if (packet.header.major != kProtocolMajor || packet.header.minor != kProtocolMinor) {
        result.error = PacketError::UnsupportedVersion;
        result.bytesConsumed = 1;
        return result;
    }

    if (packet.header.commandCount > kMaxCommandsPerPacket) {
        result.error = PacketError::TooManyCommands;
        result.bytesConsumed = 1;
        return result;
    }

    const std::size_t payloadSize = kHeaderSize + (packet.header.commandCount * kCommandSize);
    const std::size_t expectedSize = payloadSize + kChecksumSize;
    if (length < expectedSize) {
        result.error = PacketError::Truncated;
        return result;
    }

    const uint32_t transmittedCrc = LoadU32(&data[payloadSize]);
    const uint32_t computedCrc = ComputeCrc32(data, payloadSize);
    if (transmittedCrc != computedCrc) {
        result.error = PacketError::ChecksumMismatch;
        result.bytesConsumed = expectedSize;
        return result;
    }

    std::size_t offset = kHeaderSize;
    for (std::size_t i = 0; i < packet.header.commandCount; ++i) {
        const int32_t vxFixed = LoadS32(&data[offset]);
        const int32_t omegaFixed = LoadS32(&data[offset + 4]);
        const uint16_t duration = LoadU16(&data[offset + 8]);

        const float vx = DequantizeScaled(vxFixed, kLinearVelocityScale);
        const float omega = DequantizeScaled(omegaFixed, kAngularVelocityScale);

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
