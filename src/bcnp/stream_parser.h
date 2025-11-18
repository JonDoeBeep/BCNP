#pragma once

#include "bcnp/packet.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace bcnp {

class StreamParser {
public:
    using PacketCallback = std::function<void(const Packet&)>;
    struct ErrorInfo {
        PacketError code{PacketError::None};
        std::size_t offset{0};
        uint64_t consecutiveErrors{0};
    };
    using ErrorCallback = std::function<void(const ErrorInfo&)>;

    StreamParser(PacketCallback onPacket, ErrorCallback onError = {});

    void Push(const uint8_t* data, std::size_t length);

    void Reset(bool resetErrorState = true);

    static constexpr std::size_t kMaxBufferSize = 4096;

private:
    void EmitPacket(const Packet& packet);
    void EmitError(PacketError error, std::size_t offset);

    PacketCallback m_onPacket;
    ErrorCallback m_onError;
    std::vector<uint8_t> m_buffer;
    std::size_t m_head{0};
    std::size_t m_streamOffset{0};
    uint64_t m_consecutiveErrors{0};
};

} // namespace bcnp
