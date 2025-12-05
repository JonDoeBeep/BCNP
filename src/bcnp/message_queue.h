#pragma once

/**
 * @file message_queue.h
 * @brief Timed message queue for executing duration-based commands.
 * 
 * Provides a generic queue that manages timed execution of messages,
 * ensuring each message runs for its specified duration before advancing
 * to the next. Handles connection timeouts, lag compensation, and queue
 * overflow scenarios.
 * 
 * All public methods use mutex synchronization.
 */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace bcnp {

/**
 * @brief Configuration parameters for a message queue.
 */
struct MessageQueueConfig {
    std::size_t capacity{200};                          ///< Maximum messages in queue
    std::chrono::milliseconds connectionTimeout{200};    ///< Time before declaring disconnect
    std::chrono::milliseconds maxCommandLag{100};        ///< Max lag before clamping virtual time
};

/**
 * @brief Runtime metrics for queue diagnostics.
 */
struct MessageQueueMetrics {
    uint64_t messagesReceived{0};    ///< Total messages pushed to queue
    uint64_t queueOverflows{0};      ///< Push attempts when queue was full
    uint64_t messagesSkipped{0};     ///< Messages skipped due to lag compensation
};

/**
 * @brief Type trait to detect messages with a durationMs field.
 * 
 * Messages used with MessageQueue must have a uint16_t durationMs field
 * that specifies how long the message should be "active" in milliseconds.
 * 
 * @tparam T The type to check
 */
template<typename T>
struct HasDurationMs {
    template<typename U>
    static auto test(int) -> decltype(std::declval<U>().durationMs, std::true_type{});
    template<typename>
    static std::false_type test(...);
    static constexpr bool value = decltype(test<T>(0))::value;
};

/**
 * @brief Generic timed message queue for any message type with durationMs field.
 * 
 * This queue manages timed execution of messages, ensuring each message runs
 * for its specified duration before the next one starts. It handles connection timeouts, lag compensation
 * 
 * @tparam MsgType Message struct with a uint16_t durationMs field
 * 
 * @code{cpp}
 *   MessageQueue<DriveCmd> driveQueue;
 *   MessageQueue<ElevatorCmd> elevatorQueue;
 * 
 *   // In network handler:
 *   driveQueue.Push(cmd);
 *   driveQueue.NotifyReceived(now);
 * 
 *   // In periodic loop:
 *   driveQueue.Update(now);
 *   if (auto cmd = driveQueue.ActiveMessage()) {
 *       drivetrain.execute(*cmd);
 *   }
 * @endcode
 */
template<typename MsgType>
class MessageQueue {
    static_assert(HasDurationMs<MsgType>::value, 
        "MsgType must have a uint16_t durationMs field");

public:
    using Clock = std::chrono::steady_clock;

    explicit MessageQueue(MessageQueueConfig config = {})
        : m_config(config) {
        ClampConfig();
        m_storage.resize(m_config.capacity);
    }

    /**
     * @brief Remove all messages from the queue.
     * 
     * Also clears the active message and resets virtual cursor.
     * Thread-safe.
     */
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        ClearUnlocked();
    }

    /**
     * @brief Add a message to the back of the queue.
     * 
     * @param message The message to enqueue
     * @return true if successfully added, false if queue was full
     * 
     * @note Increments queueOverflows metric on failure.
     */
    bool Push(const MsgType& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!PushUnlocked(message)) {
            ++m_metrics.queueOverflows;
            return false;
        }
        ++m_metrics.messagesReceived;
        return true;
    }

    /**
     * @brief Get the current number of queued messages.
     * @return Number of messages waiting (excludes active message)
     */
    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_count;
    }

    /**
     * @brief Notify that messages were received from the network.
     * 
     * Call this after pushing messages to update the connection timeout.
     * The queue will clear itself if no notifications occur within
     * the configured connectionTimeout period.
     * 
     * @param now Current timestamp (typically steady_clock::now())
     */
    void NotifyReceived(Clock::time_point now) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastRx = now;
    }

    /**
     * @brief Update queue state - call once per control loop iteration.
     * 
     * Performs the following:
     * Checks connection timeout - clears queue if timed out
     * Checks if active message duration has elapsed
     * Promotes next message from queue if ready
     * Handles lag compensation by skipping stale messages
     * 
     * @param now Current timestamp for timing calculations
     */
    void Update(Clock::time_point now) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsConnectedUnlocked(now)) {
            ClearUnlocked();
            m_hasVirtualCursor = false;
            return;
        }

        while (true) {
            if (m_active) {
                const auto elapsed = now - m_active->start;
                const auto duration = std::chrono::milliseconds(m_active->message.durationMs);
                
                if (elapsed < duration) {
                    break;
                }

                const auto endTime = m_active->start + duration;
                m_active.reset();
                m_virtualCursor = endTime;
                m_hasVirtualCursor = true;
            }

            if (!m_active) {
                PromoteNext(now);
                if (!m_active) {
                    break;
                }
            }
        }
    }

    /**
     * @brief Get the currently executing message.
     * 
     * Returns the message whose duration is currently being executed.
     * Blocks briefly if mutex is held by network thread (typically <5µs).
     * 
     * @return The active message, or nullopt if none active
     */
    std::optional<MsgType> ActiveMessage() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active) {
            return m_active->message;
        }
        return std::nullopt;
    }

    /**
     * @brief Check if the connection is still active.
     * 
     * @param now Current timestamp for timeout calculation
     * @return true if packets received within connectionTimeout, false otherwise
     */
    bool IsConnected(Clock::time_point now) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return IsConnectedUnlocked(now);
    }

    /**
     * @brief Get current queue metrics.
     * @return Snapshot of queue statistics
     */
    MessageQueueMetrics GetMetrics() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_metrics;
    }

    /**
     * @brief Reset all metrics to zero.
     */
    void ResetMetrics() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics = {};
    }

    /**
     * @brief Update queue configuration.
     * 
     * @note Clears the queue if capacity changes.
     * 
     * @param config New configuration to apply
     */
    void SetConfig(const MessageQueueConfig& config) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
        ClampConfig();
        if (m_storage.size() != m_config.capacity) {
            ClearUnlocked();
            m_storage.resize(m_config.capacity);
        }
    }

    MessageQueueConfig GetConfig() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config;
    }

    /**
     * @brief RAII transaction for atomic batch operations.
     * 
     * Holds the queue mutex for the lifetime of the transaction,
     * allowing multiple Push() or Clear() calls without interleaving.
     * 
     * @code{cpp}
     * {
     *     auto tx = queue.BeginTransaction();
     *     tx.Clear();
     *     for (const auto& cmd : commands) {
     *         tx.Push(cmd);
     *     }
     * } // Lock released here
     * @endcode
     */
    class Transaction {
    public:
        explicit Transaction(MessageQueue& queue) 
            : m_queue(queue), m_lock(queue.m_mutex) {}
        
        bool Push(const MsgType& message) {
            if (!m_queue.PushUnlocked(message)) {
                ++m_queue.m_metrics.queueOverflows;
                return false;
            }
            ++m_queue.m_metrics.messagesReceived;
            return true;
        }

        void Clear() {
            m_queue.ClearUnlocked();
        }

    private:
        MessageQueue& m_queue;
        std::lock_guard<std::mutex> m_lock;
    };

    Transaction BeginTransaction() { return Transaction(*this); }

