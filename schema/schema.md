# BCNP Schema

Define messages in JSON, generate C++ and Python code.

## Files

| File | Purpose |
|------|---------|
| `messages.json` | Message definitions (edit this) |
| `bcnp_schema.json` | JSON Schema for validation |
| `bcnp_codegen.py` | Code generator |

## Usage

```bash
python bcnp_codegen.py messages.json --cpp ../generated --python ../examples
```

## Message Format

```json
{
  "id": 1,
  "name": "DriveCmd",
  "description": "Differential drive command",
  "fields": [
    {"name": "vx", "type": "float32", "scale": 10000, "unit": "m/s"},
    {"name": "omega", "type": "float32", "scale": 10000, "unit": "rad/s"},
    {"name": "durationMs", "type": "uint16", "unit": "ms"}
  ]
}
```

| Property | Required | Description |
|----------|----------|-------------|
| `id` | Yes | Unique ID (1-65535) |
| `name` | Yes | PascalCase identifier |
| `description` | No | Documentation |
| `fields` | Yes | Array of field definitions |

### Field Properties

| Property | Required | Description |
|----------|----------|-------------|
| `name` | Yes | camelCase identifier |
| `type` | Yes | One of supported types |
| `scale` | No | For float32 only |
| `unit` | No | Documentation only |

## Field Types

| Type | Size | Description |
|------|------|-------------|
| `int8` | 1B | Signed 8-bit |
| `uint8` | 1B | Unsigned 8-bit |
| `int16` | 2B | Signed 16-bit (big-endian) |
| `uint16` | 2B | Unsigned 16-bit (big-endian) |
| `int32` | 4B | Signed 32-bit (big-endian) |
| `uint32` | 4B | Unsigned 32-bit (big-endian) |
| `float32` | 4B | int32 × scale factor |

## Generated Output

**C++** (`message_types.h`):
```cpp
struct DriveCmd {
    static constexpr MessageTypeId kTypeId = MessageTypeId::DriveCmd;
    static constexpr std::size_t kWireSize = 10;
    
    float vx{0.0f};
    float omega{0.0f};
    uint16_t durationMs{0};
    
    bool Encode(uint8_t* buffer, std::size_t size) const;
    static std::optional<DriveCmd> Decode(const uint8_t* buffer, std::size_t size);
};
```

**Python** (`bcnp_messages.py`):
```python
@dataclass
class DriveCmd:
    TYPE_ID = 1
    WIRE_SIZE = 10
    
    vx: float = 0.0
    omega: float = 0.0
    durationMs: int = 0
```

## Schema Hash

CRC32 of canonical JSON. Printed during codegen:

```
Schema hash: 0x0152BCDB
```

Both endpoints must match.
