"""
Прецизионный телеметрический зонд MAKCU + ABCurves H80
Синхронный замер 4 уровней конвейера на каждом такте:
1. Аналитическая дельта генератора (dx_in, dy_in)
2. Выход Global Renderer (dx_out, dy_out) + скрытый Q16 долг из памяти DLL
3. Сетевой интервал между пакетами (dt_ms)
4. Экранное смещение Windows (screen_dx, screen_dy) + радиальная скорость v_r
"""

from __future__ import annotations

import ctypes
import math
import os
from pathlib import Path
import secrets
import socket
import struct
import sys
import time

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

# Высокоточные таймеры Windows
_winmm = None
if sys.platform == "win32":
    try:
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00000080)
        _winmm = ctypes.WinDLL("winmm")
        _winmm.timeBeginPeriod(1)
    except Exception:
        pass

_ABC_ROOT = Path(__file__).resolve().parent / "ABCurves"
if str(_ABC_ROOT) not in sys.path:
    sys.path.insert(0, str(_ABC_ROOT))

from abcurves.portable_renderer import PortableRendererModel, CONTEXT_TICKS
import numpy as np

TARGET_IP = "192.168.50.175"
PORT = 8080
LOG_FILE = "telemetry_flight_log.csv"


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def get_screen_cursor() -> tuple[int, int]:
    if sys.platform == "win32":
        try:
            pt = POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
            return int(pt.x), int(pt.y)
        except Exception:
            pass
    return 0, 0


def get_screen_size() -> tuple[int, int]:
    if sys.platform == "win32":
        try:
            u32 = ctypes.windll.user32
            u32.SetProcessDPIAware()
            return int(u32.GetSystemMetrics(0)), int(u32.GetSystemMetrics(1))
        except Exception:
            pass
    return 3840, 2160


class TelemetryLogger:
    def __init__(self, filename: str = LOG_FILE):
        self.filename = filename
        self.records = []
        self.start_time = time.perf_counter()
        self.last_send_time = self.start_time
        self.last_cursor = get_screen_cursor()

    def record(
        self,
        tick: int,
        dx_in: float,
        dy_in: float,
        dx_out: int,
        dy_out: int,
        debt_x_q16: float,
        debt_y_q16: float,
        center_x: float,
        center_y: float,
    ):
        now = time.perf_counter()
        t_ms = (now - self.start_time) * 1000.0
        dt_ms = (now - self.last_send_time) * 1000.0
        self.last_send_time = now

        cx, cy = get_screen_cursor()
        scr_dx = cx - self.last_cursor[0]
        scr_dy = cy - self.last_cursor[1]
        self.last_cursor = (cx, cy)

        # Радиальное смещение относительно центра окружности:
        # v_r > 0 означает раскрутку спирали (срыв радиуса)
        rel_x = cx - center_x
        rel_y = cy - center_y
        dist = math.hypot(rel_x, rel_y)
        v_radial = (rel_x * scr_dx + rel_y * scr_dy) / dist if dist > 1e-3 else 0.0

        self.records.append((
            tick,
            round(t_ms, 2),
            round(dt_ms, 3),
            round(dx_in, 3),
            round(dy_in, 3),
            int(dx_out),
            int(dy_out),
            round(debt_x_q16, 4),
            round(debt_y_q16, 4),
            cx,
            cy,
            scr_dx,
            scr_dy,
            round(dist, 1),
            round(v_radial, 2),
        ))

    def save(self):
        if not self.records:
            return
        with open(self.filename, "w", encoding="utf-8") as f:
            f.write("tick,t_ms,dt_ms,dx_in,dy_in,dx_out,dy_out,debt_x,debt_y,cur_x,cur_y,scr_dx,scr_dy,radius,v_radial\n")
            for r in self.records:
                f.write(",".join(map(str, r)) + "\n")
        print(f"\n[+] Лог телеметрии сохранен: {os.path.abspath(self.filename)} ({len(self.records)} строк)")


