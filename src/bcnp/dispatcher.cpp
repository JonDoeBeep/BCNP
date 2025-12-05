/**
 * @file dispatcher.cpp
 * @brief Implementation of the BCNP packet dispatcher.
 * 
 * Routes parsed packets to registered message type handlers and manages
 * connection state tracking. Thread-safe for concurrent PushBytes() calls.
 */

#include "bcnp/dispatcher.h"

namespace bcnp {

/**
 * @brief Construct a dispatcher with the given configuration.
 * 
 * Creates internal StreamParser with callbacks wired to HandlePacket/HandleError.
 * 
 * @param config Dispatcher configuration (buffer size, timeouts)
 */
PacketDispatcher::PacketDispatcher(DispatcherConfig config)
    : m_config(config),
      m_parser(
          [this](const PacketView& packet) { HandlePacket(packet); },
          [this](const StreamParser::ErrorInfo& error) { HandleError(error); },
          m_config.parserBufferSize) {}

/**
 * @brief Push raw bytes for parsing and dispatch.
 * 
 * Thread-safe: acquires mutex before accessing parser.
 * Parsed packets are dispatched to registered handlers synchronously.
 * 
 * @param data Pointer to incoming byte data
 * @param length Number of bytes to process
 */
void PacketDispatcher::PushBytes(const uint8_t* data, std::size_t length) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_parser.Push(data, length);
}

/**
 * @brief Register a handler for a specific message type.
 * 
 * @param typeId Message type ID to handle
 * @param handler Callback to invoke when packets of this type arrive
 */
void PacketDispatcher::RegisterHandler(MessageTypeId typeId, PacketHandler handler) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handlers[static_cast<uint16_t>(typeId)] = std::move(handler);
}

/**
 * @brief Remove a previously registered handler.
 * @param typeId Message type ID to stop handling
 */
void PacketDispatcher::UnregisterHandler(MessageTypeId typeId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handlers.erase(static_cast<uint16_t>(typeId));
}

/**
 * @brief Set the error callback for parse failures.
 * @param handler Callback to invoke on stream parsing errors
 */
void PacketDispatcher::SetErrorHandler(ErrorHandler handler) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_errorHandler = std::move(handler);
}

/**
 * @brief Check if the connection is active.
 * 
 * Returns true if a packet was received within the configured timeout period.
 * 
 * @param now Current time point for comparison
 * @return true if recently received packets, false if timed out
 */
bool PacketDispatcher::IsConnected(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastRx == Clock::time_point::min()) {
        return false;
    }
    return (now - m_lastRx) <= m_config.connectionTimeout;
}

/**
 * @brief Get the timestamp of the last received packet.
 * @return Time point of last successful packet reception
 */
PacketDispatcher::Clock::time_point PacketDispatcher::LastReceiveTime() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastRx;
}

/**
 * @brief Get the total number of parse errors encountered.
 * @return Cumulative count of parse errors since creation
 */
uint64_t PacketDispatcher::ParseErrorCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_parseErrors;
}

/**
 * @brief Internal handler for successfully parsed packets.
 * 
 * Updates last receive time and dispatches to registered handler if one exists.
 * Unknown message types are silently ignored (no handler registered).
 * 
 * @param packet The validated packet view
 */
void PacketDispatcher::HandlePacket(const PacketView& packet) {
    m_lastRx = Clock::now();

    auto it = m_handlers.find(static_cast<uint16_t>(packet.header.messageType));
    if (it != m_handlers.end()) {
        it->second(packet);
    }
    // Unknown message types are silently ignored (no handler registered)
}

/**
 * @brief Internal handler for parse errors.
 * 
 * Increments error counter and invokes user error callback if set.
 * 
 * @param error Error information from the stream parser
 */
void PacketDispatcher::HandleError(const StreamParser::ErrorInfo& error) {
    ++m_parseErrors;
    if (m_errorHandler) {
        m_errorHandler(error);
    }
}

} // namespace bcnp

