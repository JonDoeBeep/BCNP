# BCNP Schema & Code Generation

> Define message types in JSON, generate type-safe C++ and Python code automatically.

---

## Overview

The BCNP schema system provides:

- JSON schema
- Code generation
- Schema hashing
- Type safety

---

## Files

| File | Purpose |
|------|---------|
| `messages.json` | Your message type definitions (edit this) |
| `bcnp_schema.json` | JSON Schema for validating messages.json |
| `bcnp_codegen.py` | Code generator script |

---

## Quick Start

### 1. Define Messages

Edit `messages.json`:

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
# Generate C++ and Python
python bcnp_codegen.py messages.json --cpp ../generated --python ../examples

# C++ only
python bcnp_codegen.py messages.json --cpp ../generated

# View schema info (no file generation)
python bcnp_codegen.py messages.json
```

### 3. Use Generated Code

**C++:**
```cpp
#include <bcnp/message_types.h>

bcnp::DriveCmd cmd{.vx = 1.5f, .omega = 0.0f, .durationMs = 100};
```

*Python:*
```python
from bcnp_messages import DriveCmd, encode_packet

cmd = DriveCmd(vx=1.5, omega=0.0, durationMs=100)
packet = encode_packet([cmd])
```

---

## Adding a New Message Type

### Step 1: Edit `messages.json`

Add a new entry to the `messages` array:

```json
{
  "id": 10,
  "name": "ArmCmd",
  "description": "Arm position command",
  "fields": [
    {"name": "shoulderAngle", "type": "float32", "scale": 10000, "unit": "rad"},
    {"name": "elbowAngle", "type": "float32", "scale": 10000, "unit": "rad"},
    {"name": "gripperOpen", "type": "uint8"},
    {"name": "durationMs", "type": "uint16", "unit": "ms"}
  ]
}
```

### Step 2: Run Codegen

```bash
python bcnp_codegen.py messages.json --cpp ../generated --python ../examples
```

### Step 3: Rebuild & Update

1. Rebuild your C++ project
2. Update Python clients with new `bcnp_messages.py`
3. **Both sides must have matching schema hash**

---

## Supported Field Types

| Type | Wire Size | Description |
|------|-----------|-------------|
| `int8` | 1 byte | Signed 8-bit integer |
| `uint8` | 1 byte | Unsigned 8-bit integer |
| `int16` | 2 bytes | Signed 16-bit integer (big-endian) |
| `uint16` | 2 bytes | Unsigned 16-bit integer (big-endian) |
| `int32` | 4 bytes | Signed 32-bit integer (big-endian) |
| `uint32` | 4 bytes | Unsigned 32-bit integer (big-endian) |
| `float32` | 4 bytes | Float encoded as int32 with scale factor |

### Float Encoding

`float32` fields use fixed-point encoding for platform independence:

```
wire_value = (int32_t)(float_value * scale)
float_value = wire_value / scale
```

Default scale: `10000` (4 decimal places, range ±214,748.3647)

---

## Message Definition Format

```json
{
  "id": 1,                              // Required: Unique ID (1-65535)
  "name": "MessageName",                // Required: PascalCase identifier
  "description": "What this does",      // Optional: Documentation
  "fields": [                           // Required: Array of fields
    {
      "name": "fieldName",              // Required: camelCase identifier
      "type": "uint16",                 // Required: One of supported types
      "scale": 10000,                   // Optional: For float32 only
      "unit": "m/s"                     // Optional: Documentation only
    }
  ]
}
```

---

## Schema Hash

The schema hash is a CRC32 computed from the canonical JSON representation of all message definitions.
The hash is printed during codegen and embedded in generated code:

```
Schema hash: 0x0152BCDB
```

---

## Generated Output

### C++ Header (`message_types.h`)

```cpp
namespace bcnp {

// Protocol constants
constexpr uint8_t kProtocolMajorV3 = 3;
constexpr uint8_t kProtocolMinorV3 = 2;
constexpr uint32_t kSchemaHash = 0x0152BCDB;

// Message type enum
enum class MessageTypeId : uint16_t {
    Unknown = 0,
    DriveCmd = 1,
    // ... your types
};

// Generated struct
struct DriveCmd {
    static constexpr MessageTypeId kTypeId = MessageTypeId::DriveCmd;
    static constexpr std::size_t kWireSize = 10;
    
    float vx{0.0f};
    float omega{0.0f};
    uint16_t durationMs{0};
    
    bool Encode(uint8_t* buffer, std::size_t size) const;
    static std::optional<DriveCmd> Decode(const uint8_t* buffer, std::size_t size);
};

} // namespace bcnp
```

### Python Module (`bcnp_messages.py`)

```python
@dataclass
class DriveCmd:
    TYPE_ID = 1
    WIRE_SIZE = 10
    
    vx: float = 0.0
    omega: float = 0.0
    durationMs: int = 0
    
    def encode(self) -> bytes: ...
    
    @classmethod
    def decode(cls, data: bytes) -> 'DriveCmd': ...

def encode_packet(messages: List[Any], flags: int = 0) -> bytes: ...
```

---
