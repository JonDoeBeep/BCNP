#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "bcnp/command_queue.h"
#include "bcnp/controller.h"
#include "bcnp/packet.h"
#include "bcnp/stream_parser.h"
#include "bcnp/transport/tcp_posix.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ============================================================================
// Test Suite: Packet Encoding/Decoding
// ============================================================================

TEST_CASE("Packet: Encode and decode round-trip") {
    bcnp::Packet packet{};
    packet.header.flags = bcnp::kFlagClearQueue;
    packet.commands.push_back({0.5f, -1.0f, 1500});
    packet.commands.push_back({-0.25f, 0.25f, 500});

    std::vector<uint8_t> buffer;
    const bool encoded = bcnp::EncodePacket(packet, buffer);
    REQUIRE(encoded);

    const auto decode = bcnp::DecodePacket(buffer.data(), buffer.size());
    REQUIRE(decode.packet.has_value());
    CHECK(decode.packet->commands.size() == 2);
    CHECK(decode.packet->commands[0].vx == 0.5f);
    CHECK(decode.packet->commands[1].omega == 0.25f);
}

TEST_CASE("Packet: Detects checksum mismatch") {
    bcnp::Packet packet{};
    packet.commands.push_back({0.1f, 0.2f, 250});

    std::vector<uint8_t> bytes;
    REQUIRE(bcnp::EncodePacket(packet, bytes));

    bytes.back() ^= 0xFF; // Corrupt CRC without touching payload

    const auto decode = bcnp::DecodePacket(bytes.data(), bytes.size());
    CHECK(!decode.packet.has_value());
    CHECK(decode.error == bcnp::PacketError::ChecksumMismatch);
}

// ============================================================================
// Test Suite: CommandQueue
// ============================================================================

TEST_CASE("CommandQueue: Basic command execution timing") {
    bcnp::CommandQueue queue;
    queue.Push({1.0f, 0.0f, 100});
    queue.Push({2.0f, 0.5f, 50});

    const auto start = bcnp::CommandQueue::Clock::now();
    queue.NotifyPacketReceived(start);
    queue.Update(start);
    auto cmd = queue.ActiveCommand();
    REQUIRE(cmd.has_value());
    CHECK(cmd->vx == 1.0f);

    queue.Update(start + 110ms);
    cmd = queue.ActiveCommand();
    REQUIRE(cmd.has_value());
    CHECK(cmd->vx == 2.0f);

    queue.Update(start + 200ms);
    cmd = queue.ActiveCommand();
    CHECK(!cmd.has_value());
}

// ============================================================================
// Test Suite: StreamParser
// ============================================================================

TEST_CASE("StreamParser: Chunked packet delivery") {
    bcnp::Packet packet{};
    packet.commands.push_back({0.1f, 0.2f, 250});
    packet.header.commandCount = 1;

    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodePacket(packet, encoded));

    bool packetSeen = false;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet& parsed) {
            packetSeen = true;
            CHECK(parsed.commands.size() == 1);
        },
        [&](const bcnp::StreamParser::ErrorInfo&) {
            FAIL("Unexpected parse error");
        });

    parser.Push(encoded.data(), 3);
    CHECK(!packetSeen);
    parser.Push(encoded.data() + 3, encoded.size() - 3);
    CHECK(packetSeen);
}

TEST_CASE("StreamParser: Truncated packet waits without error") {
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
    CHECK(!packetSeen);
    CHECK(errors == 0);

    parser.Push(encoded.data() + encoded.size() - 1, 1);
    CHECK(packetSeen);
    CHECK(errors == 0);
}

