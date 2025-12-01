#include "bcnp/controller.h"

namespace bcnp {

Controller::Controller(ControllerConfig config)
        : m_config(config),
            m_queue(m_config.queue),
            m_parser(
                    [this](const PacketView& packet) { HandlePacket(packet); },
                    [this](const StreamParser::ErrorInfo&) { m_queue.IncrementParseErrors(); },
                    m_config.parserBufferSize) {}

void Controller::PushBytes(const uint8_t* data, std::size_t length) {
    std::lock_guard<std::mutex> lock(m_parserMutex);
    m_parser.Push(data, length);
}

void Controller::HandlePacket(const PacketView& packet) {
    m_queue.NotifyPacketReceived(CommandQueue::Clock::now());

    // Check for registered handler for this message type
    auto it = m_handlers.find(static_cast<uint16_t>(packet.header.messageType));
    if (it != m_handlers.end()) {
        it->second(packet);
        return;
    }

    // Default handling for DriveCmd (backwards compatibility)
    if (packet.header.messageType != MessageTypeId::DriveCmd) {
        // Unknown message type with no handler - ignore
        return;
    }

    auto txn = m_queue.BeginTransaction();

    if (packet.header.flags & kFlagClearQueue) {
        txn.Clear();
    }

    for (const auto& cmd : packet) {
        const auto clamped = ClampCommand(cmd);
        if (!txn.Push(clamped)) {
            break;
        }
    }
}

void Controller::RegisterHandler(MessageTypeId typeId, MessageHandler handler) {
    std::lock_guard<std::mutex> lock(m_parserMutex);
    m_handlers[static_cast<uint16_t>(typeId)] = std::move(handler);
}

void Controller::UnregisterHandler(MessageTypeId typeId) {
    std::lock_guard<std::mutex> lock(m_parserMutex);
    m_handlers.erase(static_cast<uint16_t>(typeId));
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
