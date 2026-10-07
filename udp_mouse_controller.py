"""
MAKCU UDP Mouse Controller (Single PC / Pure Ethernet / No COM port)
Hardware setup:
  - USB 1: PC (5V power + HID output)
  - USB 2: DISCONNECTED (No COM port in Windows!)
  - USB 3: UGREEN USB Hub with RJ45 + Physical Mouse
"""

import sys
import time

sys.path.insert(0, r"c:\makcu-new-firmware-test\mak-suite\python")
from makxd import create_controller, ConnectionConfig, UdpWireMode, MouseButton

TARGET_IP = "192.168.50.175"
PORT = 8080

def main():
    print(f"=== MAKCU Pure UDP Controller ===")
    print(f"Target: {TARGET_IP}:{PORT} (UdpWireMode.RAW)")
    
    # Critical setting: mode=UdpWireMode.RAW is required for MAKCU Ethernet proxy!
    cfg = ConnectionConfig.udp(
        host=TARGET_IP,
        port=PORT,
        mode=UdpWireMode.RAW
    )

    device = create_controller(connection=cfg)
    try:
        kinds = device.device()
        print(f"[+] Connected! Device Kinds: {kinds}")
        print(f"[+] Firmware Version: {device.firmware_version()}")

        print("\n[*] 1. Moving mouse in a square...")
        steps = [
            (80, 0),
            (0, 80),
            (-80, 0),
            (0, -80)
        ]
        for dx, dy in steps:
            device.move(dx, dy)
            time.sleep(0.08)

        print("[*] 2. Testing Left Click (Press -> Sleep -> Release)...")
        device.press(MouseButton.LEFT)
        time.sleep(0.06)
        device.release(MouseButton.LEFT)
        print("[+] Click completed!")

        print("\n[SUCCESS] Everything is working exclusively over UDP without COM port!")

    finally:
        device.disconnect()
        print("[+] Disconnected cleanly.")

if __name__ == "__main__":
    main()

