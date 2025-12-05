# BCNP: Batched Command Network Protocol

> **Version 3.2.2** 

---

## Table of Contents

- [Overview](#overview)
- [Version History](#version-history)
- [Key Concepts](#key-concepts)
- [Wire Format](#wire-format)
- [Message Types](#message-types)
- [Transport Layer](#transport-layer)
- [Robot Behavior](#robot-behavior)
- [Code Generation](#code-generation)

---

## Overview

BCNP is a binary protocol designed for real-time robot control over unreliable networks. It provides:

- Batched commands
- Fixed-point encoding
- Graceful degradation
- And more, meant to make life easier for robotics networking.

---

## Version History

| Version | Type | Changes |
|---------|------|---------|
| **3.2.2** | Bugfix | Fix telemetry behaviour and move into stable! |
| **3.2.1** | Bugfix | The Commenting and Docs Update! |
| **3.2.0** | Minor | Full duplex communication (bidirectional telemetry) |
| **3.1.0** | Minor | Decoupled command queue, genericized message handling, deprecated SPI |
| **3.0.1** | Bugfix | Minor fixes |
| **3.0.0** | **Major** | Registration-based serialization, JSON schema, codegen, handshake. *Breaking change from v2.x* |
| 2.4.1 | Optimization | Zero-copy parsing with `PacketView`, batch locking |
| 2.4.0 | Minor | 16-bit command count (65k/packet), dynamic allocation |
| 2.3.x | Bugfixes | CRC32, fixed-point encoding, UDP handshake |
| 2.2.x | Minor | Security fixes, optimization |
| 2.1.x | Minor | TCP optimization |
| 2.0.x | **Major** | Complete rewrite, standalone library |
| 1.x | Deprecated | Initial release |

---

## Key Concepts

### Registration-Based Serialization

BCNP v3 uses a Message Type System:

1. Each message type has a unique ID (1–65535)
2. Message structures are defined in JSON (`schema/messages.json`)
3. A codegen tool compiles the schema to C++ and Python
4. Schema hash ensures both endpoints agree on message definitions

### Schema-Driven Development

```bash
# 1. Define messages in schema/messages.json
# 2. Generate code
python schema/bcnp_codegen.py schema/messages.json --cpp generated --python examples

# 3. Include in your code
#include <bcnp/message_types.h>

# 4. Use generated types
bcnp::DriveCmd cmd{.vx = 1.0f, .omega = 0.0f, .durationMs = 100};
```

### Handshake Requirement

All connections must complete a schema handshake before streaming packets:

1. Both sides send an 8-byte handshake packet on connect
2. Schema hashes are compared
3. Connection is rejected if hashes don't match

This prevents silent data corruption from version mismatches.

---

## Wire Format

> **All multi-byte integers use big-endian byte order.**

### Schema Hash

The schema hash is a CRC32 of the canonical JSON representation:

- Ensures client and server agree on field types and order
- Detects version mismatches before data corruption
- Automatically updated by codegen

### Handshake Packet (8 bytes)

```
┌──────────────────────────────────────────────────────────────┐
│  Handshake (8 bytes)                                         │
├─────────────────────────────┬────────────────────────────────┤
│  Magic "BCNP" (4B ASCII)    │  Schema Hash (4B big-endian)   │
└─────────────────────────────┴────────────────────────────────┘
```

Both client and server send this immediately upon connection.

### Data Packet

```
┌──────────────────────────────────────────────────────────────────────────┐
│  HEADER (7 bytes)                                                        │
├──────────┬──────────┬──────────┬─────────────────┬───────────────────────┤
│ Major(1) │ Minor(1) │ Flags(1) │ MsgTypeId(2 BE) │ MsgCount(2 BE)        │
│    3     │    2     │   0x00   │     0x0001      │      0x000A           │
└──────────┴──────────┴──────────┴─────────────────┴───────────────────────┘

┌──────────────────────────────────────────────────────────────────────────┐
│  PAYLOAD (MsgCount × message wire size)                                  │
├──────────────────────────────────────────────────────────────────────────┤
│  Message 0: [field0][field1][field2]...                                  │
│  Message 1: [field0][field1][field2]...                                  │
│  ...                                                                     │
│  Message N: [field0][field1][field2]...                                  │
└──────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────┐
│  CRC32 (4 bytes) — IEEE CRC32 of header + payload                        │
└──────────────────────────────────────────────────────────────────────────┘
```

### Header Fields

| Field | Size | Description |
|-------|------|-------------|
| **Major** | 1 byte | Protocol major version (`3`) |
| *Minor* | 1 byte | Protocol minor version (`2`) |
| Flags | 1 byte | Bit 0: `CLEAR_QUEUE` — clear queue before adding messages |
| MsgTypeId | 2 bytes | Message type ID (1–65535, big-endian) |
| MsgCount | 2 bytes | Number of messages (0–65535, big-endian) |

### Homogeneous Packets

> **Each packet contains messages of a single type.**

The `MsgTypeId` in the header applies to all messages in that packet.

To send different message types, use separate packets:

```
Packet 1: [Header: Type=DriveCmd, Count=10] [DriveCmd×10] [CRC]
Packet 2: [Header: Type=ArmCmd,   Count=5]  [ArmCmd×5]   [CRC]
```

**FRC Impact:** Negligible overhead (11 bytes per packet type). The dispatcher handles multiple packet types per control loop tick.

---

## Message Types

### Default: DriveCmd (ID: 1)

Differential drive velocity command.

| Field | Type | Wire Size | Encoding | Unit |
|-------|------|-----------|----------|------|
| `vx` | float32 | 4 bytes | int32 × 10000 | m/s |
| `omega` | float32 | 4 bytes | int32 × 10000 | rad/s |
| `durationMs` | uint16 | 2 bytes | raw | ms |

Total wire size: 10 bytes

### Fixed-Point Float Encoding

Floats are encoded as `int32` with a scale factor (default: 10000):

* 4 decimal places of precision
* Platform-independent encoding
* Range: ±214,748.3647 (with scale=10000)

**Example:** `vx = 1.5 m/s` → wire value: `15000`

### Defining Custom Message Types

Edit `schema/messages.json`:

```json
{
  "id": 10,
  "name": "SwerveCmd",
  "description": "Swerve drive command",
  "fields": [
    {"name": "vx", "type": "float32", "scale": 10000, "unit": "m/s"},
    {"name": "vy", "type": "float32", "scale": 10000, "unit": "m/s"},
    {"name": "omega", "type": "float32", "scale": 10000, "unit": "rad/s"},
    {"name": "durationMs", "type": "uint16", "unit": "ms"}
  ]
}
```

### Supported Field Types

| Type | Size | Description |
|------|------|-------------|
| `int8` | 1 byte | Signed 8-bit integer |
| `uint8` | 1 byte | Unsigned 8-bit integer |
| `int16` | 2 bytes | Signed 16-bit (big-endian) |
| `uint16` | 2 bytes | Unsigned 16-bit (big-endian) |
| `int32` | 4 bytes | Signed 32-bit (big-endian) |
| `uint32` | 4 bytes | Unsigned 32-bit (big-endian) |
| `float32` | 4 bytes | Float encoded as int32 with scale |

---

## Transport Layer

### Handshake Protocol

| Transport | Handshake Procedure |
|-----------|---------------------|
| **TCP** | Send handshake after connect. Wait for peer before streaming. |
| *UDP* | Send handshake as first datagram. Peer-lock mode requires it. |

```cpp
// Check handshake status
if (adapter.IsHandshakeComplete()) {
    // Ready to stream packets
}

// Diagnostics
uint32_t remoteHash = adapter.GetRemoteSchemaHash();
```

### Transport Guidelines

BCNP packets are self-delimiting (header + length + CRC), so transports act as byte pipes:

| Transport | Guidance |
|-----------|----------|
| **TCP** | Stream bytes directly to `StreamParser`. Framing handled automatically. |
| *UDP* | Forward each datagram to parser. One datagram may contain partial/multiple packets. |

### Sending Packets

```cpp
// Encode once, send the resulting bytes
std::vector<uint8_t> buffer;
bcnp::EncodeTypedPacket(packet, buffer);
transport.Send(buffer.data(), buffer.size());
```

---

## Robot Behavior

### Command Execution Model

1. Queue: Commands are queued in order received
2. Sequential: One command executes at a time
3. Timed: Each command runs for its `durationMs`
4. Timeout: No packets for 200ms → disconnected
5. Safety: Disconnection clears queue, robot stops

### Safety Features

- Command limits: Robot can enforce tighter limits than wire spec:
  ```cpp
  bcnp::MessageQueueConfig config;
  config.maxCommandLag = std::chrono::milliseconds(100);
  ```
- Centralized enforcement: Limits applied before commands enter queue
- Graceful degradation: Stale commands are skipped, not executed late

### SmartDashboard Keys

| Key | Type | Description |
|-----|------|-------------|
| `Network/Connected` | bool | Receiving commands? |
| `Network/QueueSize` | number | Pending commands |
| `Network/CmdVx` | number | Current vx (m/s) |
| `Network/CmdW` | number | Current omega (rad/s) |
| `Network/SchemaHash` | string | Current hash (hex) |
| `Network/ParseErrors` | number | Cumulative errors |

---

## Code Generation

### Running Codegen

```bash
# Generate C++ and Python
python schema/bcnp_codegen.py schema/messages.json \
    --cpp src/bcnp/generated \
    --python examples

# C++ only
python schema/bcnp_codegen.py schema/messages.json --cpp generated

# View schema info (no generation)
python schema/bcnp_codegen.py schema/messages.json
```

### Generated Files

| File | Contents |
|------|----------|
| `message_types.h` | C++ structs with `Encode()`/`Decode()`, constants, registry |
| `bcnp_messages.py` | Python dataclasses with serialization |

### After Schema Changes

1. Run codegen
2. Rebuild C++ project
3. Update Python clients
4. **Both endpoints must use matching schema hash**

---

## Parser Diagnostics

`StreamParser` provides detailed error information:

```cpp
parser.SetErrorCallback([](const StreamParser::ErrorInfo& err) {
    std::cerr << "Parse error: " << int(err.code)
              << " at offset " << err.offset
              << " (consecutive: " << err.consecutiveErrors << ")\n";
});
```

**Error codes:** `TooSmall`, `UnsupportedVersion`, `TooManyMessages`, `Truncated`, `ChecksumMismatch`, `UnknownMessageType`, `SchemaMismatch`
