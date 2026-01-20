#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <bcnp/message_types.h>
#include "bcnp/message_queue.h"
#include "bcnp/dispatcher.h"
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

namespace {
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
    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.header.flags = bcnp::kFlagClearQueue;
    packet.messages.push_back({0.5f, -1.0f, 1500});
    packet.messages.push_back({-0.25f, 0.25f, 500});

    std::vector<uint8_t> buffer;
    const bool encoded = bcnp::EncodeTypedPacket(packet, buffer);
    REQUIRE(encoded);

    const auto decode = bcnp::DecodePacketViewAs<bcnp::TestCmd>(buffer.data(), buffer.size());
    REQUIRE(decode.view.is_some());
    
    auto typedPacket = bcnp::DecodeTypedPacket<bcnp::TestCmd>(decode.view.unwrap());
    REQUIRE(typedPacket.is_some());
    CHECK(typedPacket.unwrap().messages.size() == 2);
    CHECK(typedPacket.unwrap().messages[0].value1 == 0.5f);
    CHECK(typedPacket.unwrap().messages[1].value2 == 0.25f);
}

TEST_CASE("Packet: Detects checksum mismatch") {
    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.messages.push_back({0.1f, 0.2f, 250});

    std::vector<uint8_t> bytes;
    REQUIRE(bcnp::EncodeTypedPacket(packet, bytes));

    bytes.back() ^= 0xFF; // Corrupt CRC without touching payload

    const auto decode = bcnp::DecodePacketViewAs<bcnp::TestCmd>(bytes.data(), bytes.size());
    CHECK(!decode.view.is_some());
    CHECK(decode.error == bcnp::PacketError::ChecksumMismatch);
}

// ============================================================================
// Test Suite: MessageQueue
// ============================================================================

TEST_CASE("MessageQueue: Basic message execution timing") {
    bcnp::MessageQueue<bcnp::TestCmd> queue;
    queue.Push({1.0f, 0.0f, 100});
    queue.Push({2.0f, 0.5f, 50});

    const auto start = bcnp::MessageQueue<bcnp::TestCmd>::Clock::now();
    queue.NotifyReceived(start);
    queue.Update(start);
    auto msg = queue.ActiveMessage();
    REQUIRE(msg.is_some());
    CHECK(msg.unwrap().value1 == 1.0f);

    queue.Update(start + 110ms);
    msg = queue.ActiveMessage();
    REQUIRE(msg.is_some());
    CHECK(msg.unwrap().value1 == 2.0f);

    queue.Update(start + 200ms);
    msg = queue.ActiveMessage();
    CHECK(!msg.is_some());
}

// ============================================================================
// Test Suite: StreamParser
// ============================================================================

namespace {
// Helper to get wire size for test message types
std::size_t TestWireSizeLookup(bcnp::MessageTypeId typeId) {
    if (typeId == bcnp::TestCmd::kTypeId) {
        return bcnp::TestCmd::kWireSize;
    }
    return 0;
}
}

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
                ++count;
            }
            CHECK(count == 1);
        },
        [&](const bcnp::StreamParser::ErrorInfo&) {
            FAIL("Unexpected parse error");
        });
    parser.SetWireSizeLookup(TestWireSizeLookup);

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

    bool packetSeen = false;
    std::size_t errors = 0;
    bcnp::StreamParser parser(
        [&](const bcnp::PacketView&) { packetSeen = true; },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errors; });
    parser.SetWireSizeLookup(TestWireSizeLookup);

    parser.Push(encoded.data(), encoded.size() - 1);
    CHECK(!packetSeen);
    CHECK(errors == 0);

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

    // Append a malformed header (wrong protocol version) - V3 format: 7 bytes
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
    std::size_t errorCount = 0;
    bcnp::StreamParser parser(
        [&](const bcnp::PacketView& parsed) {
            auto p = bcnp::DecodeTypedPacket<bcnp::TestCmd>(parsed);
            if (p.is_some()) seen.push_back(p.unwrap());
        },
        [&](const bcnp::StreamParser::ErrorInfo&) { ++errorCount; });
    parser.SetWireSizeLookup(TestWireSizeLookup);

    parser.Push(combined.data(), combined.size());

    CHECK(errorCount >= 1);
    CHECK(seen.size() == 2);
    CHECK(seen.front().messages.front().value1 == first.messages.front().value1);
    CHECK(seen.back().messages.front().value2 == second.messages.front().value2);
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

    parser.Reset();
    parser.Push(badHeader.data(), badHeader.size());
    CHECK(errors.size() > 2);
    CHECK(errors.back().consecutiveErrors == 1);
}

