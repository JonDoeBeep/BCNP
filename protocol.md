# BCNP Protocol Specification

Version 3.2

## Wire Format

All multi-byte integers are big-endian.

### Handshake (8 bytes)

```
┌─────────────────────────┬────────────────────────────┐
│  Magic "BCNP" (4B)      │  Schema Hash (4B)          │
└─────────────────────────┴────────────────────────────┘
```

Both sides send on connect. Connection rejected if hashes don't match.

### Data Packet

```
┌──────────────────────────────────────────────────────────────────┐
│  HEADER (7 bytes)                                                │
├──────────┬──────────┬──────────┬───────────────┬─────────────────┤
│ Major(1) │ Minor(1) │ Flags(1) │ MsgTypeId(2)  │ MsgCount(2)     │
└──────────┴──────────┴──────────┴───────────────┴─────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│  PAYLOAD (MsgCount × message wire size)                          │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│  CRC32 (4 bytes)                                                 │
└──────────────────────────────────────────────────────────────────┘
```

### Header Fields

| Field | Size | Description |
|-------|------|-------------|
| Major | 1B | Protocol major version (3) |
| Minor | 1B | Protocol minor version (2) |
| Flags | 1B | Bit 0: `CLEAR_QUEUE` |
| MsgTypeId | 2B | Message type ID (1-65535) |
| MsgCount | 2B | Number of messages |

Each packet contains messages of a single type.

## Field Types

| Type | Size | Description |
|------|------|-------------|
| `int8` | 1B | Signed 8-bit |
| `uint8` | 1B | Unsigned 8-bit |
| `int16` | 2B | Signed 16-bit |
| `uint16` | 2B | Unsigned 16-bit |
| `int32` | 4B | Signed 32-bit |
| `uint32` | 4B | Unsigned 32-bit |
| `float32` | 4B | int32 × scale factor |

### Float Encoding

Floats encoded as `int32` with scale factor (default 10000):

```
wire_value = (int32_t)(float_value * scale)
```

Range: ±214,748.3647 with scale=10000

## Command Execution

1. Commands queued in order
2. One command executes at a time for its `durationMs`
3. Disconnection clears queue, robot stops

```cpp
bcnp::MessageQueueConfig config;
config.maxCommandLag = std::chrono::milliseconds(100);
```

Stale commands are skipped, not executed late.

## Error Codes

`TooSmall`, `UnsupportedVersion`, `TooManyMessages`, `Truncated`, `ChecksumMismatch`, `UnknownMessageType`, `SchemaMismatch`

```cpp
parser.SetErrorCallback([](const StreamParser::ErrorInfo& err) {
    std::cerr << "Error: " << int(err.code) << " at " << err.offset << "\n";
});
```
