# BCNP: Batched Command Network Protocol v2.3.0

## Version History:
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

## Protocol Specification

### Packet Structure

All multi-byte integers use **big-endian** byte order.

```
┌─────────────────────────────────────────────────────┐
│ Header (4 bytes)                                     │
├──────────┬───────────┬──────────┬───────────────────┤
│ Major(1) │ Minor (1) │ Flags(1) │ Cmd Count (1)     │
└──────────┴───────────┴──────────┴───────────────────┘

┌─────────────────────────────────────────────────────┐
│ Command 1 (10 bytes)                                 │
├──────────────────┬───────────────┬──────────────────┤
│ VX (4)           │ Omega (4)     │ Duration (2)     │
│ int32 (BE)       │ int32 (BE)    │ uint16 (BE)      │
│ 1e-4 m/s units   │ 1e-4 rad/s    │ milliseconds     │
└──────────────────┴───────────────┴──────────────────┘

┌─────────────────────────────────────────────────────┐
│ Command 2 (10 bytes)                                 │
│ ... (same structure)                                 │
└─────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│ CRC32 (4 bytes)                                      │
├─────────────────────────────────────────────────────┤
│ IEEE CRC32 of header+commands                        │
└─────────────────────────────────────────────────────┘

... (up to 100 commands per packet)
```

### Header Fields

- **Major** (1 byte): Protocol major version. Current: `2`
- **Minor** (1 byte): Protocol minor version. Current: `3`
  - Robot rejects packets with mismatched major.minor version
  - Patch version not transmitted (for bug fixes only)
- **Flags** (1 byte): Bit flags for special operations:
  - Bit 0: `CLEAR_QUEUE` - If set, clears the existing command queue before adding new commands
  - Bits 1-7: Reserved (set to 0)
- **Command Count** (1 byte): Number of commands in this packet (0-100)

### Queue Management

- **Maximum queue size:** 200 commands (DoS protection)
- **Maximum commands per packet:** 100 commands
- **Packet size:** 4 bytes (header) + 1000 bytes (100 commands) + 4 bytes (CRC32) = 1008 bytes (fits in standard MTU)
- Clients can send multiple packets to fill the 200-command queue

### Command Fields

Each command consists of 10 bytes:

- **VX** (4 bytes, signed int32, big-endian): Forward velocity in meters per second encoded as fixed-point (`value = raw / 10,000`).
  - Range: still clamped to robot limits (e.g., -1.5 to +1.5 m/s).
  - Positive = forward, Negative = backward.
  - Senders convert `float` to wire format with `round(vx * 10000.0f)`.
  
- **Omega** (4 bytes, signed int32, big-endian): Angular velocity encoded as fixed-point radians/sec with the same 1e-4 scale.
  
- **Duration** (2 bytes, uint16, big-endian): How long to execute this command in milliseconds
  - Range: 0 to 65535 ms (~65 seconds max per command)
  - The robot will execute this command for the specified duration before moving to the next

### Integrity and Fixed-Point Math

- Every packet carries an IEEE CRC32 over the header and command payload. Receivers drop packets whose CRC does not match, preventing silent bit flips from propagating into the queue.
- Fixed-point encoding removes IEEE-754 dependencies so heterogeneous controllers (DSPs, MCUs, desktop planners) agree on the exact on-wire value without per-platform float quirks.

### Controller Limits & Safety

- Robots may choose tighter limits than the wire specification by supplying a
  `ControllerConfig::CommandLimits` struct when constructing `bcnp::Controller`.
- Limits are enforced centrally (before commands enter the execution queue), so
  even legacy senders are bounded to robot-approved velocity and duration
  ranges.

### Parser Diagnostics

- `bcnp::StreamParser` emits an `ErrorInfo` payload to its error callback that
  includes the `PacketError` code, the byte offset (from the start of the stream)
  where parsing failed, and a monotonically increasing consecutive-error count.
- Transports can log these details to correlate bursty link noise, sync loss, or
  malformed traffic back to specific timestamps and offsets.

## Transport Layer Guidance

BCNP frames already provide packet delineation (start/stop/escape + CRC in the
framing layer), so transports should behave like streaming byte pipes:

- **SPI**: clock bytes continuously while chip-select is asserted and feed each
  chunk into `bcnp::StreamParser`/`Controller::PushBytes`. There is no
  requirement (or benefit) to issue a "one transfer == one packet" request.
- **UDP or other datagram transports**: forward each datagram buffer directly to
  the parser; the controller validates headers, lengths, command counts, and CRC.
  When `UdpPosixAdapter` runs in **peer-lock** mode it now requires a pairing
  handshake: send a single 8-byte datagram containing the ASCII literal
  `"BCNP"` followed by the agreed 32-bit token before streaming BCNP packets.
  Call `UdpPosixAdapter::UnlockPeer()` to re-arm pairing if you intentionally
  switch driver stations.
- **Send path**: when transmitting commands back to a client, serialize once via
  `bcnp::EncodePacket` and send the resulting byte span with a blocking
  `sendBytes()` equivalent. The supplied TCP adapter now buffers and drains TX
  data in the background to avoid blocking real-time loops while the kernel
  window is full.

## Robot Behavior

1. **Command Queue**: The robot maintains an internal queue of commands
2. **Sequential Execution**: Commands are executed one at a time, in the order received
3. **Timing**: Each command runs for its specified duration before the next command starts
4. **Connection Timeout**: If no packets are received for 200ms, the robot considers itself disconnected
5. **Safety**: When disconnected, the BCNP controller clears its queue (including the active command) so the robot stops immediately, even if long-duration commands were pending

## SmartDashboard Keys

The robot publishes the following information to SmartDashboard:

- `Network/Connected`: Boolean indicating if robot is receiving commands
- `Network/QueueSize`: Number of commands waiting in the queue
- `Network/CmdVx`: Current commanded forward velocity (m/s)
- `Network/CmdW`: Current commanded angular velocity (rad/s)