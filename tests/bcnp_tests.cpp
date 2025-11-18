#include "bcnp/command_queue.h"
#include "bcnp/controller.h"
#include "bcnp/packet.h"
#include "bcnp/stream_parser.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace testutil {
[[noreturn]] void Fail(const char* expr, const char* file, int line, const char* message = nullptr) {
    std::cerr << "Test failure: " << expr << " at " << file << ':' << line;
    if (message) {
        std::cerr << " - " << message;
    }
    std::cerr << std::endl;
    std::exit(EXIT_FAILURE);
}
} // namespace testutil

#define REQUIRE(expr)                                                                    \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            testutil::Fail(#expr, __FILE__, __LINE__);                                   \
        }                                                                                \
    } while (false)

#define REQUIRE_MSG(expr, msg)                                                           \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            testutil::Fail(#expr, __FILE__, __LINE__, msg);                              \
        }                                                                                \
    } while (false)

namespace {

void TestEncodeDecode() {
    bcnp::Packet packet{};
    packet.header.flags = bcnp::kFlagClearQueue;
    packet.commands.push_back({0.5f, -1.0f, 1500});
    packet.commands.push_back({-0.25f, 0.25f, 500});

    std::vector<uint8_t> buffer;
    const bool encoded = bcnp::EncodePacket(packet, buffer);
    REQUIRE(encoded);

    const auto decode = bcnp::DecodePacket(buffer.data(), buffer.size());
    REQUIRE(decode.packet.has_value());
    REQUIRE(decode.packet->commands.size() == 2);
    REQUIRE(decode.packet->commands[0].vx == 0.5f);
    REQUIRE(decode.packet->commands[1].omega == 0.25f);
}

void TestCommandQueue() {
    bcnp::CommandQueue queue;
    queue.Push({1.0f, 0.0f, 100});
    queue.Push({2.0f, 0.5f, 50});

    const auto start = bcnp::CommandQueue::Clock::now();
    queue.NotifyPacketReceived(start);
    queue.Update(start);
    auto cmd = queue.ActiveCommand();
    REQUIRE(cmd.has_value());
    REQUIRE(cmd->vx == 1.0f);

    queue.Update(start + std::chrono::milliseconds(110));
    cmd = queue.ActiveCommand();
    REQUIRE(cmd.has_value());
    REQUIRE(cmd->vx == 2.0f);

    queue.Update(start + std::chrono::milliseconds(200));
    cmd = queue.ActiveCommand();
    REQUIRE(!cmd.has_value());
}

void TestStreamParserChunking() {
    bcnp::Packet packet{};
    packet.commands.push_back({0.1f, 0.2f, 250});
    packet.header.commandCount = 1;

    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodePacket(packet, encoded));

    bool packetSeen = false;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet& parsed) {
            packetSeen = true;
            REQUIRE(parsed.commands.size() == 1);
        },
        [&](const bcnp::StreamParser::ErrorInfo&) {
            REQUIRE_MSG(false, "Unexpected parse error");
        });

    parser.Push(encoded.data(), 3);
    REQUIRE(!packetSeen);
    parser.Push(encoded.data() + 3, encoded.size() - 3);
    REQUIRE(packetSeen);
}

void TestStreamParserTruncatedWaits() {
    bcnp::Packet packet{};
    packet.header.commandCount = 1;
    packet.commands.push_back({0.5f, 0.1f, 100});

    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodePacket(packet, encoded));

    bool packetSeen = false;
    std::size_t errors = 0;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet&) { packetSeen = true; },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errors; });

    parser.Push(encoded.data(), encoded.size() - 1);
    REQUIRE(!packetSeen);
    REQUIRE(errors == 0);

    parser.Push(encoded.data() + encoded.size() - 1, 1);
    REQUIRE(packetSeen);
    REQUIRE(errors == 0);
}

void TestStreamParserSkipsBadHeader() {
    bcnp::Packet first{};
    first.header.commandCount = 1;
    first.commands.push_back({0.2f, 0.0f, 150});

    bcnp::Packet second{};
    second.header.commandCount = 1;
    second.commands.push_back({-0.1f, 0.5f, 200});

    std::vector<uint8_t> combined;
    std::vector<uint8_t> encoded;

    REQUIRE(bcnp::EncodePacket(first, encoded));
    combined.insert(combined.end(), encoded.begin(), encoded.end());

    // Append a malformed header (commandCount > kMaxCommandsPerPacket).
    combined.push_back(bcnp::kProtocolMajor);
    combined.push_back(bcnp::kProtocolMinor);
    combined.push_back(0x00);
    combined.push_back(static_cast<uint8_t>(bcnp::kMaxCommandsPerPacket + 1));
    combined.push_back(0xAA);
    combined.push_back(0x55);

    REQUIRE(bcnp::EncodePacket(second, encoded));
    combined.insert(combined.end(), encoded.begin(), encoded.end());

    std::vector<bcnp::Packet> seen;
    std::size_t errorCount = 0;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet& parsed) { seen.push_back(parsed); },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errorCount; });

    parser.Push(combined.data(), combined.size());

    REQUIRE(errorCount >= 1);
    REQUIRE(seen.size() == 2);
    REQUIRE(seen.front().commands.front().vx == first.commands.front().vx);
    REQUIRE(seen.back().commands.front().omega == second.commands.front().omega);
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

    REQUIRE(errors.size() == 2);
    REQUIRE(errors[0].code == bcnp::PacketError::TooManyCommands);
    REQUIRE(errors[0].offset == 0);
    REQUIRE(errors[0].consecutiveErrors == 1);
    REQUIRE(errors[1].consecutiveErrors == 2);
    REQUIRE(errors[1].offset > errors[0].offset);

    parser.Reset();
    parser.Push(badHeader.data(), badHeader.size());
    REQUIRE(errors.size() == 3);
    REQUIRE(errors.back().consecutiveErrors == 1);
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
    REQUIRE(cmd.has_value());
    REQUIRE(cmd->vx == config.limits.vxMax);
    REQUIRE(cmd->omega == config.limits.omegaMin);
    REQUIRE(cmd->durationMs == config.limits.durationMax);
}

void TestQueueDisconnectStopsCommands() {
    bcnp::QueueConfig config{};
    config.connectionTimeout = std::chrono::milliseconds(50);
    bcnp::CommandQueue queue(config);

    const auto now = bcnp::CommandQueue::Clock::now();
    queue.NotifyPacketReceived(now);
    queue.Push({0.0f, 0.0f, 60000});
    queue.Update(now);
    REQUIRE(queue.ActiveCommand().has_value());

    const auto later = now + config.connectionTimeout + std::chrono::milliseconds(1);
    queue.Update(later);
    REQUIRE(!queue.ActiveCommand().has_value());
    REQUIRE(queue.Size() == 0);
}

} // namespace

int main() {
    TestEncodeDecode();
    TestCommandQueue();
    TestStreamParserChunking();
    TestStreamParserTruncatedWaits();
    TestStreamParserErrors();
    TestStreamParserSkipsBadHeader();
    TestControllerClamping();
    TestQueueDisconnectStopsCommands();
    std::cout << "BCNP tests passed" << std::endl;
    return 0;
}
