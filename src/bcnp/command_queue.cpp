#include "bcnp/command_queue.h"

#include <utility>

namespace bcnp {

CommandQueue::CommandQueue(QueueConfig config) : m_config(config) {}

void CommandQueue::Clear() {
    std::queue<Command> empty;
    std::swap(m_queue, empty);
    m_active.reset();
}

bool CommandQueue::Push(const Command& command) {
    if (m_queue.size() >= m_config.maxQueueDepth) {
        ++m_metrics.queueOverflows;
        return false;
    }
    m_queue.push(command);
    return true;
}

void CommandQueue::NotifyPacketReceived(Clock::time_point now) {
    m_lastRx = now;
    ++m_metrics.packetsReceived;
}

void CommandQueue::Update(Clock::time_point now) {
    if (m_active) {
        const auto elapsed = now - m_active->start;
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        if (duration.count() >= m_active->command.durationMs) {
            m_active.reset();
        }
    }

    if (!m_active) {
        PromoteNext(now);
    }
}

std::optional<Command> CommandQueue::ActiveCommand() const {
    if (m_active) {
        return m_active->command;
    }
    return std::nullopt;
}

bool CommandQueue::IsConnected(Clock::time_point now) const {
    if (m_lastRx == Clock::time_point::min()) {
        return false;
    }
    const auto elapsed = now - m_lastRx;
    return elapsed <= m_config.connectionTimeout;
}

void CommandQueue::PromoteNext(Clock::time_point now) {
    if (m_queue.empty()) {
        return;
    }
    m_active = ActiveSlot{m_queue.front(), now};
    m_queue.pop();
}

} // namespace bcnp