TEST_CASE("StreamParser: DoS protection - survives garbage flood") {
    bool packetSeen = false;
    const size_t kBufferSize = 4096;
    bcnp::StreamParser parser(
        [&](const bcnp::PacketView&) { packetSeen = true; },
        [](const bcnp::StreamParser::ErrorInfo&) {},
        kBufferSize);
    parser.SetWireSizeLookup(TestWireSizeLookup);

    std::vector<uint8_t> garbage(kBufferSize + 100, 0xFF);
    parser.Push(garbage.data(), garbage.size());

    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.messages.push_back({0.1f, 0.1f, 100});
    std::vector<uint8_t> encoded;
    bcnp::EncodeTypedPacket(packet, encoded);

    parser.Push(encoded.data(), encoded.size());
    CHECK(packetSeen);
}

// ============================================================================
// Test Suite: PacketDispatcher
// ============================================================================

TEST_CASE("PacketDispatcher: Routes to registered handlers") {
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
    packet.messages.push_back({1.0f, -2.0f, 6000});
    
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodeTypedPacket(packet, encoded));
    
    dispatcher.PushBytes(encoded.data(), encoded.size());
    
    auto now = bcnp::MessageQueue<bcnp::TestCmd>::Clock::now();
    queue.Update(now);
    
    auto msg = queue.ActiveMessage();
    REQUIRE(msg.is_some());
    CHECK(msg.unwrap().value1 == 1.0f);
    CHECK(msg.unwrap().value2 == -2.0f);
    CHECK(msg.unwrap().durationMs == 6000);
}

TEST_CASE("MessageQueue: Disconnect clears active message immediately") {
    bcnp::MessageQueueConfig config{};
    config.connectionTimeout = 50ms;
    bcnp::MessageQueue<bcnp::TestCmd> queue(config);

    const auto now = bcnp::MessageQueue<bcnp::TestCmd>::Clock::now();
    queue.NotifyReceived(now);
    queue.Push({0.0f, 0.0f, 60000});
    queue.Update(now);
    REQUIRE(queue.ActiveMessage().is_some());

    const auto later = now + config.connectionTimeout + 1ms;
    queue.Update(later);
    CHECK(!queue.ActiveMessage().is_some());
    CHECK(queue.Size() == 0);
}

// ============================================================================
// Test Suite: Thread Safety
// ============================================================================

