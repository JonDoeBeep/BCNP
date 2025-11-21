#pragma once

#include "bcnp/controller.h"
#include "bcnp/transport/adapter.h"

#include <vector>

namespace bcnp {

class ControllerDriver {
public:
    ControllerDriver(Controller& controller, DuplexAdapter& adapter);

    void PollOnce();

    bool SendPacket(const Packet& packet);

private:
    Controller& m_controller;
    DuplexAdapter& m_adapter;
    std::vector<uint8_t> m_txBuffer;
    std::vector<uint8_t> m_rxScratch;
};

} // namespace bcnp
