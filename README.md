# BCNP

Binary protocol for real-time robot control. Schema-driven with C++/Python codegen.

## Usage

```cpp
#include <bcnp/dispatcher.h>
#include <bcnp/message_types.h>

bcnp::PacketDispatcher dispatcher;
bcnp::MessageQueue<bcnp::DriveCmd> driveQueue;

dispatcher.RegisterHandler<bcnp::DriveCmd>([&](const bcnp::PacketView& pkt) {
    for (auto it = pkt.begin_as<bcnp::DriveCmd>(); it != pkt.end_as<bcnp::DriveCmd>(); ++it) {
        driveQueue.Push(*it);
    }
});
```

## Installation

Add as CMake subdirectory:

```cmake
set(BCNP_SCHEMA_FILE "${CMAKE_SOURCE_DIR}/src/messages.json" CACHE FILEPATH "")
add_subdirectory(libraries/BCNP)
target_link_libraries(robot PRIVATE bcnp_core)
```

Or include `src/bcnp/` directly and run codegen manually:

```bash
python schema/bcnp_codegen.py messages.json --cpp generated --python examples
```

## Requirements

- C++17
- Python 3.x (for codegen)

## Documentation

| Document | Description |
|----------|-------------|
| [protocol.md](protocol.md) | Wire format specification |
| [schema/schema.md](schema/schema.md) | Schema format and codegen |

## License

[MIT](LICENSE)
