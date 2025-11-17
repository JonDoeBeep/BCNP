#pragma once

#include <cstddef>
#include <cstdint>

namespace bcnp {

class ByteWriter {
public:
    virtual ~ByteWriter() = default;
    virtual bool SendBytes(const uint8_t* data, std::size_t length) = 0;
};

class ByteStream {
public:
    virtual ~ByteStream() = default;
    virtual std::size_t ReceiveChunk(uint8_t* buffer, std::size_t maxLength) = 0;
};

class DuplexAdapter : public ByteWriter, public ByteStream {
public:
    ~DuplexAdapter() override = default;
};

} // namespace bcnp