private:
    struct ActiveSlot {
        MsgType message;
        Clock::time_point start;
    };

    void PromoteNext(Clock::time_point now) {
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
            const MsgType& next = FrontUnlocked();
            const auto duration = std::chrono::milliseconds(next.durationMs);
            auto projectedStart = m_virtualCursor;
            auto projectedEnd = projectedStart + duration;

            if (projectedEnd <= lagFloor) {
                PopUnlocked();
                m_virtualCursor = projectedEnd;
                ++m_metrics.messagesSkipped;
                continue;
            }

            if (projectedStart < lagFloor) {
                projectedStart = lagFloor;
            }

            m_active = ActiveSlot{next, projectedStart};
            PopUnlocked();
            m_virtualCursor = projectedStart + duration;
            return;
        }
    }

    void ClearUnlocked() {
        m_head = 0;
        m_tail = 0;
        m_count = 0;
        m_active.reset();
        m_virtualCursor = Clock::time_point::min();
        m_hasVirtualCursor = false;
    }
    
    bool PushUnlocked(const MsgType& message) {
        if (m_count >= EffectiveDepth()) {
            return false;
        }
        m_storage[m_tail] = message;
        m_tail = (m_tail + 1) % Capacity();
        ++m_count;
        return true;
    }

    void PopUnlocked() {
        if (m_count == 0) return;
        m_head = (m_head + 1) % Capacity();
        --m_count;
    }

    const MsgType& FrontUnlocked() const {
        return m_storage[m_head];
    }
    
    std::size_t Capacity() const { return m_storage.size(); }
    std::size_t EffectiveDepth() const { return std::min(m_config.capacity, Capacity()); }
    
    void ClampConfig() {
        if (m_config.capacity == 0) {
            m_config.capacity = 200;
        }
        if (m_config.maxCommandLag <= std::chrono::milliseconds::zero()) {
            m_config.maxCommandLag = std::chrono::milliseconds(1);
        }
    }

    bool IsConnectedUnlocked(Clock::time_point now) const {
        if (m_lastRx == Clock::time_point::min()) {
            return false;
        }
        return (now - m_lastRx) <= m_config.connectionTimeout;
    }

    MessageQueueConfig m_config{};
    MessageQueueMetrics m_metrics{};
    std::vector<MsgType> m_storage;
    std::size_t m_head{0};
    std::size_t m_tail{0};
    std::size_t m_count{0};
    std::optional<ActiveSlot> m_active;
    Clock::time_point m_virtualCursor{Clock::time_point::min()};
    bool m_hasVirtualCursor{false};
    Clock::time_point m_lastRx{Clock::time_point::min()};
    mutable std::mutex m_mutex;
};

/**
 * @brief Convenience alias for MessageQueue (backward compatibility).
 * @tparam MsgType Message type with durationMs field
 */
template<typename MsgType>
using TimedQueue = MessageQueue<MsgType>;

} // namespace bcnp