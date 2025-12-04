#pragma once

#include "bcnp/packet.h"
#include "bcnp/packet_storage.h"
#include "bcnp/static_vector.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace bcnp {

/**
 * @brief Configuration for telemetry accumulator.
 */
struct TelemetryAccumulatorConfig {
    /// Flush interval: send telemetry every N control loop ticks
    /// Default: 2 ticks = 25Hz telemetry at 50Hz control loop
    std::size_t flushIntervalTicks{2};
    
    /// Maximum messages to accumulate before forcing a flush
    /// Default: 64 (matches StaticVector default capacity)
    std::size_t maxBufferedMessages{64};
};

/**
 * @brief Accumulates high-frequency telemetry data and batches into packets.
 * 
 * The accumulator collects sensor/state readings during the control loop and 
 * sends them as batched BCNP packets at a configurable rate. This avoids 
 * the overhead of a send() syscall per reading.
 * 
 * Design principles:
 * - **Absolute snapshots**: Always send current state, not deltas. Self-correcting if packets drop.
 * - **Latest-wins semantics**: Dashboard grabs messages.back(). Loggers iterate all.
 * - **Real-time safe**: Uses StaticVector by default (no heap allocation in control loop).
 * 
 * @tparam MsgType The message struct type (e.g., DrivetrainState, EncoderData)
 * @tparam Storage Container type (default: StaticVector<MsgType, 64>)
 * 
 * Usage (robot side):
 * @code
 *   TelemetryAccumulator<DrivetrainState> drivetrainTelem;
 *   
 *   // In TeleopPeriodic (50Hz):
 *   drivetrainTelem.Record(DrivetrainState{
 *       .vxActual = drivetrain.GetVelocity(),
 *       .omegaActual = drivetrain.GetAngularVelocity(),
 *       .leftPos = drivetrain.GetLeftEncoder(),
 *       .rightPos = drivetrain.GetRightEncoder(),
 *       .timestampMs = static_cast<uint32_t>(Timer::GetFPGATimestamp() * 1000)
 *   });
 *   
 *   // At end of Periodic:
 *   drivetrainTelem.MaybeFlush(tcpAdapter);  // Sends every N ticks
 * @endcode
 */
template<typename MsgType, typename Storage = StaticVector<MsgType, 64>>
class TelemetryAccumulator {
public:
    using Clock = std::chrono::steady_clock;

    explicit TelemetryAccumulator(TelemetryAccumulatorConfig config = {})
        : m_config(config) {}

    /**
     * @brief Record a telemetry reading.
     * 
     * Call this for each sensor/state update during the control loop.
     * If the buffer is full, the oldest reading is discarded (ring behavior via clear+re-add).
     * 
     * @param msg The telemetry message to record
     * @return true if recorded successfully, false if buffer was full (still recorded, but overwrote)
     */
    bool Record(const MsgType& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // If at capacity, we need to make room
        // For real-time, we just clear and start fresh (latest-wins philosophy)
        if (m_buffer.size() >= m_config.maxBufferedMessages) {
            // Keep the most recent half to preserve some history
            // Or just clear - simpler and fits "latest-wins" better
            m_buffer.clear();
            ++m_metrics.bufferOverflows;
        }
        
        m_buffer.push_back(msg);
        ++m_metrics.messagesRecorded;
        return true;
    }

    /**
     * @brief Record multiple telemetry readings at once.
     * 
     * Useful for batching multiple encoder readings in a single call.
     */
    template<typename InputIt>
    void RecordBatch(InputIt first, InputIt last) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = first; it != last; ++it) {
            if (m_buffer.size() >= m_config.maxBufferedMessages) {
                m_buffer.clear();
                ++m_metrics.bufferOverflows;
            }
            m_buffer.push_back(*it);
            ++m_metrics.messagesRecorded;
        }
    }

    /**
     * @brief Flush if interval has elapsed.
     * 
     * Call this at the end of each control loop iteration.
     * 
     * @param adapter The transport adapter to send through (must have SendBytes)
     * @return true if a packet was sent, false if interval not yet elapsed or buffer empty
     */
    template<typename Adapter>
    bool MaybeFlush(Adapter& adapter) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        ++m_tickCount;
        if (m_tickCount < m_config.flushIntervalTicks) {
            return false;
        }
        
        m_tickCount = 0;
        return FlushUnlocked(adapter);
    }

    /**
     * @brief Force an immediate flush regardless of interval.
     * 
     * @param adapter The transport adapter to send through
     * @return true if a packet was sent, false if buffer was empty
     */
    template<typename Adapter>
    bool ForceFlush(Adapter& adapter) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tickCount = 0;
        return FlushUnlocked(adapter);
    }

    /**
     * @brief Get the number of buffered messages waiting to be sent.
     */
    std::size_t BufferedCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_buffer.size();
    }

    /**
     * @brief Clear all buffered messages without sending.
     */
    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buffer.clear();
        m_tickCount = 0;
    }

    /**
     * @brief Metrics for diagnostics.
     */
    struct Metrics {
        uint64_t messagesRecorded{0};
        uint64_t messagesSent{0};
        uint64_t packetsSent{0};
        uint64_t bufferOverflows{0};
        uint64_t sendFailures{0};
    };

    Metrics GetMetrics() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_metrics;
    }

    void ResetMetrics() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_metrics = {};
    }

    /**
     * @brief Update configuration.
     */
    void SetConfig(const TelemetryAccumulatorConfig& config) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config = config;
    }

private:
    template<typename Adapter>
    bool FlushUnlocked(Adapter& adapter) {
        if (m_buffer.empty()) {
            return false;
        }

        // Build packet from buffer
        TypedPacket<MsgType, Storage> packet;
        packet.messages = std::move(m_buffer);
        m_buffer = Storage{};  // Reset buffer
        
        // Encode to wire format
        std::vector<uint8_t> wireBuffer;
        if (!EncodeTypedPacket(packet, wireBuffer)) {
            ++m_metrics.sendFailures;
            return false;
        }

        // Send
        if (!adapter.SendBytes(wireBuffer.data(), wireBuffer.size())) {
            ++m_metrics.sendFailures;
            return false;
        }

        m_metrics.messagesSent += packet.messages.size();
        ++m_metrics.packetsSent;
        return true;
    }

    TelemetryAccumulatorConfig m_config;
    Storage m_buffer{};
    std::size_t m_tickCount{0};
    Metrics m_metrics{};
    mutable std::mutex m_mutex;
};

/**
 * @brief Convenience alias for heap-allocated accumulator (large batches).
 */
template<typename MsgType>
using DynamicTelemetryAccumulator = TelemetryAccumulator<MsgType, std::vector<MsgType>>;

/**
 * @brief Convenience alias for stack-allocated accumulator (real-time, default).
 */
template<typename MsgType, std::size_t Capacity = 64>
using StaticTelemetryAccumulator = TelemetryAccumulator<MsgType, StaticVector<MsgType, Capacity>>;

} // namespace bcnp
