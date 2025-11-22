#include "bcnp/command_queue.h"

#include <utility>

namespace bcnp {

CommandQueue::CommandQueue(QueueConfig config) : m_config(config) {
    ClampConfig();
    m_storage.resize(m_config.capacity);
}

void CommandQueue::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ClearUnlocked();
}

bool CommandQueue::Push(const Command& command) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!PushUnlocked(command)) {
        ++m_metrics.queueOverflows;
        return false;
    }
    return true;
}

std::size_t CommandQueue::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_count;
}

void CommandQueue::NotifyPacketReceived(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastRx = now;
    ++m_metrics.packetsReceived;
}

void CommandQueue::Update(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!IsConnectedUnlocked(now)) {
        ClearUnlocked();
        m_hasVirtualCursor = false;
        return;
    }

    if (m_active) {
        const auto elapsed = now - m_active->start;
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        if (duration.count() >= m_active->command.durationMs) {
            const auto endTime = m_active->start + std::chrono::milliseconds(m_active->command.durationMs);
            m_active.reset();
            m_virtualCursor = endTime;
            m_hasVirtualCursor = true;
        }
    }

    if (!m_active) {
        PromoteNext(now);
    }
}

std::optional<Command> CommandQueue::ActiveCommand() const {
    // Try-lock to avoid priority inversion in real-time control loops
    std::unique_lock<std::mutex> lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        // Network thread holds mutex - return previous command to avoid blocking
        return std::nullopt;
    }
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
    ClampConfig();
    if (m_storage.size() != m_config.capacity) {
        // Resizing queue clears it for safety
        ClearUnlocked();
        m_storage.resize(m_config.capacity);
    }
}

QueueConfig CommandQueue::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void CommandQueue::PromoteNext(Clock::time_point now) {
    if (!m_hasVirtualCursor || m_virtualCursor == Clock::time_point::min()) {
        m_virtualCursor = now;
        m_hasVirtualCursor = true;
    }

    if (m_count == 0) {
        m_virtualCursor = std::max(m_virtualCursor, now);
        return;
    }

    const auto lagFloor = now - m_config.maxCommandLag;

    while (m_count > 0) {
        const Command next = FrontUnlocked();
        const auto duration = std::chrono::milliseconds(next.durationMs);
        auto projectedStart = m_virtualCursor;
        auto projectedEnd = projectedStart + duration;

        if (projectedEnd <= lagFloor) {
            PopUnlocked();
            m_virtualCursor = projectedEnd;
            ++m_metrics.commandsSkipped;
            continue;
        }

        if (projectedStart < lagFloor) {
            projectedStart = lagFloor;
            projectedEnd = projectedStart + duration;
        }

        m_active = ActiveSlot{next, projectedStart};
        PopUnlocked();
        m_virtualCursor = projectedEnd;
        return;
    }
}

void CommandQueue::ClearUnlocked() {
    m_head = 0;
    m_tail = 0;
    m_count = 0;
    m_active.reset();
    m_virtualCursor = Clock::time_point::min();
    m_hasVirtualCursor = false;
}

void CommandQueue::PopUnlocked() {
    if (m_count == 0) {
        return;
    }
    m_head = (m_head + 1) % Capacity();
    --m_count;
}

const Command& CommandQueue::FrontUnlocked() const {
    return m_storage[m_head];
}

void CommandQueue::ClampConfig() {
    if (m_config.capacity == 0) {
        m_config.capacity = 200;
    }
    if (m_config.maxCommandLag <= std::chrono::milliseconds::zero()) {
        m_config.maxCommandLag = std::chrono::milliseconds(1);
    }
}

} // namespace bcnp
