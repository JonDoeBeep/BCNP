#pragma once

#include "bcnp/stream_parser.h"
#include "bcnp/message_queue.h"
#include <bcnp/message_types.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace bcnp {

/// Configuration for the packet dispatcher
struct DispatcherConfig {
    std::size_t parserBufferSize{4096};
    std::chrono::milliseconds connectionTimeout{200};
};

/// Callback for handling message packets
using PacketHandler = std::function<void(const PacketView&)>;

/// Error callback for parse errors
using ErrorHandler = std::function<void(const StreamParser::ErrorInfo&)>;

/**
 * @brief Parses BCNP stream and dispatches packets to registered handlers.
 * 
 * This class handles:
 * - Stream parsing and packet validation
 * - Handshake management (for transports)
 * - Dispatching packets to type-specific handlers
 * 
 * PacketDispatcher does NOT own any queues. Robot code creates queues per 
 * subsystem and registers handlers to fill them.
 * 
 * Usage:
 *   PacketDispatcher dispatcher;
 *   MessageQueue<MyMotorCmd> motorQueue;
 *   MessageQueue<MyServoCmd> servoQueue;
 * 
 *   dispatcher.RegisterHandler<MyMotorCmd>([&](const PacketView& pkt) {
 *       for (auto it = pkt.begin_as<MyMotorCmd>(); it != pkt.end_as<MyMotorCmd>(); ++it) {
 *           motorQueue.Push(*it);
 *       }
 *       motorQueue.NotifyReceived(Clock::now());
 *   });
 * 
 *   dispatcher.RegisterHandler<MyServoCmd>([&](const PacketView& pkt) {
 *       // Handle servo commands...
 *   });
 */
class PacketDispatcher {
public:
    using Clock = std::chrono::steady_clock;

    explicit PacketDispatcher(DispatcherConfig config = {});

    /// Feed raw bytes from transport (thread-safe)
    void PushBytes(const uint8_t* data, std::size_t length);

    /// Register a handler for a message type (by type)
    template<typename MsgType>
    void RegisterHandler(PacketHandler handler) {
        RegisterHandler(MsgType::kTypeId, std::move(handler));
    }

    /// Register a handler for a message type (by ID)
    void RegisterHandler(MessageTypeId typeId, PacketHandler handler);

    /// Remove a handler
    void UnregisterHandler(MessageTypeId typeId);

    /// Set error callback
    void SetErrorHandler(ErrorHandler handler);

    /// Check if any packets received recently
    bool IsConnected(Clock::time_point now) const;

    /// Get last receive time
    Clock::time_point LastReceiveTime() const;

    /// Access the parser (for diagnostics)
    StreamParser& Parser() { return m_parser; }
    const StreamParser& Parser() const { return m_parser; }

    /// Set wire size lookup for custom message types (for testing)
    void SetWireSizeLookup(StreamParser::WireSizeLookup lookup) {
        m_parser.SetWireSizeLookup(std::move(lookup));
    }
    
    /// Convenience: set wire size lookup from a list of message types
    template<typename... MsgTypes>
    void RegisterMessageTypes() {
        m_parser.SetWireSizeLookup([](MessageTypeId typeId) -> std::size_t {
            return GetWireSizeFor<MsgTypes...>(typeId);
        });
    }

    /// Get parse error count
    uint64_t ParseErrorCount() const;

private:
    template<typename First, typename... Rest>
    static std::size_t GetWireSizeFor(MessageTypeId typeId) {
        if (typeId == First::kTypeId) {
            return First::kWireSize;
        }
        if constexpr (sizeof...(Rest) > 0) {
            return GetWireSizeFor<Rest...>(typeId);
        }
        return 0;
    }

    void HandlePacket(const PacketView& packet);
    void HandleError(const StreamParser::ErrorInfo& error);

    DispatcherConfig m_config;
    StreamParser m_parser;
    mutable std::mutex m_mutex;
    std::unordered_map<uint16_t, PacketHandler> m_handlers;
    ErrorHandler m_errorHandler;
    Clock::time_point m_lastRx{Clock::time_point::min()};
    uint64_t m_parseErrors{0};
};

// Type alias for backward compatibility
using MessageHandler = PacketHandler;

} // namespace bcnp
