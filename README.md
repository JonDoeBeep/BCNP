# BCNP Core

This repository hosts the BCNP (Batched Command Network Protocol) core
library. The codebase is split into two layers:

- `src/bcnp`: wire-format definitions, serialization/deserialization, command
	queueing, and a streaming parser. This layer is pure C++17 and only depends on
	the standard library.
- `src/bcnp/transport`: thin adapters that connect streaming byte sources
	(UDP/SPI/etc.) to the core controller without leaking platform headers into
	the protocol logic.

## Quick Start

### 1. Generate Message Types

Define your messages in `schema/messages.json`, then generate the C++ header:

```bash
python schema/bcnp_codegen.py schema/messages.json --cpp generated --python examples
```

This creates `generated/bcnp/message_types.h`.

### 2. Add Include Path

Add the `generated/` folder to your include path so the library can find `<bcnp/message_types.h>`.

**CMake:**
```cmake
target_include_directories(your_target PRIVATE path/to/generated)
```

**FRC Gradle (build.gradle):**
```groovy
model {
    components {
        frcUserProgram(NativeExecutableSpec) {
            // ... existing config ...
            binaries.all {
                cppCompiler.args "-I${projectDir}/libraries/BCNP/generated"
            }
        }
    }
}
```

### 3. Build & Test

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

The command configures the project, builds `libbcnp_core.a`, and runs the
`bcnp_tests` executable that exercises packet encode/decode, queue scheduling,
and chunked stream parsing.

## Using BCNP Core

1. Create a `bcnp::Controller` with a `ControllerConfig` that sets queue
	timeouts and `CommandLimits` for vx/omega/duration clamping.
2. Feed raw bytes into `Controller::PushBytes` (or use `bcnp::ControllerDriver`
	 plus a transport adapter).
3. Poll `Controller::CurrentCommand` to retrieve active commands as
	 wire-accurate `float` + `uint16_t` data.
4. Unit conversions can still happen at the robot layer, but wire values are
	 clamped inside the controller using the provided limits to guarantee safe
	 bounds even if upstream clients misbehave. When `connectionTimeout` elapses
	 without fresh packets, the controller immediately clears the active queue to
	 avoid runaway motion.

### Diagnostics

- `bcnp::StreamParser` surfaces rich `ErrorInfo` (error code, absolute stream
	offset, and consecutive error count) through its error callback so transports
	can log flaky links with context.
- `ControllerDriver` reuses persistent RX/TX buffers to avoid per-cycle
	allocations when feeding data between transports and the controller.