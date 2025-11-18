#pragma once

#include "bcnp/controller.h"
#include "bcnp/transport/adapter.h"

#include <array>

namespace bcnp {

class ControllerDriver {
public:
    ControllerDriver(Controller& controller, DuplexAdapter& adapter);

    void PollOnce();

    bool SendPacket(const Packet& packet);

private:
    Controller& m_controller;
    DuplexAdapter& m_adapter;
    std::array<uint8_t, kMaxPacketSize> m_txBuffer{};
    // Use a full-packet scratch buffer so UDP datagrams are never truncated mid-packet.
    std::array<uint8_t, kMaxPacketSize> m_rxScratch{};
};

} // namespace bcnp
