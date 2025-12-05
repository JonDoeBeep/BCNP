#pragma once

/**
 * @file spi_adapter.h
 * @brief SPI transport adapter for BCNP (deprecated).
 * 
 * @deprecated This SPI adapter is deprecated. Use TcpPosixAdapter or 
 *             UdpPosixAdapter instead for modern implementations.
 */

#include "bcnp/packet.h"
#include "bcnp/stream_parser.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace bcnp {

/**
 * @brief SPI transport adapter for legacy SPI-based communication.
 * 
 * @deprecated Use TcpPosixAdapter or UdpPosixAdapter instead.
 *             This class will be removed in a future version.
 * 
 * Provides a polling-based interface for SPI communication with the
 * BCNP stream parser. Handles both receiving data chunks and sending
 * typed packets over an SPI bus.
 * 
 * @note This adapter does not manage SPI hardware directly; it requires
 *       user-provided receive and send function callbacks.
 */
class [[deprecated("Use TcpPosixAdapter instead")]] SpiStreamAdapter {
public:
    /**
     * @brief Callback type for receiving SPI data chunks.
     * @param dst Destination buffer to fill
     * @param maxLen Maximum bytes to read
     * @return Number of bytes actually read (0 if none available)
     */
    using ReceiveChunkFn = std::function<std::size_t(uint8_t* dst, std::size_t maxLen)>;
    
    /**
     * @brief Callback type for sending raw bytes over SPI.
     * @param data Data to transmit
     * @param length Number of bytes to send
     * @return true if send succeeded, false on error
     */
    using SendBytesFn = std::function<bool(const uint8_t* data, std::size_t length)>;

    /**
     * @brief Construct an SPI adapter with receive/send callbacks.
     * 
     * @param receive Function to call to receive data chunks
     * @param send Function to call to transmit data
     * @param parser Reference to StreamParser for received data
     */
    SpiStreamAdapter(ReceiveChunkFn receive, SendBytesFn send, StreamParser& parser);

    /**
     * @brief Poll for incoming SPI data.
     * 
     * Calls the receive callback repeatedly until no more data is available,
     * feeding each chunk to the parser. Call this periodically in your main loop.
     */
    void Poll();

    /**
     * @brief Push a data chunk directly to the parser.
     * 
     * Bypasses the receive callback for manual data injection.
     * 
     * @param data Pointer to received data
     * @param length Number of bytes received
     */
    void PushChunk(const uint8_t* data, std::size_t length);

    /**
     * @brief Send a typed packet over SPI.
     * 
     * Encodes the packet to wire format and transmits using the send callback.
     * 
     * @tparam MsgType Message type in the packet
     * @param packet The packet to send
     * @return true if send succeeded, false on encoding or transmission error
     */
    template<typename MsgType>
    bool SendPacket(const TypedPacket<MsgType>& packet) {
        std::vector<uint8_t> buffer;
        if (!EncodeTypedPacket(packet, buffer)) {
            return false;
        }
        return m_send(buffer.data(), buffer.size());
    }

private:
    ReceiveChunkFn m_receive;
    SendBytesFn m_send;
    StreamParser& m_parser;
};

} // namespace bcnp
