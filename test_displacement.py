import ctypes, secrets, socket, time

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()

class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

def get_pos():
    pt = POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    return int(pt.x), int(pt.y)

target = ("192.168.50.175", 8080)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send_move_now(dx, dy):
    cmd = f"m.move_now({dx},{dy})\r\n".encode("ascii")
    frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"\x6B" + cmd
    packet = b"\x55" + secrets.token_bytes(8) + frame
    sock.sendto(packet, target)

def send_binary_move(dx, dy):
    import struct
    payload = struct.pack("<hh", dx, dy)
    frame = b"\xDE\xAD\x04\x00\x18" + payload
    packet = b"\x55" + secrets.token_bytes(8) + frame
    sock.sendto(packet, target)

print("=== ТЕСТ 1: Одиночные смещения m.move_now ===")
time.sleep(0.5)
for test_val in [10, 20, 50]:
    p0 = get_pos()
    send_move_now(test_val, 0)
    time.sleep(0.05)
    p1 = get_pos()
    dx_px = p1[0] - p0[0]
    ratio = dx_px / test_val if test_val else 0
    mickeys_per_px = test_val / dx_px if dx_px else 0
    print(f"m.move_now({test_val}, 0) -> Экран dx={dx_px} px | px/mickey={ratio:.3f} | mickeys/px={mickeys_per_px:.2f}x")

print("\n=== ТЕСТ 2: Серия из 10 шагов по 5 mickeys каждые 2 мс ===")
time.sleep(0.5)
p0 = get_pos()
for _ in range(10):
    send_move_now(5, 0)
    time.sleep(0.002)
time.sleep(0.05)
p1 = get_pos()
total_cmd = 50
dx_px = p1[0] - p0[0]
print(f"Сумма команд: {total_cmd} mickeys -> Экран dx={dx_px} px | mickeys/px={total_cmd/dx_px:.2f}x")

print("\n=== ТЕСТ 3: Бинарный опкод 0x18 MOVE (те же 50 mickeys) ===")
time.sleep(0.5)
p0 = get_pos()
for _ in range(10):
    send_binary_move(5, 0)
    time.sleep(0.008) # 125 Гц очередь
time.sleep(0.05)
p1 = get_pos()
dx_px = p1[0] - p0[0]
print(f"0x18 MOVE: {total_cmd} mickeys -> Экран dx={dx_px} px | mickeys/px={total_cmd/dx_px:.2f}x")

sock.close()

