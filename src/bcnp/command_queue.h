#pragma once

#include "bcnp/packet.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>

namespace bcnp {

struct QueueConfig {
    std::size_t capacity{200};
    std::chrono::milliseconds connectionTimeout{200};
    std::chrono::milliseconds maxCommandLag{100}; // Max lag before clamping virtual time
};

struct QueueMetrics {
    uint64_t packetsReceived{0};
    uint64_t parseErrors{0};
    uint64_t queueOverflows{0};
    uint64_t commandsSkipped{0};
};

class CommandQueue {
public:
    using Clock = std::chrono::steady_clock;

    explicit CommandQueue(QueueConfig config = {});

    void Clear();
    bool Push(const Command& command);
    std::size_t Size() const;

    void NotifyPacketReceived(Clock::time_point now);

    void Update(Clock::time_point now);

    std::optional<Command> ActiveCommand() const;

    bool IsConnected(Clock::time_point now) const;

    void SetMetrics(const QueueMetrics& metrics);
    QueueMetrics GetMetrics() const;
    void IncrementParseErrors();

    void SetConfig(const QueueConfig& config);
    QueueConfig GetConfig() const;

    class Transaction {
    public:
        explicit Transaction(CommandQueue& queue) : m_queue(queue), m_lock(queue.m_mutex) {}
        
        bool Push(const Command& command) {
            if (!m_queue.PushUnlocked(command)) {
                m_queue.IncrementQueueOverflows();
                return false;
            }
            return true;
        }

        void Clear() {
            m_queue.ClearUnlocked();
        }

    private:
        CommandQueue& m_queue;
        std::lock_guard<std::mutex> m_lock;
    };

    Transaction BeginTransaction() { return Transaction(*this); }

private:
    struct ActiveSlot {
        Command command;
        Clock::time_point start;
    };

    void PromoteNext(Clock::time_point now);
    void ClearUnlocked();
    
    // Inline for performance and Transaction access
    bool PushUnlocked(const Command& command) {
        if (m_count >= EffectiveDepth()) {
            return false;
        }
        m_storage[m_tail] = command;
        m_tail = (m_tail + 1) % Capacity();
        ++m_count;
        return true;
    }

    void PopUnlocked();
    const Command& FrontUnlocked() const;
    
    std::size_t Capacity() const { return m_storage.size(); }
    std::size_t EffectiveDepth() const { return std::min(m_config.capacity, Capacity()); }
    
    void ClampConfig();
    bool IsConnectedUnlocked(Clock::time_point now) const;
    void IncrementQueueOverflows() { ++m_metrics.queueOverflows; }

    QueueConfig m_config{};
    QueueMetrics m_metrics{};
    std::vector<Command> m_storage;
    std::size_t m_head{0};
    std::size_t m_tail{0};
    std::size_t m_count{0};
    std::optional<ActiveSlot> m_active;
    Clock::time_point m_virtualCursor{Clock::time_point::min()};
    bool m_hasVirtualCursor{false};
    Clock::time_point m_lastRx{Clock::time_point::min()};
    mutable std::mutex m_mutex;
};

} // namespace bcnp
