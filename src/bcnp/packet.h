#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bcnp {

constexpr uint8_t kProtocolMajor = 1;
constexpr uint8_t kProtocolMinor = 1;
constexpr std::size_t kHeaderSize = 4;
constexpr std::size_t kCommandSize = 10;
constexpr std::size_t kMaxCommandsPerPacket = 100;
constexpr std::size_t kMaxQueueSize = 200;
constexpr std::size_t kMaxPacketSize = kHeaderSize + (kCommandSize * kMaxCommandsPerPacket);
constexpr uint8_t kFlagClearQueue = 0x01;
constexpr std::size_t kHeaderMajorIndex = 0;
constexpr std::size_t kHeaderMinorIndex = 1;
constexpr std::size_t kHeaderFlagsIndex = 2;
constexpr std::size_t kHeaderCountIndex = 3;

struct PacketHeader {
    uint8_t major{kProtocolMajor};
    uint8_t minor{kProtocolMinor};
    uint8_t flags{0};
    uint8_t commandCount{0};
};

struct Command {
    float vx{0.0f};
    float omega{0.0f};
    uint16_t durationMs{0};
};

struct Packet {
    PacketHeader header{};
    std::vector<Command> commands;
};

enum class PacketError {
    None,
    TooSmall,
    UnsupportedVersion,
    TooManyCommands,
    Truncated,
    InvalidFloat
};

struct DecodeResult {
    std::optional<Packet> packet;
    PacketError error{PacketError::None};
    std::size_t bytesConsumed{0};
};

bool EncodePacket(const Packet& packet, std::vector<uint8_t>& output);

bool EncodePacket(const Packet& packet, uint8_t* output, std::size_t capacity, std::size_t& bytesWritten);

DecodeResult DecodePacket(const uint8_t* data, std::size_t length);

} // namespace bcnp
