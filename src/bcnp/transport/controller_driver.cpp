#include "bcnp/transport/controller_driver.h"

#include "bcnp/packet.h"

#include <vector>

namespace bcnp {

ControllerDriver::ControllerDriver(Controller& controller, DuplexAdapter& adapter)
    : m_controller(controller), m_adapter(adapter) {}

void ControllerDriver::PollOnce() {
    while (true) {
        const std::size_t received = m_adapter.ReceiveChunk(m_rxScratch.data(), m_rxScratch.size());
        if (received == 0) {
            break;
        }
        m_controller.PushBytes(m_rxScratch.data(), received);
    }
}

bool ControllerDriver::SendPacket(const Packet& packet) {
    std::vector<uint8_t> buffer;
    if (!EncodePacket(packet, buffer)) {
        return false;
    }
    return m_adapter.SendBytes(buffer.data(), buffer.size());
}

} // namespace bcnp
