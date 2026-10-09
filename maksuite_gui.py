#!/usr/bin/env python3
"""
MAKCU Direct GUI — Минималистичный и чистый интерфейс на чистом бинарном опкоде 0x18 (MAK_API).
БЕЗ нейросетей, БЕЗ ABCurves, БЕЗ C-DLL, БЕЗ тяжелых оберток dev.move.
Прямой бинарный сокет UDP RAW.
"""

import math
import sys
import threading
import time
import struct
import socket
import secrets
import ctypes
import tkinter as tk
from tkinter import ttk, messagebox

BOARD_IP = "192.168.50.175"
BOARD_PORT = 8080
TARGET = (BOARD_IP, BOARD_PORT)

OPCODE_MOVE = 0x18
OPCODE_LEFT = 0x11

_winmm = None
if sys.platform == "win32":
    try:
        _winmm = ctypes.WinDLL("winmm")
        _winmm.timeBeginPeriod(1)
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00000080)
    except Exception:
        pass


def precise_sleep_until(target_time: float):
    rem = target_time - time.perf_counter()
    if rem > 0.002:
        time.sleep(rem - 0.0015)
    while time.perf_counter() < target_time:
        pass


class MakcuDirectGui(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("MAKCU Direct (Опкод 0x18)")
        self.geometry("380x460")
        self.resizable(False, False)

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
        self.sock.settimeout(0.5)

        self.worker_thread = None
        self.stop_event = threading.Event()

        self._create_widgets()
        self._check_connection()

    def _create_widgets(self):
        pad_opts = {"padx": 10, "pady": 4}

        # Статус
        self.lbl_status = ttk.Label(self, text="Проверка связи...", font=("Segoe UI", 10, "bold"))
        self.lbl_status.pack(fill="x", **pad_opts)

        # Выбор частоты
        f_frame = ttk.LabelFrame(self, text="Частота полинга (Опкод 0x18)")
        f_frame.pack(fill="x", **pad_opts)

        self.var_hz = tk.IntVar(value=500)
        r1 = ttk.Radiobutton(f_frame, text="100 Гц (10.0 мс)", variable=self.var_hz, value=100)
        r2 = ttk.Radiobutton(f_frame, text="125 Гц (8.0 мс — нативный такт MAKCU)", variable=self.var_hz, value=125)
        r3 = ttk.Radiobutton(f_frame, text="500 Гц (2.0 мс — Sweet Spot)", variable=self.var_hz, value=500)
        r4 = ttk.Radiobutton(f_frame, text="1000 Гц (1.0 мс — High-Speed)", variable=self.var_hz, value=1000)
        r1.pack(anchor="w", padx=5, pady=1)
        r2.pack(anchor="w", padx=5, pady=1)
        r3.pack(anchor="w", padx=5, pady=1)
        r4.pack(anchor="w", padx=5, pady=1)

        # Кнопки действий
        btn_frame = ttk.LabelFrame(self, text="Тестовые фигуры")
        btn_frame.pack(fill="both", expand=True, **pad_opts)

        self.btn_circle = ttk.Button(btn_frame, text="⭕ Круг обычный (R=120, 2.0 сек)", command=lambda: self._start_pattern("circle_120"))
        self.btn_circle.pack(fill="x", padx=5, pady=2)

        self.btn_big_circle = ttk.Button(btn_frame, text="⭕ Большой круг (R=250, 2.0 сек)", command=lambda: self._start_pattern("circle_250"))
        self.btn_big_circle.pack(fill="x", padx=5, pady=2)

        self.btn_fast_circle = ttk.Button(btn_frame, text="⚡ Быстрый круг (R=250, 1.2 сек — 1000Гц)", command=lambda: self._start_pattern("circle_fast"))
        self.btn_fast_circle.pack(fill="x", padx=5, pady=2)

        self.btn_line_x = ttk.Button(btn_frame, text="↔️ Линия X (+-200 отсчётов)", command=lambda: self._start_pattern("line_x"))
        self.btn_line_x.pack(fill="x", padx=5, pady=2)

        self.btn_line_y = ttk.Button(btn_frame, text="↕️ Линия Y (+-200 отсчётов)", command=lambda: self._start_pattern("line_y"))
        self.btn_line_y.pack(fill="x", padx=5, pady=2)

        self.btn_loop = ttk.Button(btn_frame, text="🔄 Бесконечный круг (Loop)", command=lambda: self._start_pattern("loop"))
        self.btn_loop.pack(fill="x", padx=5, pady=2)

        self.btn_stop = ttk.Button(self, text="🛑 СТОП (Остановить)", command=self._stop_pattern)
        self.btn_stop.pack(fill="x", padx=10, pady=6)

        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _check_connection(self):
        frame = b"\xDE\xAD\x00\x00\x04"
        packet = b"\x55" + secrets.token_bytes(8) + frame
        try:
            self.sock.sendto(packet, TARGET)
            resp, _ = self.sock.recvfrom(1024)
            if len(resp) >= 18 and resp[9:11] == b"\xDE\xAD" and resp[13] == 0x04:
                fw = struct.unpack("<I", resp[14:18])[0]
                self.lbl_status.config(text=f"🟢 MAKCU V{fw} ({BOARD_IP})", foreground="green")
                return
        except Exception:
            pass
        self.lbl_status.config(text=f"🟡 Готов (UDP {BOARD_IP}:8080)", foreground="blue")

    def _send_move(self, dx: int, dy: int):
        if dx == 0 and dy == 0:
            return
        payload = struct.pack("<hh", int(dx), int(dy))
        frame = b"\xDE\xAD\x04\x00\x18" + payload
        packet = b"\x55" + secrets.token_bytes(8) + frame
        self.sock.sendto(packet, TARGET)

    def _start_pattern(self, pattern_type: str):
        self._stop_pattern()
        self.stop_event.clear()
        self.worker_thread = threading.Thread(target=self._run_worker, args=(pattern_type,), daemon=True)
        self.worker_thread.start()

    def _stop_pattern(self):
        self.stop_event.set()
        if self.worker_thread and self.worker_thread.is_alive():
            self.worker_thread.join(timeout=0.3)
        self.worker_thread = None

    def _run_worker(self, pattern_type: str):
        hz = self.var_hz.get()
        dt = 1.0 / hz

        if pattern_type in ("circle_120", "circle_250", "circle_fast"):
            radius = 120 if pattern_type == "circle_120" else 250
            duration = 1.2 if pattern_type == "circle_fast" else 2.0
            steps = max(10, int(duration * hz))

            prev_x, prev_y = float(radius), 0.0
            t0 = time.perf_counter()

            for step in range(1, steps + 1):
                if self.stop_event.is_set():
                    break
                theta = 2.0 * math.pi * (step / steps)
                cur_x = radius * math.cos(theta)
                cur_y = radius * math.sin(theta)

                dx = int(round(cur_x - prev_x))
                dy = int(round(cur_y - prev_y))
                prev_x += dx
                prev_y += dy

                self._send_move(dx, dy)
                precise_sleep_until(t0 + step * dt)

        elif pattern_type == "loop":
            radius = 120
            period = 2.0
            steps_per_loop = max(10, int(period * hz))
            step = 0
            prev_x, prev_y = float(radius), 0.0
            t0 = time.perf_counter()

            while not self.stop_event.is_set():
                step += 1
                theta = 2.0 * math.pi * ((step % steps_per_loop) / steps_per_loop)
                cur_x = radius * math.cos(theta)
                cur_y = radius * math.sin(theta)

                dx = int(round(cur_x - prev_x))
                dy = int(round(cur_y - prev_y))
                prev_x += dx
                prev_y += dy

                self._send_move(dx, dy)
                precise_sleep_until(t0 + step * dt)

                if (t0 + step * dt - time.perf_counter()) < -0.5:
                    t0 = time.perf_counter() - (step * dt)

        elif pattern_type in ("line_x", "line_y"):
            total_dx = 200 if pattern_type == "line_x" else 0
            total_dy = 200 if pattern_type == "line_y" else 0
            duration = 1.0
            half_steps = max(5, int((duration / 2.0) * hz))
            half_dt = (duration / 2.0) / half_steps

            accum_x, accum_y = 0, 0
            t0 = time.perf_counter()

            for s in range(1, half_steps + 1):
                if self.stop_event.is_set():
                    break
                target_x = int(round(total_dx * (s / half_steps)))
                target_y = int(round(total_dy * (s / half_steps)))
                dx = target_x - accum_x
                dy = target_y - accum_y
                accum_x += dx
                accum_y += dy
                self._send_move(dx, dy)
                precise_sleep_until(t0 + s * half_dt)

            time.sleep(0.05)
            t0 = time.perf_counter()

            for s in range(1, half_steps + 1):
                if self.stop_event.is_set():
                    break
                target_x = int(round(total_dx * (1.0 - s / half_steps)))
                target_y = int(round(total_dy * (1.0 - s / half_steps)))
                dx = target_x - accum_x
                dy = target_y - accum_y
                accum_x += dx
                accum_y += dy
                self._send_move(dx, dy)
                precise_sleep_until(t0 + s * half_dt)

    def _on_close(self):
        self._stop_pattern()
        try:
            self.sock.close()
        except Exception:
            pass
        if _winmm is not None:
            try:
                _winmm.timeEndPeriod(1)
            except Exception:
                pass
        self.destroy()


def main():
    app = MakcuDirectGui()
    app.mainloop()


if __name__ == "__main__":
    main()
