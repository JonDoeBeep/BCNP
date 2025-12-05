#pragma once

#include <cstddef>
#include <cstdint>

namespace bcnp {

/**
 * @brief Interface for sending raw bytes over a transport.
 */
class ByteWriter {
public:
    virtual ~ByteWriter() = default;
    
    /**
     * @brief Send bytes over the transport.
     * @param data Pointer to data buffer
     * @param length Number of bytes to send
     * @return true if sent successfully (or queued), false on error
     */
    virtual bool SendBytes(const uint8_t* data, std::size_t length) = 0;
};

/**
 * @brief Interface for receiving raw bytes from a transport.
 */
class ByteStream {
public:
    virtual ~ByteStream() = default;
    
    /**
     * @brief Receive available bytes from the transport (non-blocking).
     * @param buffer Destination buffer
     * @param maxLength Maximum bytes to read
     * @return Number of bytes actually read (0 if none available)
     */
    virtual std::size_t ReceiveChunk(uint8_t* buffer, std::size_t maxLength) = 0;
};

/**
 * @brief Combined send/receive interface for bidirectional transports.
 */
class DuplexAdapter : public ByteWriter, public ByteStream {
public:
    ~DuplexAdapter() override = default;
};

} // namespace bcnp
