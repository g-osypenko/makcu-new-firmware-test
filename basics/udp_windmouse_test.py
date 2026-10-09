import socket
import struct
import secrets
import time
import sys
from windmouse_smooth_example import WindMouse

BOARD = ("192.168.50.175", 8080)

def make_move_packet(dx: int, dy: int) -> bytes:
    """RAW UDP in 0x18 opcode."""
    return b"\x55" + secrets.token_bytes(8) + b"\xDE\xAD\x04\x00\x18" + struct.pack("<hh", int(dx), int(dy))

def execute_windmouse_udp(sock, target_dx: float, target_dy: float):
    # Initialize WindMouse
    wm = WindMouse()
    
    # Standard human-like parameters
    gravity = 9.0
    wind = 3.0
    min_wait = 0.005  # 5 ms min delay between steps
    max_wait = 0.015  # 15 ms max delay between steps
    max_step = 10.0   # max pixels per step
    target_area = 2.0
    
    print(f"[*] Generating WindMouse path for {target_dx}x{target_dy}...")
    
    # Generate the path
    path = wm.wind_mouse(
        0, 0, target_dx, target_dy,
        gravity=gravity,
        wind=wind,
        min_wait=min_wait,
        max_wait=max_wait,
        max_step=max_step,
        target_area=target_area
    )
    
    print(f"[*] Path generated with {len(path)} steps. Executing over UDP...")
    
    sum_dx, sum_dy = 0, 0
    start_t = time.perf_counter()
    
    # Execute the generated path
    for dx, dy, delay in path:
        sum_dx += dx
        sum_dy += dy
        
        if dx != 0 or dy != 0:
            sock.sendto(make_move_packet(dx, dy), BOARD)
            
        # Pacing (spinwait for accuracy instead of sleep)
        target_time = time.perf_counter() + delay
        while time.perf_counter() < target_time:
            pass

    elapsed = time.perf_counter() - start_t
    print(f"[+] Execution finished in {elapsed:.3f}s")
    print(f"[+] Total Mickey Sent: X={sum_dx}, Y={sum_dy} (Target: {target_dx})")
    
    return sum_dx, sum_dy

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        while True:
            print("\n [1] +300 px using WindMouse (UDP 0x18)")
            print(" [0] Exit")
            ch = input("Select: ").strip()

            if ch == "1":
                execute_windmouse_udp(sock, 300, 0)
            elif ch == "0":
                break
    finally:
        sock.close()

if __name__ == "__main__":
    main()

