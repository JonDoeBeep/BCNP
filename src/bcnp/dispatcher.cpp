#include "bcnp/dispatcher.h"

namespace bcnp {

// ============================================================================
// PacketDispatcher Implementation
// ============================================================================

PacketDispatcher::PacketDispatcher(DispatcherConfig config)
    : m_config(config),
      m_parser(
          [this](const PacketView& packet) { HandlePacket(packet); },
          [this](const StreamParser::ErrorInfo& error) { HandleError(error); },
          m_config.parserBufferSize) {}

void PacketDispatcher::PushBytes(const uint8_t* data, std::size_t length) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_parser.Push(data, length);
}

void PacketDispatcher::RegisterHandler(MessageTypeId typeId, PacketHandler handler) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handlers[static_cast<uint16_t>(typeId)] = std::move(handler);
}

void PacketDispatcher::UnregisterHandler(MessageTypeId typeId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_handlers.erase(static_cast<uint16_t>(typeId));
}

void PacketDispatcher::SetErrorHandler(ErrorHandler handler) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_errorHandler = std::move(handler);
}

bool PacketDispatcher::IsConnected(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastRx == Clock::time_point::min()) {
        return false;
    }
    return (now - m_lastRx) <= m_config.connectionTimeout;
}

PacketDispatcher::Clock::time_point PacketDispatcher::LastReceiveTime() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastRx;
}

uint64_t PacketDispatcher::ParseErrorCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_parseErrors;
}

void PacketDispatcher::HandlePacket(const PacketView& packet) {
    m_lastRx = Clock::now();

    auto it = m_handlers.find(static_cast<uint16_t>(packet.header.messageType));
    if (it != m_handlers.end()) {
        it->second(packet);
    }
    // Unknown message types are silently ignored (no handler registered)
}

void PacketDispatcher::HandleError(const StreamParser::ErrorInfo& error) {
    ++m_parseErrors;
    if (m_errorHandler) {
        m_errorHandler(error);
    }
}

} // namespace bcnp
