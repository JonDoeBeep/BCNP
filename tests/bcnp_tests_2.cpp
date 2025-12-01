#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <bcnp/message_types.h>
#include "bcnp/message_queue.h"
#include "bcnp/dispatcher.h"
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
// Schema handshake frame for V3 protocol
std::array<uint8_t, 8> MakeSchemaHandshake() {
    std::array<uint8_t, 8> frame{};
    bcnp::EncodeHandshake(frame.data(), frame.size());
    return frame;
}

// Helper to get wire size for test message types
std::size_t TestWireSizeLookup(bcnp::MessageTypeId typeId) {
    if (typeId == bcnp::TestCmd::kTypeId) {
        return bcnp::TestCmd::kWireSize;
    }
    return 0;
}
}

// ============================================================================
// Test Suite: Packet Encoding/Decoding
// ============================================================================

TEST_CASE("Packet: Encode and decode round-trip") {
    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.header.flags = bcnp::kFlagClearQueue;
    packet.messages.push_back({0.5f, -1.0f, 1500});
    packet.messages.push_back({-0.25f, 0.25f, 500});

    std::vector<uint8_t> buffer;
    const bool encoded = bcnp::EncodeTypedPacket(packet, buffer);
    REQUIRE(encoded);

    const auto decode = bcnp::DecodePacketViewAs<bcnp::TestCmd>(buffer.data(), buffer.size());
    REQUIRE(decode.view.has_value());
    
    auto typedPacket = bcnp::DecodeTypedPacket<bcnp::TestCmd>(*decode.view);
    REQUIRE(typedPacket.has_value());
    CHECK(typedPacket->messages.size() == 2);
    CHECK(typedPacket->messages[0].value1 == 0.5f);
    CHECK(typedPacket->messages[0].value2 == -1.0f);
    CHECK(typedPacket->messages[0].durationMs == 1500);
    CHECK(typedPacket->messages[1].value1 == -0.25f);
    CHECK(typedPacket->messages[1].value2 == 0.25f);
}

TEST_CASE("Packet: CRC detects payload corruption") {
    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.messages.push_back({0.25f, -0.5f, 100});

    std::vector<uint8_t> bytes;
    REQUIRE(bcnp::EncodeTypedPacket(packet, bytes));

    // Flip one payload byte without updating the checksum
    REQUIRE(bytes.size() > bcnp::kHeaderSize);
    bytes[bcnp::kHeaderSize] ^= 0xFF;

    auto result = bcnp::DecodePacketViewAs<bcnp::TestCmd>(bytes.data(), bytes.size());
    CHECK(!result.view.has_value());
    CHECK(result.error == bcnp::PacketError::ChecksumMismatch);
}

TEST_CASE("Packet: Reject unsupported version") {
    // V3 header is 7 bytes: Major(1) + Minor(1) + Flags(1) + MsgType(2) + MsgCount(2)
    std::vector<uint8_t> buffer = {
        static_cast<uint8_t>(bcnp::kProtocolMajor + 1), bcnp::kProtocolMinor, 
        0x00,       // flags
        0x00, 0x01, // message type = 1 (TestCmd)
        0x00, 0x00  // message count = 0
    };

    // Use type-aware decode to avoid registry lookup issues
    auto result = bcnp::DecodePacketViewAs<bcnp::TestCmd>(buffer.data(), buffer.size());
    CHECK(!result.view.has_value());
    CHECK(result.error == bcnp::PacketError::UnsupportedVersion);
}

// ============================================================================
// Test Suite: MessageQueue (Logic Tests - No Sleep)
// ============================================================================

