#include "bcnp/command_queue.h"

#include <utility>

namespace bcnp {

CommandQueue::CommandQueue(QueueConfig config) : m_config(config) {}

void CommandQueue::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::queue<Command> empty;
    std::swap(m_queue, empty);
    m_active.reset();
}

bool CommandQueue::Push(const Command& command) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.size() >= m_config.maxQueueDepth) {
        ++m_metrics.queueOverflows;
        return false;
    }
    m_queue.push(command);
    return true;
}

std::size_t CommandQueue::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

void CommandQueue::NotifyPacketReceived(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastRx = now;
    ++m_metrics.packetsReceived;
}

void CommandQueue::Update(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!IsConnectedUnlocked(now)) {
        // Inline Clear() to avoid deadlock (we already hold mutex)
        std::queue<Command> empty;
        std::swap(m_queue, empty);
        m_active.reset();
        return;
    }

    if (m_active) {
        const auto elapsed = now - m_active->start;
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        if (duration.count() >= m_active->command.durationMs) {
            // Calculate the virtual end time of the current command to prevent drift
            const auto endTime = m_active->start + std::chrono::milliseconds(m_active->command.durationMs);
            m_active.reset();
            PromoteNext(endTime, now);
        }
    }

    if (!m_active) {
        PromoteNext(now, now);
    }
}

std::optional<Command> CommandQueue::ActiveCommand() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_active) {
        return m_active->command;
    }
    return std::nullopt;
}

bool CommandQueue::IsConnected(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return IsConnectedUnlocked(now);
}

bool CommandQueue::IsConnectedUnlocked(Clock::time_point now) const {
    if (m_lastRx == Clock::time_point::min()) {
        return false;
    }
    const auto elapsed = now - m_lastRx;
    return elapsed <= m_config.connectionTimeout;
}

void CommandQueue::SetMetrics(const QueueMetrics& metrics) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_metrics = metrics;
}

QueueMetrics CommandQueue::GetMetrics() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_metrics;
}

void CommandQueue::IncrementParseErrors() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_metrics.parseErrors;
}

void CommandQueue::SetConfig(const QueueConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
}

QueueConfig CommandQueue::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void CommandQueue::PromoteNext(Clock::time_point startTime, Clock::time_point now) {
    if (m_queue.empty()) {
        return;
    }
    
    // Lag compensation: Prevents "fast-forwarding" through buffered commands after system pause.
    const auto maxLagTime = now - m_config.maxCommandLag;
    
    Clock::time_point effectiveStart = startTime;
    if (startTime < maxLagTime) {
        effectiveStart = maxLagTime;
    }
    
    m_active = ActiveSlot{m_queue.front(), effectiveStart};
    m_queue.pop();
}

} // namespace bcnp
