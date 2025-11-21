#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "bcnp/command_queue.h"
#include "bcnp/controller.h"
#include "bcnp/packet.h"
#include "bcnp/stream_parser.h"
#include "bcnp/transport/tcp_posix.h"
#include "bcnp/transport/udp_posix.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
std::array<uint8_t, 8> MakePairingFrame(uint32_t token) {
    std::array<uint8_t, 8> frame{};
    frame[0] = 'B';
    frame[1] = 'C';
    frame[2] = 'N';
    frame[3] = 'P';
    frame[4] = static_cast<uint8_t>((token >> 24) & 0xFF);
    frame[5] = static_cast<uint8_t>((token >> 16) & 0xFF);
    frame[6] = static_cast<uint8_t>((token >> 8) & 0xFF);
    frame[7] = static_cast<uint8_t>(token & 0xFF);
    return frame;
}
}

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
    CHECK(decode.packet->commands[0].omega == -1.0f);
    CHECK(decode.packet->commands[0].durationMs == 1500);
    CHECK(decode.packet->commands[1].vx == -0.25f);
    CHECK(decode.packet->commands[1].omega == 0.25f);
}

TEST_CASE("Packet: CRC detects payload corruption") {
    bcnp::Packet packet{};
    packet.commands.push_back({0.25f, -0.5f, 100});

    std::vector<uint8_t> bytes;
    REQUIRE(bcnp::EncodePacket(packet, bytes));

    // Flip one payload byte without updating the checksum
    REQUIRE(bytes.size() > bcnp::kHeaderSize);
    bytes[bcnp::kHeaderSize] ^= 0xFF;

    auto result = bcnp::DecodePacket(bytes.data(), bytes.size());
    CHECK(!result.packet.has_value());
    CHECK(result.error == bcnp::PacketError::ChecksumMismatch);
}

TEST_CASE("Packet: Reject too many commands") {
    std::vector<uint8_t> buffer = {
        bcnp::kProtocolMajor, bcnp::kProtocolMinor, 0x00,
        static_cast<uint8_t>(bcnp::kMaxCommandsPerPacket + 1)
    };

    auto result = bcnp::DecodePacket(buffer.data(), buffer.size());
    CHECK(!result.packet.has_value());
    CHECK(result.error == bcnp::PacketError::TooManyCommands);
}

// ============================================================================
// Test Suite: CommandQueue (Logic Tests - No Sleep)
// ============================================================================

TEST_CASE("CommandQueue: Basic command execution timing") {
    bcnp::CommandQueue queue;
    
    // Use deterministic time
    auto now = bcnp::CommandQueue::Clock::time_point{} + 1000ms;
    
    queue.Push({1.0f, 0.0f, 100});
    queue.Push({2.0f, 0.5f, 50});
    
    queue.NotifyPacketReceived(now);
    queue.Update(now);
    
    auto cmd = queue.ActiveCommand();
    REQUIRE(cmd.has_value());
    CHECK(cmd->vx == 1.0f);
    
    // Update at t=50ms (mid-command1)
    now += 50ms;
    queue.Update(now);
    CHECK(queue.ActiveCommand()->vx == 1.0f);
    
    // Update at t=100ms (command1 should end, command2 starts)
    now += 50ms;
    queue.Update(now);
    cmd = queue.ActiveCommand();
    REQUIRE(cmd.has_value());
    CHECK(cmd->vx == 2.0f);
    
    // Update at t=150ms (mid-command2, which ends at t=150 since it started at t=100)
    now += 50ms;
    queue.Update(now);
    cmd = queue.ActiveCommand();
    CHECK(!cmd.has_value());
}

TEST_CASE("CommandQueue: Disconnect clears active command immediately") {
    bcnp::QueueConfig config{};
    config.connectionTimeout = 50ms;
    bcnp::CommandQueue queue(config);
    
    auto now = bcnp::CommandQueue::Clock::time_point{} + 1000ms;
    queue.NotifyPacketReceived(now);
    queue.Push({0.0f, 0.0f, 60000}); // 60 second command
    queue.Update(now);
    
    REQUIRE(queue.ActiveCommand().has_value());
    
    // Exceed timeout - command should be cleared for safety
    queue.Update(now + config.connectionTimeout + 1ms);
    CHECK(!queue.ActiveCommand().has_value());
    CHECK(queue.Size() == 0);
}