TEST_CASE("PacketDispatcher: Thread-safe PushBytes from multiple threads") {
    bcnp::PacketDispatcher dispatcher;
    dispatcher.RegisterMessageTypes<bcnp::TestCmd>();
    std::atomic<int> packetsReceived{0};
    
    dispatcher.RegisterHandler<bcnp::TestCmd>([&](const bcnp::PacketView&) {
        ++packetsReceived;
    });
    
    // Prepare valid packets
    bcnp::TypedPacket<bcnp::TestCmd> packet;
    packet.messages.push_back({0.1f, 0.1f, 100});
    std::vector<uint8_t> encoded;
    REQUIRE(bcnp::EncodeTypedPacket(packet, encoded));
    
    // Launch 3 threads that all push the same packet repeatedly
    const int threadsCount = 3;
    const int iterationsPerThread = 50;
    
    std::vector<std::thread> threads;
    for (int t = 0; t < threadsCount; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < iterationsPerThread; ++i) {
                dispatcher.PushBytes(encoded.data(), encoded.size());
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify no crashes occurred and packets were received
    CHECK(packetsReceived > 0);
}

TEST_CASE("MessageQueue: Concurrent Push and ActiveMessage") {
    bcnp::MessageQueue<bcnp::TestCmd> queue;
    std::atomic<bool> running{true};
    std::atomic<int> pushCount{0};
    std::atomic<int> readCount{0};
    
    auto now = bcnp::MessageQueue<bcnp::TestCmd>::Clock::now();
    queue.NotifyReceived(now);
    
    // Thread 1: Continuously push messages
    std::thread pusher([&]() {
        for (int i = 0; i < 100; ++i) {
            if (queue.Push({0.1f, 0.1f, 10})) {
                ++pushCount;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        running = false;
    });
    
    // Thread 2: Continuously read active message
    std::thread reader([&]() {
        while (running) {
            queue.Update(bcnp::MessageQueue<bcnp::TestCmd>::Clock::now());
            auto msg = queue.ActiveMessage();
            if (msg.is_some()) {
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
    // Use test schema hash (generated from tests/test_schema.json)
    server.SetExpectedSchemaHash(bcnp::kSchemaHash);

    bcnp::TcpPosixAdapter client(0, "127.0.0.1", 12345);
    REQUIRE(client.IsValid());
    client.SetExpectedSchemaHash(bcnp::kSchemaHash);

    // V3 requires schema handshake first
    auto handshake = MakeSchemaHandshake();
    std::vector<uint8_t> rxBuffer(1024);
    
    // Establish connection with handshakes
    for (int i = 0; i < 100; ++i) {
        client.SendBytes(handshake.data(), handshake.size());
        std::this_thread::sleep_for(10ms);
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (server.IsConnected()) {
            server.SendBytes(handshake.data(), handshake.size());
            break;
        }
    }
    
    // Wait for handshake completion
    for (int i = 0; i < 50; ++i) {
        client.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (client.IsHandshakeComplete()) break;
        std::this_thread::sleep_for(10ms);
    }
    
    REQUIRE(server.IsConnected());
    REQUIRE(client.IsHandshakeComplete());
    
    // Now send actual data
    std::vector<uint8_t> txData = {0x01, 0x02, 0x03, 0x04};
    client.SendBytes(txData.data(), txData.size());

    bool received = false;
    for (int i = 0; i < 50; ++i) {
        size_t bytes = server.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (bytes >= txData.size()) {
            received = true;
            CHECK(rxBuffer[0] == 0x01);
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(received);

    std::vector<uint8_t> response = {0x05, 0x06};
    server.SendBytes(response.data(), response.size());

    received = false;
    for (int i = 0; i < 50; ++i) {
        size_t bytes = client.ReceiveChunk(rxBuffer.data(), rxBuffer.size());
        if (bytes >= response.size()) {
            received = true;
            CHECK(rxBuffer[0] == 0x05);
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(received);
}

// ============================================================================
// Test Suite: StaticVector (Rule of Five)
// ============================================================================

#include "bcnp/static_vector.h"
#include <string>

TEST_CASE("StaticVector: Basic operations") {
    bcnp::StaticVector<int, 10> vec;
    CHECK(vec.empty());
    CHECK(vec.size() == 0);
    CHECK(vec.capacity() == 10);
    
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    
    CHECK(vec.size() == 3);
    CHECK(vec[0] == 1);
    CHECK(vec[1] == 2);
    CHECK(vec[2] == 3);
    CHECK(vec.front() == 1);
    CHECK(vec.back() == 3);
}

TEST_CASE("StaticVector: Copy constructor with non-trivial type") {
    bcnp::StaticVector<std::string, 5> original;
    original.push_back("Hello");
    original.push_back("World");
    original.push_back("Test");
    
    // Copy construct
    bcnp::StaticVector<std::string, 5> copy(original);
    
    CHECK(copy.size() == 3);
    CHECK(copy[0] == "Hello");
    CHECK(copy[1] == "World");
    CHECK(copy[2] == "Test");
    
    // Modify original - should not affect copy
    original[0] = "Modified";
    CHECK(copy[0] == "Hello");
    
    // Modify copy - should not affect original
    copy[1] = "Changed";
    CHECK(original[1] == "World");
}

TEST_CASE("StaticVector: Copy assignment with non-trivial type") {
    bcnp::StaticVector<std::string, 5> original;
    original.push_back("One");
    original.push_back("Two");
    
    bcnp::StaticVector<std::string, 5> assigned;
    assigned.push_back("Existing");
    
    // Copy assign
    assigned = original;
    
    CHECK(assigned.size() == 2);
    CHECK(assigned[0] == "One");
    CHECK(assigned[1] == "Two");
    
    // Modify original - should not affect assigned
    original[0] = "Changed";
    CHECK(assigned[0] == "One");
}

TEST_CASE("StaticVector: Move constructor with non-trivial type") {
    bcnp::StaticVector<std::string, 5> original;
    original.push_back("Move");
    original.push_back("Me");
    
    // Move construct
    bcnp::StaticVector<std::string, 5> moved(std::move(original));
    
    CHECK(moved.size() == 2);
    CHECK(moved[0] == "Move");
    CHECK(moved[1] == "Me");
    
    // Original should be empty after move
    CHECK(original.empty());
}

TEST_CASE("StaticVector: Move assignment with non-trivial type") {
    bcnp::StaticVector<std::string, 5> original;
    original.push_back("Transfer");
    
    bcnp::StaticVector<std::string, 5> target;
    target.push_back("Old");
    target.push_back("Data");
    
    // Move assign
    target = std::move(original);
    
    CHECK(target.size() == 1);
    CHECK(target[0] == "Transfer");
    CHECK(original.empty());
}

TEST_CASE("StaticVector: Self-assignment safety") {
    bcnp::StaticVector<std::string, 5> vec;
    vec.push_back("Self");
    vec.push_back("Assign");
    
    // Self copy assign
    vec = vec;
    
    CHECK(vec.size() == 2);
    CHECK(vec[0] == "Self");
    CHECK(vec[1] == "Assign");
    
    // Self move assign (technically UB for std containers, but we handle it)
    vec = std::move(vec);
    
    CHECK(vec.size() == 2);
    CHECK(vec[0] == "Self");
}

TEST_CASE("StaticVector: pop_back and resize") {
    bcnp::StaticVector<std::string, 5> vec;
    vec.push_back("A");
    vec.push_back("B");
    vec.push_back("C");
    
    vec.pop_back();
    CHECK(vec.size() == 2);
    CHECK(vec.back() == "B");
    
    vec.resize(4, "X");
    CHECK(vec.size() == 4);
    CHECK(vec[2] == "X");
    CHECK(vec[3] == "X");
    
    vec.resize(1);
    CHECK(vec.size() == 1);
    CHECK(vec[0] == "A");
}

TEST_CASE("StaticVector: at() bounds checking") {
    bcnp::StaticVector<int, 5> vec;
    vec.push_back(10);
    vec.push_back(20);
    
    CHECK(vec.at(0) == 10);
    CHECK(vec.at(1) == 20);
    
    CHECK_THROWS_AS(vec.at(2), std::out_of_range);
    CHECK_THROWS_AS(vec.at(100), std::out_of_range);
}

TEST_CASE("StaticVector: Emplace and initializer list") {
    bcnp::StaticVector<std::string, 5> vec{"First", "Second"};
    CHECK(vec.size() == 2);
    
    vec.emplace_back("Third");
    CHECK(vec.size() == 3);
    CHECK(vec[2] == "Third");
}

TEST_CASE("StaticVector: Iterator support") {
    bcnp::StaticVector<int, 5> vec{1, 2, 3, 4};
    
    int sum = 0;
    for (int v : vec) {
        sum += v;
    }
    CHECK(sum == 10);
    
    // Modify through iterator
    for (int& v : vec) {
        v *= 2;
    }
    CHECK(vec[0] == 2);
    CHECK(vec[3] == 8);
}

// Struct with destructor tracking for leak detection
struct TrackedObject {
    static int constructions;
    static int destructions;
    
    int value;
    TrackedObject(int v = 0) : value(v) { ++constructions; }
    TrackedObject(const TrackedObject& o) : value(o.value) { ++constructions; }
    TrackedObject(TrackedObject&& o) noexcept : value(o.value) { ++constructions; o.value = -1; }
    ~TrackedObject() { ++destructions; }
    
    static void reset() { constructions = destructions = 0; }
};

int TrackedObject::constructions = 0;
int TrackedObject::destructions = 0;

TEST_CASE("StaticVector: No memory leaks on destruction") {
    TrackedObject::reset();
    
    {
        bcnp::StaticVector<TrackedObject, 10> vec;
        vec.emplace_back(1);
        vec.emplace_back(2);
        vec.emplace_back(3);
    }
    
    CHECK(TrackedObject::constructions == TrackedObject::destructions);
}

TEST_CASE("StaticVector: No memory leaks on copy") {
    TrackedObject::reset();
    
    {
        bcnp::StaticVector<TrackedObject, 10> original;
        original.emplace_back(1);
        original.emplace_back(2);
        
        bcnp::StaticVector<TrackedObject, 10> copy(original);
    }
    
    CHECK(TrackedObject::constructions == TrackedObject::destructions);
}

TEST_CASE("StaticVector: No memory leaks on move") {
    TrackedObject::reset();
    
    {
        bcnp::StaticVector<TrackedObject, 10> original;
        original.emplace_back(1);
        original.emplace_back(2);
        
        bcnp::StaticVector<TrackedObject, 10> moved(std::move(original));
    }
    
    CHECK(TrackedObject::constructions == TrackedObject::destructions);
}

TEST_CASE("StaticVector: No memory leaks on clear") {
    TrackedObject::reset();
    
    bcnp::StaticVector<TrackedObject, 10> vec;
    vec.emplace_back(1);
    vec.emplace_back(2);
    vec.emplace_back(3);
    
    vec.clear();
    
    CHECK(TrackedObject::constructions == TrackedObject::destructions);
    CHECK(vec.empty());
}
