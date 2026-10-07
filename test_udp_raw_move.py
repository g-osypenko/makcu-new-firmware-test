import socket
import secrets
import struct
import time

TARGET_IP = "192.168.50.175"
PORT = 8080

def send_raw_mak_api(sock, opcode: int, payload: bytes = b""):
    nonce = secrets.token_bytes(8)
    frame = b"\xDE\xAD" + len(payload).to_bytes(2, "little") + bytes([opcode]) + payload
    packet = b"\x55" + nonce + frame
    sock.sendto(packet, (TARGET_IP, PORT))

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.ioctl(0x9800000C, False)
    except:
        pass

    print(f"[*] Testing mouse movement via RAW UDP on {TARGET_IP}:{PORT}...")
    
    # Move in square: Right, Down, Left, Up
    moves = [
        (100, 0),
        (0, 100),
        (-100, 0),
        (0, -100)
    ]
    
    for i in range(3):
        for dx, dy in moves:
            send_raw_mak_api(sock, 0x18, struct.pack("<hh", dx, dy))
            time.sleep(0.05)
    
    print("[+] Mouse movement commands successfully sent via RAW UDP!")
    sock.close()

if __name__ == "__main__":
    main()

