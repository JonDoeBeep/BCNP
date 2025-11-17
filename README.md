# BCNP Core

This repository hosts the portable BCNP (Batched Command Network Protocol) core
library. The codebase is split into three layers:

- `src/bcnp`: wire-format definitions, serialization/deserialization, command
	queueing, and a streaming parser. This layer is pure C++17 and only depends on
	the standard library.
- `src/bcnp/transport`: thin adapters that connect streaming byte sources
	(UDP/SPI/etc.) to the core controller without leaking platform headers into
	the protocol logic. The provided POSIX UDP adapter is an example of a
	transport that feeds bytes chunk-by-chunk.
- `old (remove after use!)/`: legacy robot-side code that now consumes the BCNP
	controller and converts packets into WPILib units.

## Building & Testing (WSL)

On Windows, run the build inside WSL to pick up the POSIX headers required by
the UDP adapter:

```powershell
wsl -d Ubuntu -- bash -lc "cd /mnt/c/Users/michaelb/robot/BCNP && cmake -S . -B build && cmake --build build && ctest --test-dir build"
```

The command configures the project, builds `libbcnp_core.a`, and runs the
`bcnp_tests` executable that exercises packet encode/decode, queue scheduling,
and chunked stream parsing.

## Using BCNP Core

1. Create a `bcnp::Controller` with your desired queue limits/timeouts.
2. Feed raw bytes into `Controller::PushBytes` (or use `bcnp::ControllerDriver`
	 plus a transport adapter).
3. Poll `Controller::CurrentCommand` to retrieve active commands as
	 wire-accurate `float` + `uint16_t` data.
4. Perform unit conversions/clamping in your robot code before applying motion
	 commands.

The controller never touches WPILib, sockets, or SPI APIs; transports simply
marshal bytes between your hardware interface and the pure C++ core.