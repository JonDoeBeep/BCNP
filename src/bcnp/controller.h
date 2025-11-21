#pragma once

#include "bcnp/command_queue.h"
#include "bcnp/stream_parser.h"
#include <cstdint>
#include <mutex>

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
};

class Controller {
public:
    explicit Controller(ControllerConfig config = {});

    // Thread-safe: can be called from network receive thread
    void PushBytes(const uint8_t* data, std::size_t length);

    void HandlePacket(const Packet& packet);

    std::optional<Command> CurrentCommand(CommandQueue::Clock::time_point now);

    bool IsConnected(CommandQueue::Clock::time_point now) const;

    CommandQueue& Queue() { return m_queue; }
    const CommandQueue& Queue() const { return m_queue; }

    StreamParser& Parser() { return m_parser; }
    const StreamParser& Parser() const { return m_parser; }

private:
    Command ClampCommand(const Command& cmd) const;

    ControllerConfig m_config;
    CommandQueue m_queue;
    StreamParser m_parser;
    mutable std::mutex m_parserMutex; // Protects m_parser from concurrent PushBytes calls
};

} // namespace bcnp
