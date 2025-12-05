#include "bcnp/transport/controller_driver.h"

#include "bcnp/packet.h"

namespace bcnp {

DispatcherDriver::DispatcherDriver(PacketDispatcher& dispatcher, DuplexAdapter& adapter)
    : m_dispatcher(dispatcher), m_adapter(adapter) {
        // Allocate buffer on heap to prevent stack overflow
        m_rxScratch.resize(8192);
    }

void DispatcherDriver::PollOnce() {
    // Limit iterations to prevent starvation if data arrives faster than we can process
    constexpr std::size_t kMaxChunksPerPoll = 10;
    for (std::size_t i = 0; i < kMaxChunksPerPoll; ++i) {
        const std::size_t received = m_adapter.ReceiveChunk(m_rxScratch.data(), m_rxScratch.size());
        if (received == 0) {
            break;
        }
        m_dispatcher.PushBytes(m_rxScratch.data(), received);
    }
}

bool DispatcherDriver::SendBytes(const uint8_t* data, std::size_t length) {
    return m_adapter.SendBytes(data, length);
}

} // namespace bcnp
