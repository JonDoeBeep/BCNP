#pragma once

#include "bcnp/dispatcher.h"
#include "bcnp/transport/adapter.h"

#include <vector>

namespace bcnp {

/**
 * @brief Drives a PacketDispatcher from a transport adapter.
 * 
 * Connects a network transport (TCP, UDP, etc.) to a PacketDispatcher,
 * polling for incoming data and feeding it to the parser.
 * 
 * Usage:
 *   PacketDispatcher dispatcher;
 *   TcpPosixAdapter adapter(5800);
 *   DispatcherDriver driver(dispatcher, adapter);
 *   
 *   // In main loop:
 *   driver.PollOnce();
 */
class DispatcherDriver {
public:
    DispatcherDriver(PacketDispatcher& dispatcher, DuplexAdapter& adapter);

    /// Poll transport and feed data to dispatcher
    void PollOnce();

    /// Send raw bytes through the adapter
    bool SendBytes(const uint8_t* data, std::size_t length);

    /// Send a typed packet
    template<typename MsgType>
    bool SendPacket(const TypedPacket<MsgType>& packet) {
        std::vector<uint8_t> buffer;
        if (!EncodeTypedPacket(packet, buffer)) {
            return false;
        }
        return m_adapter.SendBytes(buffer.data(), buffer.size());
    }

private:
    PacketDispatcher& m_dispatcher;
    DuplexAdapter& m_adapter;
    std::vector<uint8_t> m_rxScratch;
};

/// Backward-compatible alias (prefer DispatcherDriver for new code).
using ControllerDriver = DispatcherDriver;

} // namespace bcnp
