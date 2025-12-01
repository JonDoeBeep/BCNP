# BCNP: Batched Command Network Protocol v3.0

## Version History:
- **v3.0.0** (Major): Registration-based serialization with JSON schema, message type IDs, schema hash handshake, and codegen for C++/Python. Breaking change from v2.x.
- **v2.4.1** (Optimization): Implemented Zero-Copy packet parsing using `PacketView` and `CommandIterator`, and added batch locking to reduce mutex overhead on large packets.
- **v2.4.0** (Major): Expanded command count to 16-bit (65k commands/packet), increased packet size limit, and switched to dynamic memory allocation.
- **v2.3.2** (bugfix): Fix bloat, safety, and optimize.
- **v2.3.1** (bugfix): Fix several critical issues.
- **v2.3.0** (Minor): Adds CRC32 integrity trailer, fixed-point command encoding, UDP pairing handshake, and buffered TCP send path.
- **v2.2.1** (Minor): Security bugfixes. 
- **v2.2.0** (Minor, deprecated): Security bugfixes, optimization. 
- **v2.1.1** (Bugfix): TCP improvements.
- **v2.1.0** (Minor, deprecated): TCP Optimization/Adapter support.
- **v2.0.3** (Bugfix): Packet loss code revamps, DOS prevention.
- **v2.0.2** (Bugfix): Prevent UDP datagram truncation, keep POSIX transport optional on non-UNIX builds, and ensure unit tests run even in Release builds.
- **v2.0.1** (Bugfix): Optimize O(n^2) parser to O(m), fix queueing to be more efficient.
- **v2.0.0** (Major, deprecated): Complete rewrite, BNCP lives in it's own seperate library
- **v1.1.0** (Minor, deprecated): All fields now use big-endian. Header format changed to major.minor.flags.count
- **v1.0.0**: Initial release with mixed endianness (deprecated)

### Registration-Based Serialization
Instead of hardcoded vx/omega commands, v3 introduces a **Message Type System** where:
- Each message type has a unique ID (1-65535)
- Message structures are defined in JSON (`schema/messages.json`)
- A Python codegen tool compiles the schema to C++ headers and Python bindings
- Schema hash ensures client and server agree on message definitions

### Schema-Driven Development
1. Define messages in `schema/messages.json`
2. Run codegen: `python schema/bcnp_codegen.py schema/messages.json --cpp generated --python examples`
3. Use generated types in your code: `DriveCmd`, or your custom types

### Handshake Requirement
All connections must complete a schema handshake before streaming packets. The handshake validates that both endpoints are using the same schema version.

## Protocol Specification

### Schema Hash
The schema hash is a CRC32 of the canonical JSON representation of the message definitions. This ensures:
- Client and server agree on field types and order
- Version mismatches are detected before data corruption
- Future schema changes invalidate old clients

Current schema hash: `0x0152BCDB` (automatically updated by codegen)

### Handshake Packet (8 bytes)
```
┌───────────────────────────────────────────────────────────┐
│ Handshake (8 bytes)                                       │
├──────────────────────────────┬────────────────────────────┤
│ Magic "BCNP" (4 bytes ASCII) │ Schema Hash (4 bytes BE)   │
└──────────────────────────────┴────────────────────────────┘
```

Both client and server send this packet upon connection. If schema hashes don't match, the connection should be rejected.

### V3 Packet Structure

All multi-byte integers use **big-endian** byte order.

```
┌──────────────────────────────────────────────────────────────────────┐
│ Header (7 bytes)                                                      │
├──────────┬───────────┬──────────┬───────────────┬────────────────────┤
│ Major(1) │ Minor (1) │ Flags(1) │ MsgTypeId (2) │ Msg Count (2)      │
│    3     │     0     │          │ uint16 (BE)   │ uint16 (BE)        │
└──────────┴───────────┴──────────┴───────────────┴────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ Message 1 (variable size based on message type)         │
│ ... fields encoded per schema definition ...            │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ Message 2 (same type, same size)                        │
│ ... (all messages in a packet have the same type)       │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ CRC32 (4 bytes)                                          │
├─────────────────────────────────────────────────────────┤
│ IEEE CRC32 of header+messages                            │
└─────────────────────────────────────────────────────────┘

... (up to 65,535 messages per packet)
```

### Header Fields (V3)

- **Major** (1 byte): Protocol major version. Current: `3`
- **Minor** (1 byte): Protocol minor version. Current: `0`
  - Robot rejects packets with mismatched major.minor version
- **Flags** (1 byte): Bit flags for special operations:
  - Bit 0: `CLEAR_QUEUE` - If set, clears the existing command queue before adding new messages
  - Bits 1-7: Reserved (set to 0)
- **Message Type ID** (2 bytes, uint16, big-endian): The type of messages in this packet
  - 0: Reserved (invalid)
  - 1: DriveCmd (default, see schema)
  - 2-65535: User-defined in schema/messages.json
- **Message Count** (2 bytes, uint16, big-endian): Number of messages in this packet (0-65535)

### Default Message Type

The default schema includes DriveCmd for differential drive robots. You can add, remove, or modify message types in `schema/messages.json`.

#### DriveCmd (ID: 1, 10 bytes)
Differential drive velocity command.

