# BCNP Core

This repository hosts the BCNP (Batched Command Network Protocol) core
library. The codebase is split into two layers:

- `src/bcnp`: wire-format definitions, serialization/deserialization, message
	queueing, and a streaming parser. This layer is pure C++17 and only depends on
	the standard library.
- `src/bcnp/transport`: thin adapters that connect streaming byte sources
	(TCP/UDP) to the core dispatcher without leaking platform headers into
	the protocol logic.

## Quick Start

### 1. Define Your Message Types

Edit `schema/messages.json` to define your robot's message types:

```json
{
  "version": "3.0",
  "namespace": "bcnp",
  "messages": [
    {
      "id": 1,
      "name": "DriveCmd",
      "fields": [
        {"name": "vx", "type": "float32", "scale": 10000},
        {"name": "omega", "type": "float32", "scale": 10000},
        {"name": "durationMs", "type": "uint16"}
      ]
    }
  ]
}
```

### 2. Build (CMake auto-generates message_types.h)

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

CMake automatically runs `bcnp_codegen.py` when the schema changes, generating `generated/bcnp/message_types.h`.

### 3. For FRC Projects

Add the BCNP library and generated folder to your include path in `build.gradle`:

```groovy
model {
    components {
        frcUserProgram(NativeExecutableSpec) {
            binaries.all {
                cppCompiler.args "-I${projectDir}/libraries/BCNP/src"
                cppCompiler.args "-I${projectDir}/libraries/BCNP/generated"
            }
        }
    }
}
```

```bash
python libraries/BCNP/schema/bcnp_codegen.py libraries/BCNP/schema/messages.json --cpp libraries/BCNP/generated
```

### Diagnostics

- `bcnp::StreamParser` surfaces rich `ErrorInfo` (error code, absolute stream
	offset, and consecutive error count) through its error callback so transports
	can log flaky links with context.
- `DispatcherDriver` reuses persistent RX buffers to avoid per-cycle
	allocations when feeding data between transports and the dispatcher.