def run_diagnostic_circle(
    target_hz: float = 500.0,
    radius_px: float = 250.0,
    period_s: float = 2.0,
    duration_s: float = 6.0,
    pixel_scale: float = 2.17,
    use_move_now: bool = True,
):
    sw, sh = get_screen_size()
    print("=================================================================")
    print("      ДИАГНОСТИЧЕСКИЙ ЗОНД ТЕЛЕМЕТРИИ MAKCU + ABCURVES H80       ")
    print("=================================================================")
    print(f"[*] Экран: {sw}x{sh} | Масштаб: {pixel_scale:.2f} отсчётов/пиксель")
    print(f"[*] Целевая частота: {target_hz:.0f} Гц (Интервал {1000.0/target_hz:.2f} мс)")
    print(f"[*] Радиус: {radius_px:.0f} px | Период: {period_s:.1f} с | Длительность: {duration_s:.1f} с")
    print(f"[*] Протокол: {'m.move_now (обход 8мс)' if use_move_now else '0x18 MOVE (бинарный)'}")

    # Инициализация сокета
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.ioctl(0x9800000C, False)
    except Exception:
        pass
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
    target_addr = (TARGET_IP, PORT)

    # Инициализация рантайма ABCurves
    m_path = _ABC_ROOT / "models" / "renderer_global_h80.bin"
    d_path = _ABC_ROOT / "abcurves" / "_native" / "abcurves_renderer.dll"
    model = PortableRendererModel(m_path, library=d_path)
    ctx = model.prepare_context(np.zeros((CONTEXT_TICKS, 2), dtype=np.int16))
    stream = ctx.begin_stream(event_seed=42)

    # Указатель на внутреннее состояние рантайма для прямого считывания Q16 debt
    storage_ptr = ctypes.cast(stream._storage, ctypes.c_void_p).value

    # Замер начальной точки и вычисление центра
    start_cx, start_cy = get_screen_cursor()
    # Траектория начинается в (R, 0) относительно центра, поэтому центр смещен на -R по X
    center_x = start_cx - radius_px
    center_y = start_cy

    print(f"[*] Старт курсора: ({start_cx}, {start_cy}) | Расчетный центр орбиты: ({center_x:.0f}, {center_y:.0f})")

    logger = TelemetryLogger(LOG_FILE)
    dt = 1.0 / target_hz
    omega = (2.0 * math.pi) / period_s
    radius_mickeys = radius_px * pixel_scale

    last_x = radius_mickeys
    last_y = 0.0

    interval = 1.0 / target_hz
    min_gap = interval * 0.75
    last_send = time.perf_counter()
    start_time = time.perf_counter()
    end_time = start_time + duration_s
    tick = 0

    try:
        while time.perf_counter() < end_time:
            tick += 1
            t = tick * dt
            theta = omega * t

            curr_x = radius_mickeys * math.cos(theta)
            curr_y = radius_mickeys * math.sin(theta)

            dx_in = curr_x - last_x
            dy_in = curr_y - last_y
            last_x = curr_x
            last_y = curr_y

            # 1. Шаг Global Renderer H80
            rep = stream.step([dx_in, dy_in])
            dx_out, dy_out = int(rep[0]), int(rep[1])

            # 2. Прямое считывание скрытого долга Q16 из памяти DLL
            debt_raw = bytes((ctypes.c_ubyte * 190).from_address(storage_ptr))
            raw_acc_x = struct.unpack_from("<i", debt_raw, 180)[0]
            raw_acc_y = struct.unpack_from("<i", debt_raw, 184)[0]
            debt_x = raw_acc_x / 65536.0
            debt_y = raw_acc_y / 65536.0

            # 3. Отправка пакета на плату
            if dx_out != 0 or dy_out != 0:
                if use_move_now:
                    cmd = f"m.move_now({dx_out},{dy_out})\r\n".encode("ascii")
                    frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"\x6B" + cmd
                else:
                    payload = struct.pack("<hh", dx_out, dy_out)
                    frame = b"\xDE\xAD\x04\x00\x18" + payload
                packet = b"\x55" + secrets.token_bytes(8) + frame
                sock.sendto(packet, target_addr)

            # 4. Логирование такта
            logger.record(
                tick=tick,
                dx_in=dx_in,
                dy_in=dy_in,
                dx_out=dx_out,
                dy_out=dy_out,
                debt_x_q16=debt_x,
                debt_y_q16=debt_y,
                center_x=center_x,
                center_y=center_y,
            )

            # 5. Тактирование RatePacer с защитным зазором
            scheduled = start_time + (tick * interval)
            now = time.perf_counter()
            if now >= scheduled:
                start_time = now - ((tick - 1) * interval)
            else:
                rem = scheduled - now
                if rem > 0.003:
                    time.sleep(rem - 0.002)
                while time.perf_counter() < scheduled:
                    pass

            while (time.perf_counter() - last_send) < min_gap:
                pass
            last_send = time.perf_counter()

            if tick % 150 == 0:
                last_r = logger.records[-1]
                print(
                    f"\r  [Такт {tick:5d}] dt={last_r[2]:4.2f}мс | in=({dx_in:+4.1f},{dy_in:+4.1f}) | out=({dx_out:+2d},{dy_out:+2d}) | долг=({debt_x:+5.2f},{debt_y:+5.2f}) | R={last_r[13]:5.1f}px | v_r={last_r[14]:+5.1f}",
                    end="",
                    flush=True,
                )

    except KeyboardInterrupt:
        print("\n[!] Остановлено пользователем.")
    finally:
        sock.close()
        logger.save()
        if _winmm is not None:
            try:
                _winmm.timeEndPeriod(1)
            except Exception:
                pass


if __name__ == "__main__":
    run_diagnostic_circle()

