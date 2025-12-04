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

Create a `messages.json` file for your project:

```json
{
  "version": "3.2",
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

### 2. Integration Options

#### Option A: BCNP as a subdirectory (recommended)

If BCNP lives in `libraries/BCNP`, define your schema in your project root (e.g., `src/messages.json`):

```cmake
# In your top-level CMakeLists.txt
set(BCNP_SCHEMA_FILE "${CMAKE_SOURCE_DIR}/src/messages.json" CACHE FILEPATH "")
set(BCNP_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated" CACHE PATH "")
add_subdirectory(libraries/BCNP)

# Your executable links to bcnp_core and gets the generated types
add_executable(robot src/main.cpp)
target_link_libraries(robot PRIVATE bcnp_core)
```

#### Option B: Standalone build

Edit `schema/messages.json` directly and build:

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

CMake automatically runs `bcnp_codegen.py` when the schema changes, generating headers in the build directory.

### 3. For FRC Projects (GradleRIO)

Add the BCNP library and generated folder to your include path in `build.gradle`:

```groovy
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

Run codegen manually before building:

```bash
python libraries/BCNP/schema/bcnp_codegen.py src/messages.json --cpp generated
```

### Diagnostics

- `bcnp::StreamParser` surfaces rich `ErrorInfo` (error code, absolute stream
	offset, and consecutive error count) through its error callback so transports
	can log flaky links with context.
- `DispatcherDriver` reuses persistent RX buffers to avoid per-cycle
	allocations when feeding data between transports and the dispatcher.