TEST_CASE("MessageQueue: Basic message execution timing") {
    bcnp::MessageQueue<bcnp::TestCmd> queue;
    
    // Use deterministic time
    auto now = bcnp::MessageQueue<bcnp::TestCmd>::Clock::time_point{} + 1000ms;
    
    queue.Push({1.0f, 0.0f, 100});
    queue.Push({2.0f, 0.5f, 50});
    
    queue.NotifyReceived(now);
    queue.Update(now);
    
    auto msg = queue.ActiveMessage();
    REQUIRE(msg.has_value());
    CHECK(msg->value1 == 1.0f);
    
    // Update at t=50ms (mid-message1)
    now += 50ms;
    queue.Update(now);
    CHECK(queue.ActiveMessage()->value1 == 1.0f);
    
    // Update at t=100ms (message1 should end, message2 starts)
    now += 50ms;
    queue.Update(now);
    msg = queue.ActiveMessage();
    REQUIRE(msg.has_value());
    CHECK(msg->value1 == 2.0f);
    
    // Update at t=150ms (mid-message2, which ends at t=150 since it started at t=100)
    now += 50ms;
    queue.Update(now);
    msg = queue.ActiveMessage();
    CHECK(!msg.has_value());
}

TEST_CASE("MessageQueue: Disconnect clears active message immediately") {
    bcnp::MessageQueueConfig config{};
    config.connectionTimeout = 50ms;
    bcnp::MessageQueue<bcnp::TestCmd> queue(config);
    
    auto now = bcnp::MessageQueue<bcnp::TestCmd>::Clock::time_point{} + 1000ms;
    queue.NotifyReceived(now);
    queue.Push({0.0f, 0.0f, 60000}); // 60 second message
    queue.Update(now);
    
    REQUIRE(queue.ActiveMessage().has_value());
    
    // Exceed timeout - message should be cleared for safety
    queue.Update(now + config.connectionTimeout + 1ms);
    CHECK(!queue.ActiveMessage().has_value());
    CHECK(queue.Size() == 0);
}

TEST_CASE("MessageQueue: Lag protection prevents fast-forwarding") {
    bcnp::MessageQueueConfig config{};
    config.maxCommandLag = 100ms;
    bcnp::MessageQueue<bcnp::TestCmd> queue(config);
    
    auto now = bcnp::MessageQueue<bcnp::TestCmd>::Clock::time_point{} + 1000ms;
    queue.NotifyReceived(now);
    
    // Queue 10 messages, each 100ms
    for (int i = 0; i < 10; ++i) {
        queue.Push({static_cast<float>(i), 0.0f, 100});
    }
    
    queue.Update(now);
    CHECK(queue.ActiveMessage()->value1 == 0.0f);
    
    // Simulate 500ms lag spike (OS pause, GC, etc.)
    now += 500ms;
    queue.NotifyReceived(now); // Keep connection alive despite lag
    queue.Update(now);
    
    // Key test: queue should not be completely empty from fast-forward
    int remaining = queue.Size() + (queue.ActiveMessage().has_value() ? 1 : 0);
    CHECK(remaining >= 1); // At least some messages preserved
}

TEST_CASE("MessageQueue: Virtual time prevents drift") {
    bcnp::MessageQueue<bcnp::TestCmd> queue;
    
    auto now = bcnp::MessageQueue<bcnp::TestCmd>::Clock::time_point{} + 1000ms;
    queue.NotifyReceived(now);
    
    queue.Push({1.0f, 0.0f, 100});
    queue.Push({2.0f, 0.0f, 100});
    
    queue.Update(now);
    
    // First update at 95ms (slightly early)
    now += 95ms;
    queue.Update(now);
    CHECK(queue.ActiveMessage()->value1 == 1.0f); // Still first message
    
    // Second update at 105ms (message should transition - 5ms into message 2)
    now += 10ms;
    queue.Update(now);
    CHECK(queue.ActiveMessage()->value1 == 2.0f);
    
    // Third update at 210ms (both 100ms messages complete)
    now += 105ms;
    queue.Update(now);
    CHECK(!queue.ActiveMessage().has_value()); // Both complete
}

