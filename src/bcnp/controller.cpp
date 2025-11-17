#include "bcnp/controller.h"

namespace bcnp {

Controller::Controller(QueueConfig config)
    : m_config(config),
      m_queue(config),
      m_parser(
          [this](const Packet& packet) { HandlePacket(packet); },
          [this](PacketError error) { ++m_queue.Metrics().parseErrors; }) {}

void Controller::PushBytes(const uint8_t* data, std::size_t length) {
    m_parser.Push(data, length);
}

void Controller::HandlePacket(const Packet& packet) {
    m_queue.NotifyPacketReceived(CommandQueue::Clock::now());

    if (packet.header.flags & kFlagClearQueue) {
        m_queue.Clear();
    }

    for (const auto& cmd : packet.commands) {
        if (!m_queue.Push(cmd)) {
            break;
        }
    }
}

std::optional<Command> Controller::CurrentCommand(CommandQueue::Clock::time_point now) {
    return m_queue.CurrentCommand(now);
}

bool Controller::IsConnected(CommandQueue::Clock::time_point now) const {
    return m_queue.IsConnected(now);
}

} // namespace bcnp
