#include "bcnp/command_queue.h"
#include "bcnp/controller.h"
#include "bcnp/packet.h"
#include "bcnp/stream_parser.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

namespace {

void TestEncodeDecode() {
    bcnp::Packet packet{};
    packet.header.flags = bcnp::kFlagClearQueue;
    packet.commands.push_back({0.5f, -1.0f, 1500});
    packet.commands.push_back({-0.25f, 0.25f, 500});

    std::vector<uint8_t> buffer;
    const bool encoded = bcnp::EncodePacket(packet, buffer);
    assert(encoded);

    const auto decode = bcnp::DecodePacket(buffer.data(), buffer.size());
    assert(decode.packet.has_value());
    assert(decode.packet->commands.size() == 2);
    assert(decode.packet->commands[0].vx == 0.5f);
    assert(decode.packet->commands[1].omega == 0.25f);
}

void TestCommandQueue() {
    bcnp::CommandQueue queue;
    queue.Push({1.0f, 0.0f, 100});
    queue.Push({2.0f, 0.5f, 50});

    const auto start = bcnp::CommandQueue::Clock::now();
    auto cmd = queue.CurrentCommand(start);
    assert(cmd.has_value());
    assert(cmd->vx == 1.0f);

    cmd = queue.CurrentCommand(start + std::chrono::milliseconds(110));
    assert(cmd.has_value());
    assert(cmd->vx == 2.0f);

    cmd = queue.CurrentCommand(start + std::chrono::milliseconds(200));
    assert(!cmd.has_value());
}

void TestStreamParserChunking() {
    bcnp::Packet packet{};
    packet.commands.push_back({0.1f, 0.2f, 250});
    packet.header.commandCount = 1;

    std::vector<uint8_t> encoded;
    assert(bcnp::EncodePacket(packet, encoded));

    bool packetSeen = false;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet& parsed) {
            packetSeen = true;
            assert(parsed.commands.size() == 1);
        },
        [&](bcnp::PacketError) {
            assert(false && "Unexpected parse error");
        });

    parser.Push(encoded.data(), 3);
    assert(!packetSeen);
    parser.Push(encoded.data() + 3, encoded.size() - 3);
    assert(packetSeen);
}

} // namespace

int main() {
    TestEncodeDecode();
    TestCommandQueue();
    TestStreamParserChunking();
    std::cout << "BCNP tests passed" << std::endl;
    return 0;
}