TEST_CASE("MessageQueue: Sub-tick granularity handles short messages") {
    bcnp::MessageQueue<bcnp::TestCmd> queue;
    auto now = bcnp::MessageQueue<bcnp::TestCmd>::Clock::time_point{} + 1000ms;
    queue.NotifyReceived(now);

    // Push 10 messages of 1ms each (total 10ms)
    for (int i = 0; i < 10; ++i) {
        queue.Push({1.0f, 0.0f, 1});
    }

    queue.Update(now);
    // First message should be active
    CHECK(queue.ActiveMessage().has_value());

    // Advance time by 20ms (enough to finish all 10ms of messages)
    now += 20ms;
    queue.Update(now);

    // Should be finished with all messages
    CHECK(!queue.ActiveMessage().has_value());
    CHECK(queue.Size() == 0);
}

// ============================================================================
// Test Suite: StreamParser
// ============================================================================

TEST_CASE("StreamParser: Chunked packet delivery") {
    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.messages.push_back({0.1f, 0.2f, 250});
    
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodeTypedPacket(packet, encoded));
    
    bool packetSeen = false;
    bcnp::StreamParser parser(
        [&](const bcnp::PacketView& parsed) {
            packetSeen = true;
            int count = 0;
            for (auto it = parsed.begin_as<bcnp::TestCmd>(); it != parsed.end_as<bcnp::TestCmd>(); ++it) {
                CHECK((*it).value1 == 0.1f);
                ++count;
            }
            CHECK(count == 1);
        },
        [&](const bcnp::StreamParser::ErrorInfo&) {
            FAIL("Unexpected parse error");
        });
    parser.SetWireSizeLookup(TestWireSizeLookup);
    
    // Split packet arbitrarily
    parser.Push(encoded.data(), 3);
    CHECK(!packetSeen);
    
    parser.Push(encoded.data() + 3, encoded.size() - 3);
    CHECK(packetSeen);
}

TEST_CASE("StreamParser: Truncated packet waits without error") {
    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.messages.push_back({0.5f, 0.1f, 100});
    
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodeTypedPacket(packet, encoded));
    
    std::atomic<bool> packetSeen{false};
    std::atomic<size_t> errors{0};
    
    bcnp::StreamParser parser(
        [&](const bcnp::PacketView&) { packetSeen = true; },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errors; });
    parser.SetWireSizeLookup(TestWireSizeLookup);
    
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
    bcnp::TypedPacket<bcnp::TestCmd> first;
    first.messages.push_back({0.2f, 0.0f, 150});
    
    bcnp::TypedPacket<bcnp::TestCmd> second;
    second.messages.push_back({-0.1f, 0.5f, 200});
    
    std::vector<uint8_t> combined;
    std::vector<uint8_t> encoded;
    
    REQUIRE(bcnp::EncodeTypedPacket(first, encoded));
    combined.insert(combined.end(), encoded.begin(), encoded.end());
    
    // Insert garbage header (wrong version) - V3 format: 7 bytes
    combined.push_back(bcnp::kProtocolMajor + 1);  // Major (wrong)
    combined.push_back(bcnp::kProtocolMinor);       // Minor
    combined.push_back(0x00);                       // Flags
    combined.push_back(0x00);                       // MsgType high
    combined.push_back(0x01);                       // MsgType low (TestCmd)
    combined.push_back(0x00);                       // MsgCount high
    combined.push_back(0x01);                       // MsgCount low (1 message)
    
    REQUIRE(bcnp::EncodeTypedPacket(second, encoded));
    combined.insert(combined.end(), encoded.begin(), encoded.end());
    
    std::vector<bcnp::TypedPacket<bcnp::TestCmd>> seen;
    std::atomic<size_t> errorCount{0};
    
    bcnp::StreamParser parser(
        [&](const bcnp::PacketView& parsed) { 
            auto p = bcnp::DecodeTypedPacket<bcnp::TestCmd>(parsed);
            if (p) seen.push_back(*p); 
        },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errorCount; });
    parser.SetWireSizeLookup(TestWireSizeLookup);
    
    parser.Push(combined.data(), combined.size());
    
    CHECK(errorCount >= 1);
    CHECK(seen.size() == 2);
    CHECK(seen[0].messages[0].value1 == 0.2f);
    CHECK(seen[1].messages[0].value2 == 0.5f);
}

