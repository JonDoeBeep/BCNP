#include "bcnp/controller.h"

namespace bcnp {

Controller::Controller(ControllerConfig config)
        : m_config(config),
            m_queue(m_config.queue),
            m_parser(
                    [this](const Packet& packet) { HandlePacket(packet); },
                    [this](const StreamParser::ErrorInfo&) { ++m_queue.Metrics().parseErrors; }) {}

void Controller::PushBytes(const uint8_t* data, std::size_t length) {
    m_parser.Push(data, length);
}

void Controller::HandlePacket(const Packet& packet) {
    m_queue.NotifyPacketReceived(CommandQueue::Clock::now());

    if (packet.header.flags & kFlagClearQueue) {
        m_queue.Clear();
    }

    for (const auto& cmd : packet.commands) {
        const auto clamped = ClampCommand(cmd);
        if (!m_queue.Push(clamped)) {
            break;
        }
    }
}

std::optional<Command> Controller::CurrentCommand(CommandQueue::Clock::time_point now) {
    m_queue.Update(now);
    return m_queue.ActiveCommand();
}

bool Controller::IsConnected(CommandQueue::Clock::time_point now) const {
    return m_queue.IsConnected(now);
}

Command Controller::ClampCommand(const Command& cmd) const {
    Command clamped = cmd;
    const auto& limits = m_config.limits;

    if (clamped.vx < limits.vxMin) {
        clamped.vx = limits.vxMin;
    } else if (clamped.vx > limits.vxMax) {
        clamped.vx = limits.vxMax;
    }

    if (clamped.omega < limits.omegaMin) {
        clamped.omega = limits.omegaMin;
    } else if (clamped.omega > limits.omegaMax) {
        clamped.omega = limits.omegaMax;
    }

    if (clamped.durationMs < limits.durationMin) {
        clamped.durationMs = limits.durationMin;
    } else if (clamped.durationMs > limits.durationMax) {
        clamped.durationMs = limits.durationMax;
    }

    return clamped;
}

} // namespace bcnp
