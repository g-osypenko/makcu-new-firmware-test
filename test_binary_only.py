import math
import struct
import time
import serial

PORT = "COM5"
BAUDRATE = 4000000

# MAK_API Opcodes
OP_DEVICE = 0x02
OP_FIRMWARE_VERSION = 0x04
OP_BUTTONS = 0x10
OP_LEFT = 0x11
OP_RIGHT = 0x12
OP_MIDDLE = 0x13
OP_MOVE = 0x18
OP_WHEEL = 0x19

DEVICE_KINDS = {
    0x01: "Mouse",
    0x02: "Keyboard",
    0x04: "Generic HID Gamepad",
    0x08: "DS4",
    0x10: "DualSense (DS5)",
    0x20: "DualSense Edge",
    0x40: "Xbox GIP",
    0x80: "Xbox 360",
}


class MakcuBinaryClient:
    """Pure binary MAK_API client over Serial (COM)."""

    def __init__(self, port: str = PORT, baudrate: int = BAUDRATE):
        self.port_name = port
        self.baudrate = baudrate
        self.ser: serial.Serial | None = None

    def connect(self):
        print(f"Connecting to {self.port_name} at {self.baudrate} baud (Pure Binary MAK_API)...")
        self.ser = serial.Serial(
            port=self.port_name,
            baudrate=self.baudrate,
            timeout=0.1,
            write_timeout=0.5,
            rtscts=False,
            dsrdtr=False,
        )
        self.ser.dtr = True
        self.ser.rts = True
        time.sleep(0.18)  # Baud open settle time
        self.ser.reset_input_buffer()
        print("[+] Serial port opened.")

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("[+] Port closed.")

    def send_frame(self, cmd: int, payload: bytes = b"") -> bytes:
        """Construct and send binary frame: DE AD | LEN:u16 | CMD:u8 | PAYLOAD"""
        frame = bytes([0xDE, 0xAD]) + struct.pack("<H", len(payload)) + bytes([cmd]) + payload
        self.ser.write(frame)
        self.ser.flush()
        return frame

    def read_frame(self, expected_cmd: int | None = None, timeout: float = 0.5) -> bytes | None:
        """Read and parse response frame: DE AD | LEN:u16 | CMD:u8 | PAYLOAD"""
        deadline = time.monotonic() + timeout
        buf = bytearray()
        while time.monotonic() < deadline:
            in_waiting = self.ser.in_waiting
            if in_waiting:
                buf.extend(self.ser.read(in_waiting))
                # Search for frame header DE AD
                idx = buf.find(b"\xDE\xAD")
                if idx >= 0 and len(buf) >= idx + 5:
                    payload_len = struct.unpack("<H", buf[idx + 2 : idx + 4])[0]
                    total_len = 5 + payload_len
                    if len(buf) >= idx + total_len:
                        resp_cmd = buf[idx + 4]
                        payload = bytes(buf[idx + 5 : idx + total_len])
                        if expected_cmd is None or resp_cmd == expected_cmd:
                            return payload
            time.sleep(0.002)
        return None

    def get_device_info(self) -> int | None:
        """Opcode 0x02: GET DEVICE -> returns bitmask of routed kinds."""
        self.send_frame(OP_DEVICE)
        resp = self.read_frame(expected_cmd=OP_DEVICE)
        if resp and len(resp) >= 1:
            return resp[0]
        return None

    def get_firmware_version(self) -> int | None:
        """Opcode 0x04: GET FIRMWARE_VERSION -> returns u32 version."""
        self.send_frame(OP_FIRMWARE_VERSION)
        resp = self.read_frame(expected_cmd=OP_FIRMWARE_VERSION)
        if resp and len(resp) >= 4:
            return struct.unpack("<I", resp[:4])[0]
        return None

    def mouse_move(self, dx: int, dy: int):
        """Opcode 0x18: SET MOVE (x:i16, y:i16). Relative displacement."""
        payload = struct.pack("<hh", int(dx), int(dy))
        self.send_frame(OP_MOVE, payload)

    def mouse_left(self, state: bool):
        """Opcode 0x11: SET LEFT (1=pressed, 0=released)."""
        self.send_frame(OP_LEFT, bytes([0x01 if state else 0x00]))

    def mouse_right(self, state: bool):
        """Opcode 0x12: SET RIGHT (1=pressed, 0=released)."""
        self.send_frame(OP_RIGHT, bytes([0x01 if state else 0x00]))

    def mouse_wheel(self, delta: int):
        """Opcode 0x19: SET WHEEL (delta:i16). Positive=up, Negative=down."""
        payload = struct.pack("<h", int(delta))
        self.send_frame(OP_WHEEL, payload)

    def mouse_click(self, button: str = "left", hold_s: float = 0.05):
        """Simulate a full click: press -> sleep -> release."""
        btn_func = self.mouse_left if button == "left" else self.mouse_right
        btn_func(True)
        time.sleep(hold_s)
        btn_func(False)


def main():
    client = MakcuBinaryClient()
    client.connect()

    try:
        print("\n=== 1. Identity & Firmware Info ===")
        # 1. Device kinds bitmask
        kinds_mask = client.get_device_info()
        if kinds_mask is not None:
            active = [name for bit, name in DEVICE_KINDS.items() if (kinds_mask & bit)]
            print(f"[*] Routed kinds mask: 0x{kinds_mask:02X} -> {', '.join(active)}")
        else:
            print("[!] Could not get device info (no reply)")

        # 2. Firmware version
        fw_ver = client.get_firmware_version()
        if fw_ver is not None:
            print(f"[*] Firmware version: {fw_ver}")
        else:
            print("[!] Could not get firmware version (no reply)")

        print("\n=== 2. Binary Mouse Movement (Smooth Circle) ===")
        print("Drawing a small smooth circle with mouse cursor...")
        radius = 40
        steps = 36
        prev_x, prev_y = 0.0, 0.0

        for i in range(1, steps + 1):
            angle = 2 * math.pi * (i / steps)
            target_x = radius * math.sin(angle)
            target_y = radius * (1.0 - math.cos(angle))
            step_dx = round(target_x - prev_x)
            step_dy = round(target_y - prev_y)
            prev_x += step_dx
            prev_y += step_dy

            client.mouse_move(step_dx, step_dy)
            time.sleep(0.01)

        print("[+] Smooth movement finished.")

        print("\n=== 3. Binary Mouse Clicks ===")
        print("Performing Left Click...")
        client.mouse_click("left")
        time.sleep(0.15)
        print("[+] Left click completed.")

        print("Performing Right Click...")
        client.mouse_click("right")
        time.sleep(0.15)
        print("[+] Right click completed.")

        print("\n=== 4. Binary Mouse Wheel ===")
        print("Scrolling Wheel Down (-120)...")
        client.mouse_wheel(-120)
        time.sleep(0.1)
        print("Scrolling Wheel Up (+120)...")
        client.mouse_wheel(120)
        print("[+] Wheel scroll completed.")

        print("\n[SUCCESS] All binary tests executed cleanly!")

    finally:
        client.close()


if __name__ == "__main__":
    main()

