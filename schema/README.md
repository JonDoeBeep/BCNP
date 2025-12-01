# BCNP Schema Codegen

This directory contains the BCNP message schema and code generation tools.

## Files

- `bcnp_schema.json` - JSON Schema for validating messages.json
- `messages.json` - Message type definitions (edit this to add new types)
- `bcnp_codegen.py` - Code generator script

## Usage

```bash
# Generate C++ header and Python bindings
python bcnp_codegen.py messages.json --cpp ../src/bcnp/generated --python ../examples

# View schema info without generating
python bcnp_codegen.py messages.json
```

## Adding a New Message Type

1. Edit `messages.json` and add a new entry to the `messages` array:

```json
{
  "id": 10,
  "name": "MyNewCmd",
  "description": "My custom command",
  "fields": [
    {"name": "value1", "type": "float32", "scale": 10000},
    {"name": "value2", "type": "uint16"}
  ]
}
```

2. Run codegen to regenerate headers
3. Rebuild C++ project
4. Update Python clients

## Supported Types

| Type | Size | Description |
|------|------|-------------|
| int8 | 1 | Signed 8-bit integer |
| uint8 | 1 | Unsigned 8-bit integer |
| int16 | 2 | Signed 16-bit integer (big-endian) |
| uint16 | 2 | Unsigned 16-bit integer (big-endian) |
| int32 | 4 | Signed 32-bit integer (big-endian) |
| uint32 | 4 | Unsigned 32-bit integer (big-endian) |
| float32 | 4 | Float encoded as int32 with scale factor |

## Schema Hash

The schema hash is a CRC32 of the canonical JSON representation. It ensures:
- Client and server use the same message definitions
- Version mismatches are detected before data corruption
- Changes to field order, types, or counts invalidate the hash

Current hash is printed when running codegen and embedded in generated code.