| Field | Type | Bytes | Encoding | Description |
|-------|------|-------|----------|-------------|
| vx | float32 | 4 | int32 × 10000 | Linear velocity (m/s) |
| omega | float32 | 4 | int32 × 10000 | Angular velocity (rad/s) |
| durationMs | uint16 | 2 | raw | Duration in milliseconds |

### Defining Custom Message Types

Edit `schema/messages.json` to add your own message types:

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

Supported field types:
- `int8`, `uint8` (1 byte)
- `int16`, `uint16` (2 bytes, big-endian)
- `int32`, `uint32` (4 bytes, big-endian)
- `float32` (4 bytes, encoded as int32 with scale factor)

### Fixed-Point Float Encoding

Floats are encoded as `int32` with a scale factor (default: 10000). This provides:
- 4 decimal places of precision
- Platform-independent encoding
- Range: ±214,748.3647 (with scale=10000)

Example: `vx = 1.5 m/s` → wire value: `15000`

### Integrity and Fixed-Point Math

- Every packet carries an IEEE CRC32 over the header and message payload. Receivers drop packets whose CRC does not match, preventing silent bit flips from propagating into the queue.
- Fixed-point encoding removes IEEE-754 dependencies so heterogeneous controllers (DSPs, MCUs, desktop planners) agree on the exact on-wire value without per-platform float quirks.

### Controller Limits & Safety

- Robots may choose tighter limits than the wire specification by supplying a
  `ControllerConfig::CommandLimits` struct when constructing `bcnp::Controller`.
- Limits are enforced centrally (before commands enter the execution queue), so
  even legacy senders are bounded to robot-approved velocity and duration
  ranges.

### Message Type Handlers

Register handlers for custom message types:

```cpp
// Example: Register a handler for a custom SwerveCmd message type
controller.RegisterHandler(MessageTypeId::SwerveCmd, [](const PacketView& packet) {
    for (auto it = packet.begin_as<SwerveCmd>(); it != packet.end_as<SwerveCmd>(); ++it) {
        SwerveCmd cmd = *it;
        // Handle swerve command
    }
});
```

### Parser Diagnostics

- `bcnp::StreamParser` emits an `ErrorInfo` payload to its error callback that
  includes the `PacketError` code, the byte offset (from the start of the stream)
  where parsing failed, and a monotonically increasing consecutive-error count.
- Transports can log these details to correlate bursty link noise, sync loss, or
  malformed traffic back to specific timestamps and offsets.

## Transport Layer Guidance

### Handshake Requirement

All transports must perform schema handshake before streaming BCNP packets:

1. **TCP**: Send handshake immediately after connect. Wait for peer handshake before sending packets.
2. **UDP**: Send handshake as first datagram. Peer-lock mode requires handshake before accepting packets.

### Handshake Validation

```cpp
// Check if handshake is complete and schemas match
if (adapter.IsHandshakeComplete()) {
    // Ready to stream packets
}

// Get remote schema hash for diagnostics
uint32_t remoteHash = adapter.GetRemoteSchemaHash();
if (remoteHash != kSchemaHash) {
    // Log schema mismatch
}
```

BCNP frames already provide packet delineation (start/stop/escape + CRC in the
framing layer), so transports should behave like streaming byte pipes:

- **SPI**: clock bytes continuously while chip-select is asserted and feed each
  chunk into `bcnp::StreamParser`/`Controller::PushBytes`. There is no
  requirement (or benefit) to issue a "one transfer == one packet" request.
- **UDP or other datagram transports**: forward each datagram buffer directly to
  the parser; the controller validates headers, lengths, message types, and CRC.
- **Send path**: when transmitting commands back to a client, serialize once via
  `bcnp::EncodePacket` or `EncodeTypedPacket<T>` and send the resulting byte span.

## Robot Behavior

1. **Command Queue**: The robot maintains an internal queue of commands (DriveCmd by default)
2. **Sequential Execution**: Commands are executed one at a time, in the order received
3. **Timing**: Each command runs for its specified duration before the next command starts
4. **Connection Timeout**: If no packets are received for 200ms, the robot considers itself disconnected
5. **Safety**: When disconnected, the BCNP controller clears its queue (including the active command) so the robot stops immediately

## SmartDashboard Keys

The robot publishes the following information to SmartDashboard:

- `Network/Connected`: Boolean indicating if robot is receiving commands
- `Network/QueueSize`: Number of commands waiting in the queue
- `Network/CmdVx`: Current commanded forward velocity (m/s)
- `Network/CmdW`: Current commanded angular velocity (rad/s)
- `Network/SchemaHash`: Current schema hash (hex string)

## Code Generation

### Running Codegen

```bash
# Generate both C++ and Python
python schema/bcnp_codegen.py schema/messages.json --cpp src/bcnp/generated --python examples

# Generate only C++
python schema/bcnp_codegen.py schema/messages.json --cpp src/bcnp/generated

# View schema info without generating
python schema/bcnp_codegen.py schema/messages.json
```

### Generated Files

- `generated/message_types.h`: C++ header with structs, serializers, and constants
- `examples/bcnp_messages.py`: Python module with dataclasses and encode/decode methods

### Regenerating After Schema Changes

After modifying `schema/messages.json`:
1. Run codegen
2. Rebuild C++ project
3. Update Python clients
4. Both sides must use matching schema hash to communicate