TEST_CASE("CommandQueue: Lag protection prevents fast-forwarding") {
    bcnp::QueueConfig config{};
    config.maxCommandLag = 100ms;
    bcnp::CommandQueue queue(config);
    
    auto now = bcnp::CommandQueue::Clock::time_point{} + 1000ms;
    queue.NotifyPacketReceived(now);
    
    // Queue 5 commands, each 100ms
    for (int i = 0; i < 5; ++i) {
        queue.Push({static_cast<float>(i), 0.0f, 100});
    }
    
    queue.Update(now);
    CHECK(queue.ActiveCommand()->vx == 0.0f);
    
    // Simulate 500ms lag spike (OS pause, GC, etc.)
    now += 500ms;
    queue.NotifyPacketReceived(now); // Keep connection alive despite lag
    queue.Update(now);
    
    // Without lag protection, all 5 commands would skip instantly
    // With protection, we should not skip through all commands
    // After 500ms lag with 100ms max lag, the virtual time is clamped
    // First command started at t=1000, should end at t=1100
    // With 500ms lag at t=1500, we clamp to t=1400 (1500-100)
    // So we've only progressed through ~4 commands worth of time
    
    // The active command should be near the end but not all skipped
    auto active = queue.ActiveCommand();
    if (active.has_value()) {
        // We should have executed most but not necessarily all
        CHECK(active->vx < 5.0f); // Not completely skipped all
    }
    
    // Key test: queue should not be completely empty from fast-forward
    int remaining = queue.Size() + (queue.ActiveCommand().has_value() ? 1 : 0);
    CHECK(remaining >= 1); // At least some commands preserved
}

TEST_CASE("CommandQueue: Virtual time prevents drift") {
    bcnp::CommandQueue queue;
    
    auto now = bcnp::CommandQueue::Clock::time_point{} + 1000ms;
    queue.NotifyPacketReceived(now);
    
    queue.Push({1.0f, 0.0f, 100});
    queue.Push({2.0f, 0.0f, 100});
    
    queue.Update(now);
    
    // First update at 95ms (slightly early)
    now += 95ms;
    queue.Update(now);
    CHECK(queue.ActiveCommand()->vx == 1.0f); // Still first command
    
    // Second update at 105ms (command should transition - 5ms into command 2)
    now += 10ms;
    queue.Update(now);
    CHECK(queue.ActiveCommand()->vx == 2.0f);
    
    // Third update at 210ms (both 100ms commands complete)
    now += 105ms;
    queue.Update(now);
    CHECK(!queue.ActiveCommand().has_value()); // Both complete
    
    // Despite timing jitter, total execution time should be ~200ms
}

// ============================================================================
// Test Suite: StreamParser
// ============================================================================

TEST_CASE("StreamParser: Chunked packet delivery") {
    bcnp::Packet packet{};
    packet.commands.push_back({0.1f, 0.2f, 250});
    
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodePacket(packet, encoded));
    
    bool packetSeen = false;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet& parsed) {
            packetSeen = true;
            CHECK(parsed.commands.size() == 1);
            CHECK(parsed.commands[0].vx == 0.1f);
        },
        [&](const bcnp::StreamParser::ErrorInfo&) {
            FAIL("Unexpected parse error");
        });
    
    // Split packet arbitrarily
    parser.Push(encoded.data(), 3);
    CHECK(!packetSeen);
    
    parser.Push(encoded.data() + 3, encoded.size() - 3);
    CHECK(packetSeen);
}

TEST_CASE("StreamParser: Truncated packet waits without error") {
    bcnp::Packet packet{};
    packet.commands.push_back({0.5f, 0.1f, 100});
    
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodePacket(packet, encoded));
    
    std::atomic<bool> packetSeen{false};
    std::atomic<size_t> errors{0};
    
    bcnp::StreamParser parser(
        [&](const bcnp::Packet&) { packetSeen = true; },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errors; });
    
    // Send all but last byte
    parser.Push(encoded.data(), encoded.size() - 1);
    CHECK(!packetSeen);
    CHECK(errors == 0);
    
    // Send final byte
    parser.Push(encoded.data() + encoded.size() - 1, 1);
    CHECK(packetSeen);
    CHECK(errors == 0);
}

