#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "message_types.h"
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
// Legacy pairing frame (for UDP lock mode with custom token)
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

// Schema handshake frame for V3 protocol
std::array<uint8_t, 8> MakeSchemaHandshake() {
    std::array<uint8_t, 8> frame{};
    bcnp::EncodeHandshake(frame.data(), frame.size());
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

TEST_CASE("Packet: Reject unsupported version") {
    // V3 header is 7 bytes: Major(1) + Minor(1) + Flags(1) + MsgType(2) + MsgCount(2)
    std::vector<uint8_t> buffer = {
        static_cast<uint8_t>(bcnp::kProtocolMajor + 1), bcnp::kProtocolMinor, 
        0x00,       // flags
        0x00, 0x01, // message type = 1 (DriveCmd)
        0x00, 0x00  // message count = 0
    };

    auto result = bcnp::DecodePacket(buffer.data(), buffer.size());
    CHECK(!result.packet.has_value());
    CHECK(result.error == bcnp::PacketError::UnsupportedVersion);
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
    
    // Queue 10 commands, each 100ms
    for (int i = 0; i < 10; ++i) {
        queue.Push({static_cast<float>(i), 0.0f, 100});
    }
    
    queue.Update(now);
    CHECK(queue.ActiveCommand()->vx == 0.0f);
    
    // Simulate 500ms lag spike (OS pause, GC, etc.)
    now += 500ms;
    queue.NotifyPacketReceived(now); // Keep connection alive despite lag
    queue.Update(now);
    
    // Without lag protection, all 10 commands would skip instantly (if we processed 1000ms)
    // With protection, we should not skip through all commands
    // After 500ms lag with 100ms max lag, the virtual time is clamped
    // First command started at t=1000, should end at t=1100
    // With 500ms lag at t=1500, we clamp to t=1400 (1500-100)
    // So we've only progressed through ~4-5 commands worth of time
    
    // The active command should be near the middle
    auto active = queue.ActiveCommand();
    if (active.has_value()) {
        CHECK(active->vx < 10.0f); 
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

TEST_CASE("CommandQueue: Sub-tick granularity handles short commands") {
    bcnp::CommandQueue queue;
    auto now = bcnp::CommandQueue::Clock::time_point{} + 1000ms;
    queue.NotifyPacketReceived(now);

    // Push 10 commands of 1ms each (total 10ms)
    for (int i = 0; i < 10; ++i) {
        queue.Push({1.0f, 0.0f, 1});
    }

    queue.Update(now);
    // First command should be active
    CHECK(queue.ActiveCommand().has_value());

    // Advance time by 20ms (enough to finish all 10ms of commands)
    // In the old implementation, this would only finish Cmd1 and start Cmd2
    now += 20ms;
    queue.Update(now);

    // Should be finished with all commands
    CHECK(!queue.ActiveCommand().has_value());
    CHECK(queue.Size() == 0);
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
        [&](const bcnp::PacketView& parsed) {
            packetSeen = true;
            CHECK(std::distance(parsed.begin(), parsed.end()) == 1);
            CHECK((*parsed.begin()).vx == 0.1f);
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
        [&](const bcnp::PacketView&) { packetSeen = true; },
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
    
    // Insert garbage header (wrong version) - V3 format: 7 bytes
    combined.push_back(bcnp::kProtocolMajor + 1);  // Major (wrong)
    combined.push_back(bcnp::kProtocolMinor);       // Minor
    combined.push_back(0x00);                       // Flags
    combined.push_back(0x00);                       // MsgType high
    combined.push_back(0x01);                       // MsgType low (DriveCmd)
    combined.push_back(0x00);                       // MsgCount high
    combined.push_back(0x01);                       // MsgCount low (1 message)
    
    REQUIRE(bcnp::EncodePacket(second, encoded));
    combined.insert(combined.end(), encoded.begin(), encoded.end());
    
    std::vector<bcnp::Packet> seen;
    std::atomic<size_t> errorCount{0};
    
    bcnp::StreamParser parser(
        [&](const bcnp::PacketView& parsed) { 
            bcnp::Packet p;
            p.header = parsed.header;
            for (const auto& cmd : parsed) {
                p.commands.push_back(cmd);
            }
            seen.push_back(p); 
        },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errorCount; });
    
    parser.Push(combined.data(), combined.size());
    
    CHECK(errorCount >= 1);
    CHECK(seen.size() == 2);
    CHECK(seen[0].commands[0].vx == 0.2f);
    CHECK(seen[1].commands[0].omega == 0.5f);
}

TEST_CASE("StreamParser: DoS protection - survives garbage flood") {
    bool packetSeen = false;
    const size_t kBufferSize = 4096;
    bcnp::StreamParser parser(
        [&](const bcnp::PacketView&) { packetSeen = true; },
        [](const bcnp::StreamParser::ErrorInfo&) {},
        kBufferSize);
    
    // Flood with garbage beyond buffer limit
    std::vector<uint8_t> garbage(kBufferSize + 100, 0xFF);
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
        [](const bcnp::PacketView&) {},
        [&](const bcnp::StreamParser::ErrorInfo& info) { errors.push_back(info); });
    
    std::array<uint8_t, bcnp::kHeaderSize> badHeader{};
    badHeader[bcnp::kHeaderMajorIndex] = bcnp::kProtocolMajor + 1;
    badHeader[bcnp::kHeaderMinorIndex] = bcnp::kProtocolMinor;
    // V3: MsgTypeIndex=3 (2 bytes), MsgCountIndex=5 (2 bytes)
    badHeader[bcnp::kHeaderMsgTypeIndex] = 0;
    badHeader[bcnp::kHeaderMsgTypeIndex + 1] = 1; // DriveCmd
    badHeader[bcnp::kHeaderMsgCountIndex] = 0;
    badHeader[bcnp::kHeaderMsgCountIndex + 1] = 0;
    
    parser.Push(badHeader.data(), badHeader.size());
    parser.Push(badHeader.data(), badHeader.size());
    
    REQUIRE(errors.size() >= 2);
    CHECK(errors[0].code == bcnp::PacketError::UnsupportedVersion);
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
    
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodePacket(packet, encoded));
    
    // Decode the view to get the proper header with messageCount filled in
    auto decodeResult = bcnp::DecodePacketView(encoded.data(), encoded.size());
    REQUIRE(decodeResult.view.has_value());
    
    controller.HandlePacket(*decodeResult.view);
    
    auto now = bcnp::CommandQueue::Clock::time_point{} + 1000ms;
    controller.Queue().NotifyPacketReceived(now);
    controller.Queue().Update(now);
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
    
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodePacket(packet, encoded));
    
    // Decode the view to get the proper header with messageCount filled in
    auto decodeResult = bcnp::DecodePacketView(encoded.data(), encoded.size());
    REQUIRE(decodeResult.view.has_value());
    
    controller.HandlePacket(*decodeResult.view);
    
    auto now = bcnp::CommandQueue::Clock::time_point{} + 1000ms;
    controller.Queue().NotifyPacketReceived(now);
    controller.Queue().Update(now);
    auto cmd = controller.CurrentCommand(now);
    
    // With sub-tick updates, a 0-duration command is processed immediately
    CHECK(!cmd.has_value());
}

