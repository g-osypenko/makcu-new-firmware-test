#!/usr/bin/env python3
"""
MAKCU V4: Прямая линейная нарезка БЕЗ S-кривых.

Демонстрирует разницу между нейросетевым рендерером (который добавляет 
свою инерцию) и чистым математическим накопителем.
"""

import ctypes
import secrets
import socket
import struct
import sys
import time
from pathlib import Path
import numpy as np

_ABC_ROOT = Path(__file__).resolve().parents[1] / "ABCurves"
sys.path.insert(0, str(_ABC_ROOT))
from abcurves.portable_renderer import CONTEXT_TICKS, PortableRendererModel

BOARD = ("192.168.50.175", 8080)

def make_move_packet(dx: int, dy: int) -> bytes:
    """RAW UDP пакет 0x18."""
    return b"\x55" + secrets.token_bytes(8) + b"\xDE\xAD\x04\x00\x18" + struct.pack("<hh", int(dx), int(dy))

if sys.platform == "win32":
    try: ctypes.windll.winmm.timeBeginPeriod(1)
    except Exception: pass

# ── Обертки нарезчиков ──────────────────────────────────────────────────────
class NeuralStepper:
    """Нейросеть ABCurves. Имеет внутреннюю инерцию (momentum)."""
    def __init__(self):
        m = PortableRendererModel(
            _ABC_ROOT / "models" / "renderer_global_h80.bin", 
            library=_ABC_ROOT / "abcurves" / "_native" / "abcurves_renderer.dll"
        )
        self.stream = m.prepare_context(np.zeros((CONTEXT_TICKS, 2), np.int16)).begin_stream(event_seed=secrets.randbits(32))

    def step(self, dx: float, dy: float) -> tuple[int, int]:
        r = self.stream.step([float(dx), float(dy)])
        return int(r[0]), int(r[1])


class Q16Accumulator:
    """Чистая математика. Без инерции, без лагов в конце."""
    def __init__(self):
        self.acc_x, self.acc_y = 0.0, 0.0

    def step(self, dx: float, dy: float) -> tuple[int, int]:
        self.acc_x += dx
        self.acc_y += dy
        ox, oy = round(self.acc_x), round(self.acc_y)
        self.acc_x -= ox
        self.acc_y -= oy
        return int(ox), int(oy)


# ── Исполнитель ──────────────────────────────────────────────────────────────
def execute_linear(sock, stepper, total_dx: float, total_dy: float, duration_s: float, hz: int = 1000, udp_rate: int = 250):
    ticks = int(duration_s * hz)
    step_dx = total_dx / ticks
    step_dy = total_dy / ticks
    
    ticks_per_send = max(1, hz // udp_rate)
    sum_dx, sum_dy = 0, 0
    t0 = time.perf_counter()
    
    accum_dx, accum_dy = 0, 0
    
    for i in range(1, ticks + 1):
        dx, dy = stepper.step(step_dx, step_dy)
        sum_dx += dx
        sum_dy += dy
        accum_dx += dx
        accum_dy += dy
        
        if i % ticks_per_send == 0:
            if accum_dx != 0 or accum_dy != 0:
                sock.sendto(make_move_packet(accum_dx, accum_dy), BOARD)
            accum_dx, accum_dy = 0, 0
            
        target = t0 + i * (1.0 / hz)
        while time.perf_counter() < target:
            pass

    if accum_dx != 0 or accum_dy != 0:
        sock.sendto(make_move_packet(accum_dx, accum_dy), BOARD)
        accum_dx, accum_dy = 0, 0

    tail_dx, tail_dy = 0, 0
    for i in range(1, 51):
        dx, dy = stepper.step(0.0, 0.0)
        sum_dx += dx
        sum_dy += dy
        tail_dx += dx
        tail_dy += dy
        accum_dx += dx
        accum_dy += dy
        
        if i % ticks_per_send == 0:
            if accum_dx != 0 or accum_dy != 0:
                sock.sendto(make_move_packet(accum_dx, accum_dy), BOARD)
            accum_dx, accum_dy = 0, 0
            
        target += (1.0 / hz)
        while time.perf_counter() < target:
            pass

    if accum_dx != 0 or accum_dy != 0:
        sock.sendto(make_move_packet(accum_dx, accum_dy), BOARD)

    return sum_dx, sum_dy, tail_dx, tail_dy
def main():
    print("=" * 60)
    print(" MAKCU V4: Чистая Линейная Нарезка")
    print("=" * 60)
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    try:
        while True:
            print("\n [1] +300 px через ABCurves Renderer (Будет лаг/инерция в конце)")
            print(" [2] +300 px через Q16 Accumulator   (Идеальная математика, 0 лагов)")
            print(" [0] Выход")
            ch = input("→ ").strip()

            if ch in ("1", "2"):
                stepper = NeuralStepper() if ch == "1" else Q16Accumulator()
                
                print(f"[*] Поехали...")
                total_x, total_y, tail_x, tail_y = execute_linear(sock, stepper, 300, 0, duration_s=0.35, hz=1000)
                
                print(f"[+] Итого выдано навигатором: X={total_x}, Y={total_y} (Цель: 300)")
                print(f"[!] Из них в 'хвосте' (инерция после остановки): {tail_x} px")
                
                if total_x != 300:
                    print("  -> ВНИМАНИЕ: Навигатор потерял пиксели!")
            elif ch == "0":
                break
    finally:
        sock.close()
        if sys.platform == "win32":
            try: ctypes.windll.winmm.timeEndPeriod(1)
            except Exception: pass

if __name__ == "__main__":
    main()