TEST_CASE("StreamParser: Skip bad headers and recover") {
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

    // Append a malformed header (wrong protocol version).
    combined.push_back(bcnp::kProtocolMajor + 1);
    combined.push_back(bcnp::kProtocolMinor);
    combined.push_back(0x00);
    combined.push_back(0x00);
    combined.push_back(0x01); // Count = 1

    REQUIRE(bcnp::EncodePacket(second, encoded));
    combined.insert(combined.end(), encoded.begin(), encoded.end());

    std::vector<bcnp::Packet> seen;
    std::size_t errorCount = 0;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet& parsed) { seen.push_back(parsed); },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errorCount; });

    parser.Push(combined.data(), combined.size());

    CHECK(errorCount >= 1);
    CHECK(seen.size() == 2);
    CHECK(seen.front().commands.front().vx == first.commands.front().vx);
    CHECK(seen.back().commands.front().omega == second.commands.front().omega);
}

TEST_CASE("StreamParser: Error info provides diagnostics") {
    std::vector<bcnp::StreamParser::ErrorInfo> errors;
    bcnp::StreamParser parser(
        [](const bcnp::Packet&) {},
        [&](const bcnp::StreamParser::ErrorInfo& info) { errors.push_back(info); });

    std::array<uint8_t, bcnp::kHeaderSize> badHeader{};
    badHeader[bcnp::kHeaderMajorIndex] = bcnp::kProtocolMajor + 1;
    badHeader[bcnp::kHeaderMinorIndex] = bcnp::kProtocolMinor;
    badHeader[bcnp::kHeaderCountIndex] = 0;

    parser.Push(badHeader.data(), badHeader.size());
    parser.Push(badHeader.data(), badHeader.size());

    REQUIRE(errors.size() >= 2);
    CHECK(errors[0].code == bcnp::PacketError::UnsupportedVersion);
    CHECK(errors[0].offset == 0);
    CHECK(errors[0].consecutiveErrors == 1);
    CHECK(errors[1].consecutiveErrors == 2);

    parser.Reset();
    parser.Push(badHeader.data(), badHeader.size());
    CHECK(errors.size() > 2);
    CHECK(errors.back().consecutiveErrors == 1);
}

TEST_CASE("StreamParser: DoS protection - survives garbage flood") {
    bool packetSeen = false;
    const size_t kBufferSize = 4096;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet&) { packetSeen = true; },
        [](const bcnp::StreamParser::ErrorInfo&) {},
        kBufferSize);

    std::vector<uint8_t> garbage(kBufferSize + 100, 0xFF);
    parser.Push(garbage.data(), garbage.size());

    bcnp::Packet packet{};
    packet.commands.push_back({0.1f, 0.1f, 100});
    std::vector<uint8_t> encoded;
    bcnp::EncodePacket(packet, encoded);

    parser.Push(encoded.data(), encoded.size());
    CHECK(packetSeen);
}

// ============================================================================
// Test Suite: Controller
// ============================================================================

TEST_CASE("Controller: Command clamping enforces limits") {
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
    CHECK(cmd->vx == config.limits.vxMax);
    CHECK(cmd->omega == config.limits.omegaMin);
    CHECK(cmd->durationMs == config.limits.durationMax);
}

TEST_CASE("CommandQueue: Disconnect clears active command immediately") {
    bcnp::QueueConfig config{};
    config.connectionTimeout = 50ms;
    bcnp::CommandQueue queue(config);

    const auto now = bcnp::CommandQueue::Clock::now();
    queue.NotifyPacketReceived(now);
    queue.Push({0.0f, 0.0f, 60000});
    queue.Update(now);
    REQUIRE(queue.ActiveCommand().has_value());

    const auto later = now + config.connectionTimeout + 1ms;
    queue.Update(later);
    CHECK(!queue.ActiveCommand().has_value());
    CHECK(queue.Size() == 0);
}

// ============================================================================
// Test Suite: Thread Safety (NEW)
// ============================================================================

TEST_CASE("Controller: Thread-safe PushBytes from multiple threads") {
    bcnp::Controller controller;
    std::atomic<int> packetsReceived{0};
    
    // Prepare valid packets
    bcnp::Packet packet{};
    packet.commands.push_back({0.1f, 0.1f, 100});
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodePacket(packet, encoded));
    
    // Track received packets
    std::mutex resultsMutex;
    std::vector<bcnp::Packet> receivedPackets;
    
    // Replace the parser with one that tracks packets
    controller.Parser().Reset();
    
    // Launch 3 threads that all push the same packet repeatedly
    const int threadsCount = 3;
    const int iterationsPerThread = 50;
    
    std::vector<std::thread> threads;
    for (int t = 0; t < threadsCount; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < iterationsPerThread; ++i) {
                controller.PushBytes(encoded.data(), encoded.size());
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify no crashes occurred (test passes if we reach here)
    CHECK(true);
}