TEST_CASE("StreamParser: DoS protection - survives garbage flood") {
    bool packetSeen = false;
    const size_t kBufferSize = 4096;
    bcnp::StreamParser parser(
        [&](const bcnp::PacketView&) { packetSeen = true; },
        [](const bcnp::StreamParser::ErrorInfo&) {},
        kBufferSize);
    parser.SetWireSizeLookup(TestWireSizeLookup);
    
    // Flood with garbage beyond buffer limit
    std::vector<uint8_t> garbage(kBufferSize + 100, 0xFF);
    parser.Push(garbage.data(), garbage.size());
    
    // Parser should still accept valid packet after flood
    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.messages.push_back({0.1f, 0.1f, 100});
    std::vector<uint8_t> encoded;
    bcnp::EncodeTypedPacket(packet, encoded);
    
    parser.Push(encoded.data(), encoded.size());
    CHECK(packetSeen);
}

TEST_CASE("StreamParser: Error info provides diagnostics") {
    std::vector<bcnp::StreamParser::ErrorInfo> errors;
    bcnp::StreamParser parser(
        [](const bcnp::PacketView&) {},
        [&](const bcnp::StreamParser::ErrorInfo& info) { errors.push_back(info); });
    parser.SetWireSizeLookup(TestWireSizeLookup);
    
    std::array<uint8_t, bcnp::kHeaderSize> badHeader{};
    badHeader[bcnp::kHeaderMajorIndex] = bcnp::kProtocolMajor + 1;
    badHeader[bcnp::kHeaderMinorIndex] = bcnp::kProtocolMinor;
    // V3: MsgTypeIndex=3 (2 bytes), MsgCountIndex=5 (2 bytes)
    badHeader[bcnp::kHeaderMsgTypeIndex] = 0;
    badHeader[bcnp::kHeaderMsgTypeIndex + 1] = 1; // TestCmd
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
// Test Suite: PacketDispatcher
// ============================================================================

TEST_CASE("PacketDispatcher: Routes packets to handlers") {
    bcnp::PacketDispatcher dispatcher;
    dispatcher.RegisterMessageTypes<bcnp::TestCmd>();
    bcnp::MessageQueue<bcnp::TestCmd> queue;
    
    dispatcher.RegisterHandler<bcnp::TestCmd>([&](const bcnp::PacketView& pkt) {
        for (auto it = pkt.begin_as<bcnp::TestCmd>(); it != pkt.end_as<bcnp::TestCmd>(); ++it) {
            queue.Push(*it);
        }
        queue.NotifyReceived(bcnp::MessageQueue<bcnp::TestCmd>::Clock::now());
    });
    
    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.messages.push_back({1.0f, -2.0f, 6000}); // All out of range
    
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodeTypedPacket(packet, encoded));
    
    dispatcher.PushBytes(encoded.data(), encoded.size());
    
    auto now = bcnp::MessageQueue<bcnp::TestCmd>::Clock::time_point{} + 1000ms;
    queue.Update(now);
    auto msg = queue.ActiveMessage();
    
    REQUIRE(msg.has_value());
    CHECK(msg->value1 == 1.0f);
    CHECK(msg->value2 == -2.0f);
    CHECK(msg->durationMs == 6000);
}

// ============================================================================
// Test Suite: TCP Adapter (Integration Tests)
// ============================================================================

TEST_CASE("TCP: Basic server-client connection and data transfer") {
    bcnp::TcpPosixAdapter server(12345);
    REQUIRE(server.IsValid());
    server.SetExpectedSchemaHash(bcnp::kSchemaHash);
    
    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12345);
    REQUIRE(client.IsValid());
    client.SetExpectedSchemaHash(bcnp::kSchemaHash);
    
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
    server.SetExpectedSchemaHash(bcnp::kSchemaHash);
    
    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12346);
    REQUIRE(client.IsValid());
    client.SetExpectedSchemaHash(bcnp::kSchemaHash);
    
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
    server.SetExpectedSchemaHash(bcnp::kSchemaHash);
    
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
