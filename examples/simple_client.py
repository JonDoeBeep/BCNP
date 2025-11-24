import socket
import struct
import zlib
import time
import math

def send_command(host='127.0.0.1', port=5800):
    print(f"Connecting to {host}:{port}...")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print("Connected.")
        
        # BCNP Constants
        PROTOCOL_MAJOR = 2
        PROTOCOL_MINOR = 4
        FLAG_CLEAR_QUEUE = 0x01
        LINEAR_SCALE = 10000.0
        ANGULAR_SCALE = 10000.0
        
        # Command: Move forward at 1.0 m/s for 2000ms
        vx = 1.0
        omega = 0.0
        duration_ms = 2000
        
        # Encode command
        vx_fixed = int(round(vx * LINEAR_SCALE))
        omega_fixed = int(round(omega * ANGULAR_SCALE))
        
        # Build Packet
        # Header: Major(1), Minor(1), Flags(1), Count(2)
        packet = bytearray()
        packet.extend(struct.pack('>BBBH', PROTOCOL_MAJOR, PROTOCOL_MINOR, FLAG_CLEAR_QUEUE, 1))
        
        # Command: Vx(4), Omega(4), Duration(2)
        packet.extend(struct.pack('>iiH', vx_fixed, omega_fixed, duration_ms))
        
        # CRC32 (4)
        crc = zlib.crc32(packet) & 0xFFFFFFFF
        packet.extend(struct.pack('>I', crc))
        
        print(f"Sending packet ({len(packet)} bytes)...")
        sock.sendall(packet)
        print("Sent.")
        
        # Keep connection open briefly to allow server to process
        time.sleep(1.0)
        sock.close()
        print("Done.")
        
    except ConnectionRefusedError:
        print("Connection refused. Is the server running?")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    send_command()
