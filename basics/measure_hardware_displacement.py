#!/usr/bin/env python3
"""
Аппаратный калибратор смещения MAKCU V4 (Open-Loop Ground Truth).
Замеряет реальное соотношение [mickeys отправлено] -> [экранных пикселей пройдено]
на чистом бинарном опкоде 0x18 и m.move_now без каких-либо обёрток и кривых.
"""

import ctypes
import secrets
import socket
import struct
import sys
import time

TARGET = ("192.168.50.175", 8080)

# Включаем DPI Awareness для честных 4K пикселей
try:
    ctypes.windll.user32.SetProcessDPIAware()
except Exception:
    pass

class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]

def get_cursor_pos() -> tuple[int, int]:
    pt = POINT()
    res = ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
    if not res:
        return 0, 0
    return int(pt.x), int(pt.y)

def make_opcode_18_packet(dx: int, dy: int) -> bytes:
    payload = struct.pack("<hh", int(dx), int(dy))
    frame = b"\xDE\xAD\x04\x00\x18" + payload
    return b"\x55" + secrets.token_bytes(8) + frame

def make_move_now_packet(dx: int, dy: int) -> bytes:
    cmd = f"m.move_now({int(dx)},{int(dy)})\r\n".encode("ascii")
    frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"\x6B" + cmd
    return b"\x55" + secrets.token_bytes(8) + frame

def run_single_pulse_test(sock: socket.socket, test_mickeys: int, use_move_now: bool = False):
    name = "m.move_now" if use_move_now else "0x18 MOVE"
    print(f"\n--- ТЕСТ: Одиночный импульс {test_mickeys} mickeys ({name}) ---")
    
    # Ждём стабилизации курсора
    time.sleep(0.3)
    p0 = get_cursor_pos()
    
    pkt = make_move_now_packet(test_mickeys, 0) if use_move_now else make_opcode_18_packet(test_mickeys, 0)
    sock.sendto(pkt, TARGET)
    
    # Даем USB и DWM время обработать отчёт
    time.sleep(0.1)
    p1 = get_cursor_pos()
    
    dx_px = p1[0] - p0[0]
    dy_px = p1[1] - p0[1]
    
    ratio = (dx_px / test_mickeys) if test_mickeys else 0
    print(f"  Старт: {p0} -> Финиш: {p1}")
    print(f"  Экранная дельта: dx={dx_px} px, dy={dy_px} px")
    print(f"  Коэффициент: {ratio:.3f} px / mickey  (1 px = {1.0/ratio:.3f} mickeys)" if ratio else "  [!] Курсор не сдвинулся")
    return dx_px

def run_stream_pulse_test(sock: socket.socket, total_mickeys: int, count: int, interval_s: float, use_move_now: bool = False):
    name = "m.move_now" if use_move_now else "0x18 MOVE"
    hz = 1.0 / interval_s if interval_s > 0 else 0
    step = int(total_mickeys / count)
    print(f"\n--- ТЕСТ: Серия {count} шагов по {step} mickeys (Всего: {total_mickeys}, {hz:.0f} Гц, {name}) ---")
    
    time.sleep(0.3)
    p0 = get_cursor_pos()
    
    t0 = time.perf_counter()
    for i in range(1, count + 1):
        pkt = make_move_now_packet(step, 0) if use_move_now else make_opcode_18_packet(step, 0)
        sock.sendto(pkt, TARGET)
        
        target_t = t0 + i * interval_s
        while time.perf_counter() < target_t:
            pass
            
    time.sleep(0.1)
    p1 = get_cursor_pos()
    
    dx_px = p1[0] - p0[0]
    dy_px = p1[1] - p0[1]
    ratio = (dx_px / total_mickeys) if total_mickeys else 0
    print(f"  Старт: {p0} -> Финиш: {p1}")
    print(f"  Экранная дельта: dx={dx_px} px, dy={dy_px} px (Задано: {total_mickeys})")
    print(f"  Коэффициент: {ratio:.3f} px / mickey" if ratio else "  [!] Курсор не сдвинулся")
    return dx_px

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print("=" * 65)
    print(" MAKCU V4: АППАРАТНЫЙ ЗАМЕР СМЕЩЕНИЯ (OPEN-LOOP GROUND TRUTH)")
    print("=" * 65)
    
    try:
        while True:
            print("\nВыберите тест:")
            print(" [1] Одиночный импульс +100 mickeys через 0x18 MOVE")
            print(" [2] Одиночный импульс +300 mickeys через 0x18 MOVE")
            print(" [3] Одиночный импульс +300 mickeys через m.move_now")
            print(" [4] Серия: 300 mickeys (30 по 10) на 500 Гц (2 мс) через 0x18")
            print(" [5] Серия: 300 mickeys (30 по 10) на 1000 Гц (1 мс) через 0x18")
            print(" [6] Серия: 300 mickeys (30 по 10) на 125 Гц (8 мс) через 0x18")
            print(" [7] Серия: 300 mickeys (30 по 10) на 500 Гц (2 мс) через m.move_now")
            print(" [8] Серия: 300 mickeys (30 по 10) на 125 Гц (8 мс) через m.move_now")
            print(" [9] Калиброванный тест: 200 mickeys (цель ровно 300 px) на 125 Гц через 0x18")
            print(" [0] Выход")
            ch = input("Выбор: ").strip()
            
            if ch == "1":
                run_single_pulse_test(sock, 100, use_move_now=False)
            elif ch == "2":
                run_single_pulse_test(sock, 300, use_move_now=False)
            elif ch == "3":
                run_single_pulse_test(sock, 300, use_move_now=True)
            elif ch == "4":
                run_stream_pulse_test(sock, 300, count=30, interval_s=0.002, use_move_now=False)
            elif ch == "5":
                run_stream_pulse_test(sock, 300, count=30, interval_s=0.001, use_move_now=False)
            elif ch == "6":
                run_stream_pulse_test(sock, 300, count=30, interval_s=0.008, use_move_now=False)
            elif ch == "7":
                run_stream_pulse_test(sock, 300, count=30, interval_s=0.002, use_move_now=True)
            elif ch == "8":
                run_stream_pulse_test(sock, 300, count=30, interval_s=0.008, use_move_now=True)
            elif ch == "9":
                run_stream_pulse_test(sock, 200, count=20, interval_s=0.008, use_move_now=False)
            elif ch == "0":
                break
    finally:
        sock.close()

if __name__ == "__main__":
    main()

