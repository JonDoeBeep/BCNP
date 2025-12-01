#pragma once

#include "bcnp/command_queue.h"
#include "bcnp/stream_parser.h"
#include <bcnp/message_types.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace bcnp {

struct CommandLimits {
    float vxMin{0.0f};
    float vxMax{0.0f};
    float omegaMin{0.0f};
    float omegaMax{0.0f};
    uint16_t durationMin{0};
    uint16_t durationMax{0};
};

struct ControllerConfig {
    QueueConfig queue{};
    CommandLimits limits{};
    std::size_t parserBufferSize{4096};
};

/// Callback for handling arbitrary message types
using MessageHandler = std::function<void(const PacketView&)>;

class Controller {
public:
    explicit Controller(ControllerConfig config = {});

    // Thread-safe: can be called from network receive thread
    void PushBytes(const uint8_t* data, std::size_t length);

    void HandlePacket(const PacketView& packet);

    std::optional<Command> CurrentCommand(CommandQueue::Clock::time_point now);

    bool IsConnected(CommandQueue::Clock::time_point now) const;

    CommandQueue& Queue() { return m_queue; }
    const CommandQueue& Queue() const { return m_queue; }

    StreamParser& Parser() { return m_parser; }
    const StreamParser& Parser() const { return m_parser; }
    
    /// Register a handler for a specific message type
    void RegisterHandler(MessageTypeId typeId, MessageHandler handler);
    
    /// Remove a handler for a message type
    void UnregisterHandler(MessageTypeId typeId);

private:
    Command ClampCommand(const Command& cmd) const;

    ControllerConfig m_config;
    CommandQueue m_queue;
    StreamParser m_parser;
    mutable std::mutex m_parserMutex; // Protects m_parser from concurrent PushBytes calls
    std::unordered_map<uint16_t, MessageHandler> m_handlers;
};

} // namespace bcnp
