# BCNP Core

## **B**atched **C**ommand **N**etwork **P**rotocol: a lightweight, real-time binary protocol for robot control.

[![Protocol Version](https://img.shields.io/badge/protocol-v3.2-blue)]()
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-17-blue)]()

BCNP enables reliable, low-latency command streaming between control stations and robots. It's designed for FRC robotics but works anywhere you need efficient binary messaging with timing guarantees.

## Features

- Schema-driven: Define messages in JSON, generate C++/Python code automatically
- Real-time safe: Optional zero-allocation mode with `StaticVector` storage
- Batched commands: Send multiple timestamped commands in a single packet
- Integrity checking: CRC32 validation on every packet
- Connection monitoring: Automatic timeout detection and safe shutdown
- Full duplex: Bidirectional telemetry and command streaming simultaneously

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  src/bcnp/                                                      │
│  ├── packet.h/cpp        — Wire format, encoding/decoding       │
│  ├── stream_parser.h/cpp — Byte stream reassembly & framing     │
│  ├── dispatcher.h/cpp    — Route packets to message handlers    │
│  ├── message_queue.h     — Timed command execution queue        │
│  ├── telemetry_accumulator.h — Batched sensor data transmission │
│  └── transport/          — TCP/UDP adapters (platform-specific) │
└─────────────────────────────────────────────────────────────────┘
```

- **`src/bcnp/`**: Core protocol library (pure C++17, no platform dependencies)
- **`src/bcnp/transport/`**: Transport adapters that connect byte streams to the dispatcher

---

## Quick Start

### 1. Define Your Message Types

Create a `messages.json` file describing your command and telemetry structures:

```json
{
  "version": "3.2",
  "namespace": "bcnp",
  "messages": [
    {
      "id": 1,
      "name": "DriveCmd",
      "description": "Differential drive velocity command",
      "fields": [
        {"name": "vx", "type": "float32", "scale": 10000, "unit": "m/s"},
        {"name": "omega", "type": "float32", "scale": 10000, "unit": "rad/s"},
        {"name": "durationMs", "type": "uint16", "unit": "ms"}
      ]
    }
  ]
}
```

### 2. Generate Code

```bash
python schema/bcnp_codegen.py messages.json --cpp generated --python examples
```

This creates:
- `generated/bcnp/message_types.h` : C++ structs with `Encode()`/`Decode()` methods
- `examples/bcnp_messages.py` : Python dataclasses with serialization

### 3. Integrate with Your Project

#### Option A: CMake Subdirectory (Recommended)

```cmake
# Your project's CMakeLists.txt
set(BCNP_SCHEMA_FILE "${CMAKE_SOURCE_DIR}/src/messages.json" CACHE FILEPATH "")
set(BCNP_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated" CACHE PATH "")
add_subdirectory(libraries/BCNP)

add_executable(robot src/main.cpp)
target_link_libraries(robot PRIVATE bcnp_core)
```

CMake automatically regenerates code when the schema changes.

#### Option B: FRC GradleRIO

```groovy
// build.gradle
model {
    components {
        frcUserProgram(NativeExecutableSpec) {
            binaries.all {
                cppCompiler.args "-I${projectDir}/libraries/BCNP/src"
                cppCompiler.args "-I${projectDir}/generated"
            }
        }
    }
}
```

Run codegen before building:

```bash
python libraries/BCNP/schema/bcnp_codegen.py src/messages.json --cpp generated
```

#### Option C: Standalone Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

---

## Usage Example

### Robot Side (C++)

```cpp
#include <bcnp/dispatcher.h>
#include <bcnp/message_queue.h>
#include <bcnp/message_types.h>

// Create dispatcher and command queue
bcnp::PacketDispatcher dispatcher;
bcnp::MessageQueue<bcnp::DriveCmd> driveQueue;

// Register handler for DriveCmd packets
dispatcher.RegisterHandler<bcnp::DriveCmd>([&](const bcnp::PacketView& pkt) {
    for (auto it = pkt.begin_as<bcnp::DriveCmd>(); it != pkt.end_as<bcnp::DriveCmd>(); ++it) {
        driveQueue.Push(*it);
    }
    driveQueue.NotifyReceived(std::chrono::steady_clock::now());
});

// In your control loop:
void Periodic() {
    // Feed bytes from transport
    dispatcher.PushBytes(rxBuffer, rxLength);
    
    // Update queue timing
    driveQueue.Update(std::chrono::steady_clock::now());
    
    // Execute active command
    if (auto cmd = driveQueue.ActiveMessage()) {
        drivetrain.Drive(cmd->vx, cmd->omega);
    } else {
        drivetrain.Stop();
    }
}
```

### Client Side (Python)

```python
from bcnp_messages import DriveCmd, encode_packet

# Create commands with durations
commands = [
    DriveCmd(vx=1.0, omega=0.0, durationMs=500),  # Forward 0.5s
    DriveCmd(vx=0.0, omega=1.5, durationMs=300),  # Turn 0.3s
]

# Encode and send
packet = encode_packet(commands)
socket.send(packet)
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [protocol.md](protocol.md) | Full protocol specification and wire format |
| [schema/schema.md](schema/schema.md) | Schema format and code generation guide |

---

## Diagnostics

BCNP provides diagnostics for debugging communication issues:

- `StreamParser::ErrorInfo`: Reports error code, byte offset, and consecutive error count
- `MessageQueue::GetMetrics()`: Tracks messages received, overflows, and skipped commands
- `PacketDispatcher::ParseErrorCount()`: Cumulative parse error counter

Publish to SmartDashboard for real-time monitoring:

```cpp
frc::SmartDashboard::PutBoolean("Network/Connected", dispatcher.IsConnected(now));
frc::SmartDashboard::PutNumber("Network/ParseErrors", dispatcher.ParseErrorCount());
frc::SmartDashboard::PutString("Network/SchemaHash", "0x" + std::to_string(bcnp::kSchemaHash));
```

---

## License

MIT License — See [LICENSE](LICENSE) for details.
