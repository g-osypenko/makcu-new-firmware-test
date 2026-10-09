#!/usr/bin/env python3
"""
Автоматический тест повторяемости (5 прогонов подряд).
Замеряет дельту 5 раз подряд с паузой 1 секунда, чтобы исключить влияние руки
и проверить, плавает ли результат сам по себе.
"""

import ctypes
import secrets
import socket
import struct
import sys
import time

TARGET = ("192.168.50.175", 8080)

try:
    ctypes.windll.user32.SetProcessDPIAware()
except Exception:
    pass

class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

def get_cursor_pos() -> tuple[int, int]:
    pt = POINT()
    res = ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
    return (int(pt.x), int(pt.y)) if res else (0, 0)

def make_move_now_packet(dx: int, dy: int) -> bytes:
    cmd = f"m.move_now({int(dx)},{int(dy)})\r\n".encode("ascii")
    frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"\x6B" + cmd
    return b"\x55" + secrets.token_bytes(8) + frame

def make_opcode_18_packet(dx: int, dy: int) -> bytes:
    payload = struct.pack("<hh", int(dx), int(dy))
    frame = b"\xDE\xAD\x04\x00\x18" + payload
    return b"\x55" + secrets.token_bytes(8) + frame

def run_test_series(mode: str, hz: int, total_mickeys: int = 300, count: int = 30):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    step = int(total_mickeys / count)
    dt = 1.0 / hz
    use_move_now = (mode == "move_now")
    
    print("\n" + "=" * 65)
    print(f" ТЕСТ СТАБИЛЬНОСТИ: 5 прогонов подряд ({mode}, {hz} Гц, шаг {step}x{count}={total_mickeys} mickeys)")
    print(" ВНИМАНИЕ: НЕ ТРОГАЙ МЫШЬ РУКОЙ ВО ВРЕМЯ ТЕСТА!")
    print("=" * 65)
    
    time.sleep(1.0)
    
    results = []
    for run in range(1, 6):
        # Ждем полной тишины перед стартом
        time.sleep(0.5)
        p0 = get_cursor_pos()
        
        t0 = time.perf_counter()
        for i in range(1, count + 1):
            pkt = make_move_now_packet(step, 0) if use_move_now else make_opcode_18_packet(step, 0)
            sock.sendto(pkt, TARGET)
            
            target_t = t0 + i * dt
            while time.perf_counter() < target_t:
                pass
                
        time.sleep(0.15)
        p1 = get_cursor_pos()
        dx = p1[0] - p0[0]
        mickeys_delivered = dx / 1.5  # 150% DPI
        results.append((dx, mickeys_delivered))
        print(f"  Прогон #{run}: Экран dx = {dx:4d} px  |  Дошло mickeys = {mickeys_delivered:5.1f} / {total_mickeys}")
        
    sock.close()
    
    avg_dx = sum(r[0] for r in results) / len(results)
    min_dx = min(r[0] for r in results)
    max_dx = max(r[0] for r in results)
    print("-" * 65)
    print(f" ИТОГ: Среднее = {avg_dx:.1f} px, Мин = {min_dx} px, Макс = {max_dx} px, Разброс = {max_dx - min_dx} px")
    print("=" * 65)

if __name__ == "__main__":
    print("\nВыберите серию тестов:")
    print(" [1] 5 прогонов m.move_now на 500 Гц (2 мс)")
    print(" [2] 5 прогонов 0x18 MOVE на 125 Гц (8 мс, 200 mickeys = 300 px)")
    print(" [3] 5 прогонов 0x18 MOVE на 500 Гц (2 мс)")
    print(" [0] Выход")
    ch = input("Выбор: ").strip()
    
    if ch == "1":
        run_test_series("move_now", hz=500, total_mickeys=300, count=30)
    elif ch == "2":
        run_test_series("0x18", hz=125, total_mickeys=200, count=20)
    elif ch == "3":
        run_test_series("0x18", hz=500, total_mickeys=300, count=30)

