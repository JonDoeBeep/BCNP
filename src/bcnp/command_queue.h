#pragma once

#include "bcnp/packet.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <queue>

namespace bcnp {

struct QueueConfig {
    std::size_t maxQueueDepth{kMaxQueueSize};
    std::chrono::milliseconds connectionTimeout{200};
    std::chrono::milliseconds maxCommandLag{100}; // Max lag before clamping virtual time
};

struct QueueMetrics {
    uint64_t packetsReceived{0};
    uint64_t parseErrors{0};
    uint64_t queueOverflows{0};
};

class CommandQueue {
public:
    using Clock = std::chrono::steady_clock;

    explicit CommandQueue(QueueConfig config = {});

    void Clear();
    bool Push(const Command& command);
    std::size_t Size() const { return m_queue.size(); }

    void NotifyPacketReceived(Clock::time_point now);

    void Update(Clock::time_point now);

    std::optional<Command> ActiveCommand() const;

    bool IsConnected(Clock::time_point now) const;

    void SetMetrics(QueueMetrics metrics) { m_metrics = metrics; }
    QueueMetrics& Metrics() { return m_metrics; }
    const QueueMetrics& Metrics() const { return m_metrics; }

    QueueConfig& Config() { return m_config; }
    const QueueConfig& Config() const { return m_config; }

private:
    struct ActiveSlot {
        Command command;
        Clock::time_point start;
    };

    void PromoteNext(Clock::time_point startTime, Clock::time_point now);

    QueueConfig m_config{};
    QueueMetrics m_metrics{};
    std::queue<Command> m_queue;
    std::optional<ActiveSlot> m_active;
    Clock::time_point m_lastRx{Clock::time_point::min()};
};

} // namespace bcnp
