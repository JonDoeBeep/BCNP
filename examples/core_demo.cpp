#include "bcnp/controller.h"

#include "../SPI-EAK/src/link_layer.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    bcnp::Controller controller;

    // Compose a BCNP packet as if it originated from a remote planner.
    bcnp::Packet outgoing{};
    outgoing.commands.push_back({0.25f, 0.0f, 250});
    outgoing.commands.push_back({-0.10f, 0.35f, 400});

    std::vector<uint8_t> wirePayload;
    if (!bcnp::EncodePacket(outgoing, wirePayload)) {
        std::cerr << "Failed to encode BCNP payload\n";
        return 1;
    }

    // Wrap the BCNP payload with SPI-EAK's framed transport (start/stop/escape + CRC16).
    spi_eak::FrameCodec::Parameters frameParams{};
    auto framed = spi_eak::FrameCodec::encode(wirePayload, frameParams);
    if (!framed.ok) {
        std::cerr << "SPI-EAK framing error\n";
        return 1;
    }

    // In a real system you'd call spi_eak::SPI::transfer(). Here we simply loop the
    // frame back as if the robot immediately echoed what we sent.
    const std::vector<uint8_t> rxFrame = framed.frame;

    spi_eak::FrameDecoder::Options decoderOpts;
    decoderOpts.params = frameParams;
    decoderOpts.max_frame_bytes = 2048;
    spi_eak::FrameDecoder decoder(decoderOpts);

    std::vector<uint8_t> decoded;
    for (uint8_t byte : rxFrame) {
        const auto result = decoder.push(byte, decoded);
        if (result.frame_dropped) {
            std::cerr << "Dropped frame before BCNP could read it\n";
            decoded.clear();
            continue;
        }
        if (result.frame_ready) {
            // Feed the recovered BCNP payload directly into the controller's stream parser.
            controller.PushBytes(decoded.data(), decoded.size());
            decoded.clear();
        }
    }

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

    // When sending telemetry back to the field, encode the controller's packet once and
    // wrap it with FrameCodec before hitting spi_eak::SPI::transfer().
    std::vector<uint8_t> txBuf;
    if (bcnp::EncodePacket(outgoing, txBuf)) {
        auto txFrame = spi_eak::FrameCodec::encode(txBuf, frameParams);
        std::cout << "Prepared " << txFrame.frame.size()
                  << " byte SPI-EAK frame for transmission" << std::endl;
    }

    return 0;
}
