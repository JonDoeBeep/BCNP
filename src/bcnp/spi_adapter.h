#pragma once

#include "bcnp/packet.h"
#include "bcnp/stream_parser.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace bcnp {

class SpiStreamAdapter {
public:
    using ReceiveChunkFn = std::function<std::size_t(uint8_t* dst, std::size_t maxLen)>;
    using SendBytesFn = std::function<bool(const uint8_t* data, std::size_t length)>;

    SpiStreamAdapter(ReceiveChunkFn receive, SendBytesFn send, StreamParser& parser);

    void Poll();

    void PushChunk(const uint8_t* data, std::size_t length);

    bool SendPacket(const Packet& packet);

private:
    ReceiveChunkFn m_receive;
    SendBytesFn m_send;
    StreamParser& m_parser;
    std::vector<uint8_t> m_encodeScratch;
};

} // namespace bcnp