TEST_CASE("StreamParser: Skip bad headers and recover") {
    bcnp::Packet first{};
    first.commands.push_back({0.2f, 0.0f, 150});
    
    bcnp::Packet second{};
    second.commands.push_back({-0.1f, 0.5f, 200});
    
    std::vector<uint8_t> combined;
    std::vector<uint8_t> encoded;
    
    REQUIRE(bcnp::EncodePacket(first, encoded));
    combined.insert(combined.end(), encoded.begin(), encoded.end());
    
    // Insert garbage header
    combined.push_back(bcnp::kProtocolMajor);
    combined.push_back(bcnp::kProtocolMinor);
    combined.push_back(0x00);
    combined.push_back(static_cast<uint8_t>(bcnp::kMaxCommandsPerPacket + 1));
    combined.push_back(0xAA);
    combined.push_back(0x55);
    
    REQUIRE(bcnp::EncodePacket(second, encoded));
    combined.insert(combined.end(), encoded.begin(), encoded.end());
    
    std::vector<bcnp::Packet> seen;
    std::atomic<size_t> errorCount{0};
    
    bcnp::StreamParser parser(
        [&](const bcnp::Packet& parsed) { seen.push_back(parsed); },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errorCount; });
    
    parser.Push(combined.data(), combined.size());
    
    CHECK(errorCount >= 1);
    CHECK(seen.size() == 2);
    CHECK(seen[0].commands[0].vx == 0.2f);
    CHECK(seen[1].commands[0].omega == 0.5f);
}

TEST_CASE("StreamParser: DoS protection - survives garbage flood") {
    bool packetSeen = false;
    bcnp::StreamParser parser(
        [&](const bcnp::Packet&) { packetSeen = true; },
        [](const bcnp::StreamParser::ErrorInfo&) {});
    
    // Flood with garbage beyond buffer limit
    std::vector<uint8_t> garbage(bcnp::StreamParser::kMaxBufferSize + 100, 0xFF);
    parser.Push(garbage.data(), garbage.size());
    
    // Parser should still accept valid packet after flood
    bcnp::Packet packet{};
    packet.commands.push_back({0.1f, 0.1f, 100});
    std::vector<uint8_t> encoded;
    bcnp::EncodePacket(packet, encoded);
    
    parser.Push(encoded.data(), encoded.size());
    CHECK(packetSeen);
}

TEST_CASE("StreamParser: Error info provides diagnostics") {
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
    
    REQUIRE(errors.size() >= 2);
    CHECK(errors[0].code == bcnp::PacketError::TooManyCommands);
    CHECK(errors[0].offset == 0);
    CHECK(errors[0].consecutiveErrors == 1);
    CHECK(errors[1].consecutiveErrors == 2);
    
    // Reset clears error counter
    parser.Reset();
    parser.Push(badHeader.data(), badHeader.size());
    CHECK(errors.back().consecutiveErrors == 1);
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
    packet.commands.push_back({1.0f, -2.0f, 6000}); // All out of range
    controller.HandlePacket(packet);
    
    auto now = bcnp::CommandQueue::Clock::time_point{} + 1000ms;
    controller.Queue().NotifyPacketReceived(now);
    auto cmd = controller.CurrentCommand(now);
    
    REQUIRE(cmd.has_value());
    CHECK(cmd->vx == config.limits.vxMax);
    CHECK(cmd->omega == config.limits.omegaMin);
    CHECK(cmd->durationMs == config.limits.durationMax);
}

TEST_CASE("Controller: Zero defaults require explicit limits") {
    // Default controller with zero limits should clamp everything to zero
    bcnp::Controller controller;
    
    bcnp::Packet packet{};
    packet.commands.push_back({1.0f, 1.0f, 1000});
    controller.HandlePacket(packet);
    
    auto now = bcnp::CommandQueue::Clock::time_point{} + 1000ms;
    controller.Queue().NotifyPacketReceived(now);
    auto cmd = controller.CurrentCommand(now);
    
    REQUIRE(cmd.has_value());
    CHECK(cmd->vx == 0.0f); // Clamped to max=0
    CHECK(cmd->omega == 0.0f);
    CHECK(cmd->durationMs == 0); // Clamped to max=0
}

// ============================================================================
// Test Suite: TCP Adapter (Integration Tests)
// ============================================================================

