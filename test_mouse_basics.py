import time
import struct
import serial

PORT = "COM5"
BAUDRATE = 4000000

def test_ascii(ser: serial.Serial):
    print("\n--- 1. Testing ASCII (KM_API) ---")
    
    # 1.1 Read version
    print("[ASCII] Checking version...")
    ser.write(b"\r\nkm.version()\r\n")
    time.sleep(0.1)
    resp = ser.read_all().decode(errors="ignore")
    print(f"[ASCII] Response: {repr(resp)}")

    # 1.2 Mouse movement: small square (+50, 0) -> (0, +50) -> (-50, 0) -> (0, -50)
    print("[ASCII] Moving mouse in a square...")
    steps = [(50, 0), (0, 50), (-50, 0), (0, -50)]
    for dx, dy in steps:
        cmd = f"km.move({dx},{dy})\r\n".encode("ascii")
        ser.write(cmd)
        time.sleep(0.05)
    print("[ASCII] Mouse movement sent!")

    # 1.3 Mouse click: km.left(1) -> sleep -> km.left(0)
    print("[ASCII] Performing left mouse click...")
    ser.write(b"km.left(1)\r\n")
    time.sleep(0.06)
    ser.write(b"km.left(0)\r\n")
    time.sleep(0.05)
    print("[ASCII] Click completed!")


def test_binary(ser: serial.Serial):
    print("\n--- 2. Testing Binary (MAK_API) ---")

    # Frame helper: DE AD | LEN:u16 | CMD:u8 | PAYLOAD
    def send_frame(cmd: int, payload: bytes = b""):
        frame = bytes([0xDE, 0xAD]) + struct.pack("<H", len(payload)) + bytes([cmd]) + payload
        ser.write(frame)

    # 2.1 DEVICE query (CMD=0x01)
    print("[Binary] Querying DEVICE (0x01)...")
    send_frame(0x01)
    time.sleep(0.1)
    resp = ser.read_all()
    print(f"[Binary] Device response: {resp.hex(' ')}")

    # 2.2 Mouse movement (CMD=0x18, payload: x:i16, y:i16)
    print("[Binary] Moving mouse diagonally (+40, +40) then back (-40, -40)...")
    payload_forward = struct.pack("<hh", 40, 40)
    send_frame(0x18, payload_forward)
    time.sleep(0.1)

    payload_backward = struct.pack("<hh", -40, -40)
    send_frame(0x18, payload_backward)
    time.sleep(0.1)
    print("[Binary] Mouse movement sent!")

    # 2.3 Mouse click (CMD=0x11 LEFT: 0x01 press, 0x00 release)
    print("[Binary] Performing left click (press 0x01, release 0x00)...")
    send_frame(0x11, bytes([0x01]))
    time.sleep(0.06)
    send_frame(0x11, bytes([0x00]))
    time.sleep(0.05)
    print("[Binary] Click completed!")


def main():
    print(f"Connecting to {PORT} at {BAUDRATE} baud...")
    ser = serial.Serial(
        port=PORT,
        baudrate=BAUDRATE,
        timeout=0.2,
        rtscts=False,
        dsrdtr=False,
    )
    ser.dtr = True
    ser.rts = True
    time.sleep(0.15)
    ser.reset_input_buffer()

    try:
        test_ascii(ser)
        time.sleep(0.3)
        test_binary(ser)
        print("\n[SUCCESS] All basic mouse tests completed!")
    finally:
        ser.close()
        print(f"Closed {PORT}.")


if __name__ == "__main__":
    main()

