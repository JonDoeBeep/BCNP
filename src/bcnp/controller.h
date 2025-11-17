#pragma once

#include "bcnp/command_queue.h"
#include "bcnp/stream_parser.h"

namespace bcnp {

class Controller {
public:
    explicit Controller(QueueConfig config = {});

    void PushBytes(const uint8_t* data, std::size_t length);

    void HandlePacket(const Packet& packet);

    std::optional<Command> CurrentCommand(CommandQueue::Clock::time_point now);

    bool IsConnected(CommandQueue::Clock::time_point now) const;

    CommandQueue& Queue() { return m_queue; }
    const CommandQueue& Queue() const { return m_queue; }

    StreamParser& Parser() { return m_parser; }
    const StreamParser& Parser() const { return m_parser; }

private:
    QueueConfig m_config;
    CommandQueue m_queue;
    StreamParser m_parser;
};

} // namespace bcnp