TEST_CASE("TCP: Basic server-client connection and data transfer") {
    bcnp::TcpPosixAdapter server(12345);
    REQUIRE(server.IsValid());
    
    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12345);
    REQUIRE(client.IsValid());
    
    // Use promise/future for robust synchronization
    std::promise<void> serverConnected;
    std::promise<void> clientReceived;
    
    std::vector<uint8_t> txData = {0x01, 0x02, 0x03, 0x04};
    std::vector<uint8_t> serverRxBuffer(1024);
    std::vector<uint8_t> clientRxBuffer(1024);
    
    // Server thread
    auto serverFuture = std::async(std::launch::async, [&]() {
        // Poll for connection
        for (int i = 0; i < 100; ++i) {
            size_t bytes = server.ReceiveChunk(serverRxBuffer.data(), serverRxBuffer.size());
            if (bytes > 0) {
                CHECK(bytes == txData.size());
                CHECK(serverRxBuffer[0] == 0x01);
                serverConnected.set_value();
                
                // Send response
                std::vector<uint8_t> response = {0x05, 0x06};
                server.SendBytes(response.data(), response.size());
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("Server never received data");
    });
    
    // Client thread
    auto clientFuture = std::async(std::launch::async, [&]() {
        // Wait for connection and send
        for (int i = 0; i < 100; ++i) {
            client.SendBytes(txData.data(), txData.size());
            if (client.IsConnected()) break;
            std::this_thread::sleep_for(10ms);
        }
        
        // Wait for server to respond
        auto status = serverConnected.get_future().wait_for(2s);
        REQUIRE(status == std::future_status::ready);
        
        // Receive response
        for (int i = 0; i < 100; ++i) {
            size_t bytes = client.ReceiveChunk(clientRxBuffer.data(), clientRxBuffer.size());
            if (bytes > 0) {
                CHECK(bytes == 2);
                CHECK(clientRxBuffer[0] == 0x05);
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("Client never received response");
    });
    
    serverFuture.get();
    clientFuture.get();
    
    CHECK(server.IsConnected());
    CHECK(client.IsConnected());
}

TEST_CASE("TCP: Client reconnects after connection loss") {
    bcnp::TcpPosixAdapter server(12346);
    REQUIRE(server.IsValid());
    
    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12346);
    REQUIRE(client.IsValid());
    
    std::vector<uint8_t> data = {0xAA};
    std::vector<uint8_t> rxBuffer(1024);
    
    // Initial connection
    for (int i = 0; i < 100; ++i) {
        client.SendBytes(data.data(), data.size());
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (server.IsConnected() && client.IsConnected()) break;
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(server.IsConnected());
    REQUIRE(client.IsConnected());
    
    // Server drops connection (simulate network issue)
    server.~TcpPosixAdapter();
    new (&server) bcnp::TcpPosixAdapter(12346); // Restart server
    
    // Give client time to detect disconnect
    std::this_thread::sleep_for(100ms);
    
    // Client should reconnect automatically
    bool reconnected = false;
    for (int i = 0; i < 100; ++i) {
        client.SendBytes(data.data(), data.size());
        size_t bytes = server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (bytes > 0 && server.IsConnected()) {
            reconnected = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    
    CHECK(reconnected);
}

TEST_CASE("TCP: Server times out zombie clients") {
    bcnp::TcpPosixAdapter server(12347);
    REQUIRE(server.IsValid());
    
    bcnp::TcpPosixAdapter client1(0, "127.0.0.1", 12347);
    REQUIRE(client1.IsValid());
    
    std::vector<uint8_t> data = {0xBB};
    std::vector<uint8_t> rxBuffer(1024);
    
    // Client 1 connects
    for (int i = 0; i < 100; ++i) {
        client1.SendBytes(data.data(), data.size());
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (server.IsConnected()) break;
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(server.IsConnected());
    
    // Client 1 goes silent for >5 seconds (zombie)
    // Simulate by waiting for server timeout
    std::this_thread::sleep_for(6s);
    
    // Server should poll and detect timeout
    server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
    
    // Server should now accept new client
    bcnp::TcpPosixAdapter client2(0, "127.0.0.1", 12347);
    bool newClientConnected = false;
    
    for (int i = 0; i < 100; ++i) {
        client2.SendBytes(data.data(), data.size());
        size_t bytes = server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (bytes > 0) {
            newClientConnected = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    
    CHECK(newClientConnected);
}

TEST_CASE("TCP: EINPROGRESS handled correctly - IsConnected not premature") {
    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12348);
    REQUIRE(client.IsValid());
    
    // Immediately after construction, client should NOT be connected yet
    // (non-blocking connect returns EINPROGRESS)
    // This is the bug we fixed - it should only be true after getsockopt confirms
    CHECK(!client.IsConnected());
    
    // Start server late to ensure EINPROGRESS path
    std::this_thread::sleep_for(50ms);
    bcnp::TcpPosixAdapter server(12348);
    
    // Eventually should connect
    bool connected = false;
    for (int i = 0; i < 100; ++i) {
        std::vector<uint8_t> dummy = {0x00};
        client.SendBytes(dummy.data(), dummy.size());
        if (client.IsConnected()) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(connected);
}

// ============================================================================
// Test Suite: UDP Adapter
// ============================================================================

TEST_CASE("UDP: Basic send and receive") {
    bcnp::UdpPosixAdapter server(54321);
    REQUIRE(server.IsValid());
    
    bcnp::UdpPosixAdapter client(54322, "127.0.0.1", 54321);
    REQUIRE(client.IsValid());
    
    std::vector<uint8_t> txData = {0x11, 0x22, 0x33};
    client.SendBytes(txData.data(), txData.size());
    
    std::vector<uint8_t> rxBuffer(1024);
    size_t received = 0;
    
    for (int i = 0; i < 50; ++i) {
        received = server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (received > 0) break;
        std::this_thread::sleep_for(10ms);
    }
    
    REQUIRE(received == txData.size());
    CHECK(rxBuffer[0] == 0x11);
    CHECK(rxBuffer[1] == 0x22);
}

TEST_CASE("UDP: Locked mode requires handshake before accepting data") {
    constexpr uint32_t kToken = 0xA5A5A5A5;
    bcnp::UdpPosixAdapter robot(54323);
    robot.SetPeerLockMode(true);
    robot.SetPairingToken(kToken);
    REQUIRE(robot.IsValid());

    bcnp::UdpPosixAdapter attacker(54324, "127.0.0.1", 54323);
    bcnp::UdpPosixAdapter driver(54325, "127.0.0.1", 54323);

    std::vector<uint8_t> rxBuffer(1024);

    // Attacker sends data before pairing; robot should drop it silently.
    std::vector<uint8_t> bogus = {0xAA};
    attacker.SendBytes(bogus.data(), bogus.size());
    CHECK(robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size()) == 0);

    // Attacker attempts handshake with wrong token.
    auto wrongFrame = MakePairingFrame(kToken ^ 0xFF);
    attacker.SendBytes(wrongFrame.data(), wrongFrame.size());
    CHECK(robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size()) == 0);

    // Real driver pairs successfully.
    auto goodFrame = MakePairingFrame(kToken);
    driver.SendBytes(goodFrame.data(), goodFrame.size());
    CHECK(robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size()) == 0);

    // After pairing, only the paired driver is heard.
    std::vector<uint8_t> payload = {0x42};
    driver.SendBytes(payload.data(), payload.size());
    size_t received = 0;
    for (int i = 0; i < 10; ++i) {
        received = robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (received > 0) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(received == payload.size());
    CHECK(rxBuffer[0] == 0x42);

    // Attacker messages remain blocked after pairing.
    attacker.SendBytes(payload.data(), payload.size());
    CHECK(robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size()) == 0);
}

TEST_CASE("UDP: Peer switching when lock disabled") {
    bcnp::UdpPosixAdapter robot(54326);
    robot.SetPeerLockMode(false); // Allow peer switching
    REQUIRE(robot.IsValid());
    
    bcnp::UdpPosixAdapter client1(54327, "127.0.0.1", 54326);
    bcnp::UdpPosixAdapter client2(54328, "127.0.0.1", 54326);
    
    std::vector<uint8_t> data1 = {0x01};
    std::vector<uint8_t> data2 = {0x02};
    std::vector<uint8_t> rxBuffer(1024);
    
    // Client 1 sends
    client1.SendBytes(data1.data(), data1.size());
    std::this_thread::sleep_for(20ms);
    size_t received = robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
    REQUIRE(received > 0);
    CHECK(rxBuffer[0] == 0x01);
    
    // Client 2 sends (peer switch allowed)
    client2.SendBytes(data2.data(), data2.size());
    std::this_thread::sleep_for(20ms);
    received = robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
    REQUIRE(received > 0);
    CHECK(rxBuffer[0] == 0x02);
}
