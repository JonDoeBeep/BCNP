#include "bcnp/command_queue.h"
#include "bcnp/controller.h"
#include "bcnp/packet.h"
#include "bcnp/stream_parser.h"

#include <algorithm>
#include <array>
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
    queue.NotifyPacketReceived(start);
    queue.Update(start);
    auto cmd = queue.ActiveCommand();
    assert(cmd.has_value());
    assert(cmd->vx == 1.0f);

    queue.Update(start + std::chrono::milliseconds(110));
    cmd = queue.ActiveCommand();
    assert(cmd.has_value());
    assert(cmd->vx == 2.0f);

    queue.Update(start + std::chrono::milliseconds(200));
    cmd = queue.ActiveCommand();
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
        [&](const bcnp::StreamParser::ErrorInfo&) {
            assert(false && "Unexpected parse error");
        });

    parser.Push(encoded.data(), 3);
    assert(!packetSeen);
    parser.Push(encoded.data() + 3, encoded.size() - 3);
    assert(packetSeen);
}

void TestStreamParserTruncatedWaits() {
    bcnp::Packet packet{};
    packet.header.commandCount = 1;
    packet.commands.push_back({0.5f, 0.1f, 100});

    std::vector<uint8_t> encoded;
    assert(bcnp::EncodePacket(packet, encoded));

    bool packetSeen = false;
    std::size_t errors = 0;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet&) { packetSeen = true; },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errors; });

    parser.Push(encoded.data(), encoded.size() - 1);
    assert(!packetSeen);
    assert(errors == 0);

    parser.Push(encoded.data() + encoded.size() - 1, 1);
    assert(packetSeen);
    assert(errors == 0);
}

void TestStreamParserErrors() {
    std::vector<bcnp::StreamParser::ErrorInfo> errors;
    bcnp::StreamParser parser(
        [](const bcnp::Packet&) {},
        [&](const bcnp::StreamParser::ErrorInfo& info) { errors.push_back(info); });

    std::array<uint8_t, bcnp::kHeaderSize> badHeader{};
    badHeader[bcnp::kHeaderMajorIndex] = bcnp::kProtocolMajor;
    badHeader[bcnp::kHeaderMinorIndex] = bcnp::kProtocolMinor;
    badHeader[bcnp::kHeaderCountIndex] = static_cast<uint8_t>(bcnp::kMaxCommandsPerPacket + 1);

    parser.Push(badHeader.data(), badHeader.size());
    parser.Push(badHeader.data(), badHeader.size());

    assert(errors.size() == 2);
    assert(errors[0].code == bcnp::PacketError::TooManyCommands);
    assert(errors[0].offset == 0);
    assert(errors[0].consecutiveErrors == 1);
    assert(errors[1].consecutiveErrors == 2);
    assert(errors[1].offset == bcnp::kHeaderSize);

    parser.Reset();
    parser.Push(badHeader.data(), badHeader.size());
    assert(errors.size() == 3);
    assert(errors.back().consecutiveErrors == 1);
}

void TestControllerClamping() {
    bcnp::ControllerConfig config{};
    config.limits.vxMin = -0.25f;
    config.limits.vxMax = 0.25f;
    config.limits.omegaMin = -0.5f;
    config.limits.omegaMax = 0.5f;
    config.limits.durationMin = 50;
    config.limits.durationMax = 5000;

    bcnp::Controller controller(config);

    bcnp::Packet packet{};
    packet.header.commandCount = 1;
    packet.commands.push_back({1.0f, -2.0f, 6000});
    controller.HandlePacket(packet);

    auto cmd = controller.CurrentCommand(bcnp::CommandQueue::Clock::now());
    assert(cmd.has_value());
    assert(cmd->vx == config.limits.vxMax);
    assert(cmd->omega == config.limits.omegaMin);
    assert(cmd->durationMs == config.limits.durationMax);
}

void TestQueueDisconnectStopsCommands() {
    bcnp::QueueConfig config{};
    config.connectionTimeout = std::chrono::milliseconds(50);
    bcnp::CommandQueue queue(config);

    const auto now = bcnp::CommandQueue::Clock::now();
    queue.NotifyPacketReceived(now);
    queue.Push({0.0f, 0.0f, 60000});
    queue.Update(now);
    assert(queue.ActiveCommand().has_value());

    const auto later = now + config.connectionTimeout + std::chrono::milliseconds(1);
    queue.Update(later);
    assert(!queue.ActiveCommand().has_value());
    assert(queue.Size() == 0);
}

} // namespace

int main() {
    TestEncodeDecode();
    TestCommandQueue();
    TestStreamParserChunking();
    TestStreamParserTruncatedWaits();
    TestStreamParserErrors();
    TestControllerClamping();
    TestQueueDisconnectStopsCommands();
    std::cout << "BCNP tests passed" << std::endl;
    return 0;
}
