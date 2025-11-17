#include "bcnp/controller.h"
#include "bcnp/spi_adapter.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    bcnp::Controller controller;

    // Simulated SPI buffers: feed encoded packet in 5-byte chunks.
    bcnp::Packet packet{};
    packet.commands.push_back({0.2f, 0.0f, 250});
    packet.commands.push_back({0.0f, 0.3f, 400});

    std::vector<uint8_t> encoded;
    if (!bcnp::EncodePacket(packet, encoded)) {
        std::cerr << "Failed to encode sample packet\n";
        return 1;
    }

    std::size_t cursor = 0;
    auto receive = [&](uint8_t* dst, std::size_t maxLen) -> std::size_t {
        if (cursor >= encoded.size()) {
            return 0;
        }
        const std::size_t chunk = std::min<std::size_t>(5, maxLen);
        const std::size_t toCopy = std::min<std::size_t>(chunk, encoded.size() - cursor);
        std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(cursor), toCopy, dst);
        cursor += toCopy;
        return toCopy;
    };

    auto send = [](const uint8_t* data, std::size_t length) {
        std::cout << "Sending " << length << " bytes over SPI" << std::endl;
        (void)data;
        return true;
    };

    bcnp::SpiStreamAdapter adapter(receive, send, controller.Parser());
    adapter.Poll();

    auto now = bcnp::CommandQueue::Clock::now();
    for (int i = 0; i < 4; ++i) {
        auto cmd = controller.CurrentCommand(now);
        if (cmd) {
            std::cout << "Command vx=" << cmd->vx << " omega=" << cmd->omega
                      << " durationMs=" << cmd->durationMs << std::endl;
        } else {
            std::cout << "Queue empty" << std::endl;
        }
        now += std::chrono::milliseconds(300);
    }

    return 0;
}