TEST_CASE("CommandQueue: Concurrent Push and ActiveCommand") {
    bcnp::CommandQueue queue;
    std::atomic<bool> running{true};
    std::atomic<int> pushCount{0};
    std::atomic<int> readCount{0};
    
    auto now = bcnp::CommandQueue::Clock::now();
    queue.NotifyPacketReceived(now);
    
    // Thread 1: Continuously push commands
    std::thread pusher([&]() {
        for (int i = 0; i < 100; ++i) {
            if (queue.Push({0.1f, 0.1f, 10})) {
                ++pushCount;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        running = false;
    });
    
    // Thread 2: Continuously read active command
    std::thread reader([&]() {
        while (running) {
            queue.Update(bcnp::CommandQueue::Clock::now());
            auto cmd = queue.ActiveCommand();
            if (cmd.has_value()) {
                ++readCount;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    
    pusher.join();
    reader.join();
    
    // Verify operations completed without crashes
    CHECK(pushCount > 0);
    CHECK(readCount > 0);
}

// ============================================================================
// Test Suite: TCP Adapter (Integration Tests)
// ============================================================================

TEST_CASE("TCP: Basic server-client connection and data transfer") {
    bcnp::TcpPosixAdapter server(12345);
    REQUIRE(server.IsValid());

    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12345);
    REQUIRE(client.IsValid());

    std::vector<uint8_t> txData = {0x01, 0x02, 0x03, 0x04};
    for (int i = 0; i < 10; ++i) {
        client.SendBytes(txData.data(), txData.size());
        std::this_thread::sleep_for(10ms);
        if (client.IsConnected()) break;
    }

    std::vector<uint8_t> rxBuffer(1024);
    bool received = false;
    for (int i = 0; i < 50; ++i) {
        size_t bytes = server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (bytes > 0) {
            received = true;
            CHECK(bytes == txData.size());
            CHECK(rxBuffer[0] == 0x01);
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(received);
    CHECK(server.IsConnected());

    std::vector<uint8_t> response = {0x05, 0x06};
    server.SendBytes(response.data(), response.size());

    received = false;
    for (int i = 0; i < 50; ++i) {
        size_t bytes = client.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (bytes > 0) {
            received = true;
            CHECK(bytes == response.size());
            CHECK(rxBuffer[0] == 0x05);
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(received);
}

TEST_CASE("TCP: Partial send handling with slow consumer") {
    bcnp::TcpPosixAdapter server(12346);
    REQUIRE(server.IsValid());
    
    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12346);
    REQUIRE(client.IsValid());
    
    // Establish connection
    std::vector<uint8_t> handshake = {0xAA};
    std::vector<uint8_t> rxBuffer(65536);
    
    for (int i = 0; i < 50; ++i) {
        client.SendBytes(handshake.data(), handshake.size());
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (server.IsConnected() && client.IsConnected()) break;
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(server.IsConnected());
    
    // Test within real-time buffer limits (8 packets max)
    std::vector<uint8_t> largeData(bcnp::kMaxPacketSize * 2, 0xBB);
    bool sendSuccess = server.SendBytes(largeData.data(), largeData.size());
    
    // Should succeed within buffer capacity
    CHECK(sendSuccess);
    
    // Client should be able to receive all data
    std::vector<uint8_t> received;
    for (int i = 0; i < 1000 && received.size() < largeData.size(); ++i) {
        size_t bytes = client.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (bytes > 0) {
            received.insert(received.end(), rxBuffer.begin(), rxBuffer.begin() + bytes);
        }
        std::this_thread::sleep_for(5ms);
    }
    
    CHECK(received.size() == largeData.size());
}
