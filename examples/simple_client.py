#!/usr/bin/env python3
"""
BCNP v3 Simple Client Example

Demonstrates connecting to a BCNP server, performing schema handshake,
and sending DriveCmd messages using the generated bindings.
"""

import socket
import struct
import time

# Import generated BCNP message types
from examples.generated.bcnp_messages import (
    DriveCmd,
    MessageTypeId, SCHEMA_HASH,
    encode_handshake, validate_handshake,
    encode_packet, crc32
)


def send_commands(host='127.0.0.1', port=5800):
    """Connect to BCNP server, handshake, and send drive commands."""
    print(f"BCNP v3 Client - Schema Hash: 0x{SCHEMA_HASH:08X}")
    print(f"Connecting to {host}:{port}...")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        sock.connect((host, port))
        print("Connected.")
        
        # Step 1: Send handshake
        handshake = encode_handshake()
        print(f"Sending handshake ({len(handshake)} bytes)...")
        sock.sendall(handshake)
        
        # Step 2: Receive and validate server handshake
        print("Waiting for server handshake...")
        server_handshake = sock.recv(8)
        if len(server_handshake) < 8:
            print("Error: Incomplete handshake from server")
            sock.close()
            return
            
        if not validate_handshake(server_handshake):
            remote_hash = struct.unpack('>I', server_handshake[4:8])[0]
            print(f"Error: Schema mismatch! Local=0x{SCHEMA_HASH:08X}, Remote=0x{remote_hash:08X}")
            sock.close()
            return
            
        print("Handshake complete - schemas match!")
        
        # Step 3: Send DriveCmd packet
        commands = [
            DriveCmd(vx=1.0, omega=0.0, durationMs=1000),   # Forward 1 sec
            DriveCmd(vx=0.0, omega=1.57, durationMs=500),   # Turn 90° in 0.5 sec
            DriveCmd(vx=0.5, omega=0.0, durationMs=2000),   # Forward slow 2 sec
        ]
        
        # Use clear queue flag for first packet
        packet = encode_packet(MessageTypeId.DriveCmd, commands, flags=0x01)
        print(f"Sending {len(commands)} DriveCmd messages ({len(packet)} bytes)...")
        sock.sendall(packet)
        print("Sent.")
        
        # Keep connection open briefly
        time.sleep(0.5)
        sock.close()
        print("Done.")
        
    except socket.timeout:
        print("Connection timeout.")
    except ConnectionRefusedError:
        print("Connection refused. Is the server running?")
    except Exception as e:
        print(f"Error: {e}")


if __name__ == "__main__":
    send_commands()
