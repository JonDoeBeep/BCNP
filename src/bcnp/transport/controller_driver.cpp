#include "bcnp/transport/controller_driver.h"

#include "bcnp/packet.h"

namespace bcnp {

ControllerDriver::ControllerDriver(Controller& controller, DuplexAdapter& adapter)
    : m_controller(controller), m_adapter(adapter) {}

void ControllerDriver::PollOnce() {
    // Limit iterations to prevent starvation if data arrives faster than we can process
    constexpr std::size_t kMaxChunksPerPoll = 10;
    for (std::size_t i = 0; i < kMaxChunksPerPoll; ++i) {
        const std::size_t received = m_adapter.ReceiveChunk(m_rxScratch.data(), m_rxScratch.size());
        if (received == 0) {
            break;
        }
        m_controller.PushBytes(m_rxScratch.data(), received);
    }
}

bool ControllerDriver::SendPacket(const Packet& packet) {
    std::size_t length = 0;
    if (!EncodePacket(packet, m_txBuffer.data(), m_txBuffer.size(), length)) {
        return false;
    }
    return m_adapter.SendBytes(m_txBuffer.data(), length);
}

} // namespace bcnp