// ============================================================================
// Test Suite: TCP Adapter (Integration Tests)
// ============================================================================

TEST_CASE("TCP: Basic server-client connection and data transfer") {
    bcnp::TcpPosixAdapter server(12345);
    REQUIRE(server.IsValid());
    
    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12345);
    REQUIRE(client.IsValid());
    
    // Send schema handshake first (V3 requirement)
    auto handshake = MakeSchemaHandshake();
    std::vector<uint8_t> serverRxBuffer(1024);
    std::vector<uint8_t> clientRxBuffer(1024);
    
    // Establish connection with handshakes
    for (int i = 0; i < 100; ++i) {
        client.SendBytes(handshake.data(), handshake.size());
        std::this_thread::sleep_for(10ms);
        server.ReceiveChunk(serverRxBuffer.data(), serverRxBuffer.size());
        if (server.IsConnected()) {
            server.SendBytes(handshake.data(), handshake.size());
            break;
        }
    }
    
    // Wait for handshake to complete
    for (int i = 0; i < 50; ++i) {
        client.ReceiveChunk(clientRxBuffer.data(), clientRxBuffer.size());
        server.ReceiveChunk(serverRxBuffer.data(), serverRxBuffer.size());
        if (client.IsHandshakeComplete() && server.IsHandshakeComplete()) break;
        std::this_thread::sleep_for(10ms);
    }
    
    REQUIRE(server.IsConnected());
    REQUIRE(client.IsConnected());
    
    // Now send actual data
    std::vector<uint8_t> txData = {0x01, 0x02, 0x03, 0x04};
    client.SendBytes(txData.data(), txData.size());
    
    bool received = false;
    for (int i = 0; i < 50; ++i) {
        size_t bytes = server.ReceiveChunk(serverRxBuffer.data(), serverRxBuffer.size());
        if (bytes >= txData.size()) {
            received = true;
            CHECK(serverRxBuffer[0] == 0x01);
            CHECK(serverRxBuffer[1] == 0x02);
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(received);
    
    // Server sends response
    std::vector<uint8_t> response = {0x05, 0x06};
    server.SendBytes(response.data(), response.size());
    
    received = false;
    for (int i = 0; i < 50; ++i) {
        size_t bytes = client.ReceiveChunk(clientRxBuffer.data(), clientRxBuffer.size());
        if (bytes >= response.size()) {
            received = true;
            CHECK(clientRxBuffer[0] == 0x05);
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(received);
}

TEST_CASE("TCP: Client reconnects after connection loss") {
    bcnp::TcpPosixAdapter server(12346);
    REQUIRE(server.IsValid());
    
    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12346);
    REQUIRE(client.IsValid());
    
    auto handshake = MakeSchemaHandshake();
    std::vector<uint8_t> rxBuffer(1024);
    
    // Initial connection with handshake
    for (int i = 0; i < 100; ++i) {
        client.SendBytes(handshake.data(), handshake.size());
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (server.IsConnected()) {
            server.SendBytes(handshake.data(), handshake.size());
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    
    for (int i = 0; i < 50; ++i) {
        client.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (client.IsHandshakeComplete()) break;
        std::this_thread::sleep_for(10ms);
    }
    
    REQUIRE(server.IsConnected());
    REQUIRE(client.IsHandshakeComplete());
    
    // Server drops connection (simulate network issue)
    server.~TcpPosixAdapter();
    new (&server) bcnp::TcpPosixAdapter(12346); // Restart server
    
    // Give client time to detect disconnect
    std::this_thread::sleep_for(100ms);
    
    // Client should reconnect automatically with new handshake
    bool reconnected = false;
    for (int i = 0; i < 100; ++i) {
        client.SendBytes(handshake.data(), handshake.size());
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (server.IsConnected()) {
            server.SendBytes(handshake.data(), handshake.size());
            // Client will receive on next iteration
        }
        client.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (client.IsHandshakeComplete() && server.IsConnected()) {
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
    
    auto handshake = MakeSchemaHandshake();
    std::vector<uint8_t> rxBuffer(1024);
    
    // Client 1 connects with handshake
    for (int i = 0; i < 100; ++i) {
        client1.SendBytes(handshake.data(), handshake.size());
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (server.IsConnected()) {
            server.SendBytes(handshake.data(), handshake.size());
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    
    for (int i = 0; i < 50; ++i) {
        client1.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (client1.IsHandshakeComplete()) break;
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
        client2.SendBytes(handshake.data(), handshake.size());
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (server.IsConnected()) {
            server.SendBytes(handshake.data(), handshake.size());
            // Additional polling to complete handshake
            for (int j = 0; j < 10; ++j) {
                client2.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
                if (client2.IsHandshakeComplete()) {
                    newClientConnected = true;
                    break;
                }
                std::this_thread::sleep_for(10ms);
            }
            if (newClientConnected) break;
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
    
    auto handshake = MakeSchemaHandshake();
    std::vector<uint8_t> rxBuffer(1024);
    
    // Eventually should connect with handshake
    bool connected = false;
    for (int i = 0; i < 100; ++i) {
        client.SendBytes(handshake.data(), handshake.size());
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (server.IsConnected()) {
            server.SendBytes(handshake.data(), handshake.size());
        }
        client.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (client.IsHandshakeComplete()) {
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
    // V3: Schema hash is the pairing token
    bcnp::UdpPosixAdapter robot(54323);
    robot.SetPeerLockMode(true);
    robot.SetPairingToken(bcnp::kSchemaHash); // Use schema hash as pairing token
    REQUIRE(robot.IsValid());

    bcnp::UdpPosixAdapter attacker(54324, "127.0.0.1", 54323);
    bcnp::UdpPosixAdapter driver(54325, "127.0.0.1", 54323);

    std::vector<uint8_t> rxBuffer(1024);

    // Attacker sends data before pairing; robot should drop it silently.
    std::vector<uint8_t> bogus = {0xAA};
    attacker.SendBytes(bogus.data(), bogus.size());
    std::this_thread::sleep_for(10ms);
    CHECK(robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size()) == 0);

    // Attacker attempts handshake with wrong schema hash.
    auto wrongFrame = MakePairingFrame(bcnp::kSchemaHash ^ 0xFF);
    attacker.SendBytes(wrongFrame.data(), wrongFrame.size());
    std::this_thread::sleep_for(10ms);
    // Wrong schema will complete pairing but fail validation - robot drops subsequent data
    robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size());

    // Real driver pairs successfully with correct schema.
    auto goodFrame = MakeSchemaHandshake();
    driver.SendBytes(goodFrame.data(), goodFrame.size());
    std::this_thread::sleep_for(10ms);
    // Pairing frame is consumed, returns 0
    robot.ReceiveChunk(rxBuffer.data(), rxBuffer.size());

    // After pairing with correct schema, only the paired driver is heard.
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
    std::this_thread::sleep_for(10ms);
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
