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
    using ErrorCallback = std::function<void(PacketError)>;

    StreamParser(PacketCallback onPacket, ErrorCallback onError = {});

    void Push(const uint8_t* data, std::size_t length);

    void Reset();

private:
    void EmitPacket(const Packet& packet);
    void EmitError(PacketError error);

    PacketCallback m_onPacket;
    ErrorCallback m_onError;
    std::vector<uint8_t> m_buffer;
};

} // namespace bcnp
