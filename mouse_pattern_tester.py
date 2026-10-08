"""
MAKCU UDP Mouse Pattern Tester — Профессиональный калиброванный движок
(Интеграция ABCurves Global Renderer + mak-suite UDP RAW Driver)

Особенности:
1. Защита от переполнения буферов ESP32 (Anti-Burst & Zero-Suppression).
2. Выбор рабочей частоты (500 Гц Sweet Spot / 1000 Гц нативный режим).
3. Проверка границ экрана на 4K.
4. Нативный C-рантайм ABCurves Global Renderer (renderer_global_h80.bin).
5. Аппаратный транспорт mak-suite (m.move_now + live управление mouse_spread).
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

# Принудительно включаем UTF-8 в консоли Windows
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

from abcurves_substepper import ABCurvesSubStepper

TARGET_IP = "192.168.50.175"
PORT = 8080
LOG_FILENAME = "mouse_flight_log.csv"

# ---------------------------------------------------------------------------
# Оптимизация таймеров Windows (1 мс разрешение)
# ---------------------------------------------------------------------------
_winmm = None
if sys.platform == "win32":
    try:
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00000080)  # HIGH_PRIORITY_CLASS
        _winmm = ctypes.WinDLL("winmm")
        _winmm.timeBeginPeriod(1)
    except Exception:
        pass


def get_screen_resolution() -> tuple[int, int]:
    """Определяет физическое разрешение экрана."""
    if sys.platform == "win32":
        try:
            user32 = ctypes.windll.user32
            user32.SetProcessDPIAware()
            w = user32.GetSystemMetrics(0)
            h = user32.GetSystemMetrics(1)
            if w > 0 and h > 0:
                return w, h
        except Exception:
            pass
    return 3840, 2160


def get_cursor_pos() -> tuple[int, int]:
    """Считывает экранные координаты курсора через Win32 API."""
    if sys.platform == "win32":
        try:
            class POINT(ctypes.Structure):
                _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]
            pt = POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
            return int(pt.x), int(pt.y)
        except Exception:
            pass
    return 0, 0


def calculate_pixel_scale(screen_w: int, screen_h: int) -> float:
    """Масштаб пересчёта экранных пикселей в отсчёты мыши (mickeys/pixel)."""
    if screen_w >= 3840 or screen_h >= 2160:
        return 2.17  # 4K Ultra HD (3840x2160, mouse_spread=0)
    elif screen_w >= 2560 or screen_h >= 1440:
        return 1.45  # 2K QHD (2560x1440)
    else:
        return 1.00  # FullHD (1920x1080)


def check_screen_margin(margin_px: float = 300.0) -> bool:
    """Проверяет, не находится ли курсор слишком близко к краям экрана."""
    sw, sh = get_screen_resolution()
    cx, cy = get_cursor_pos()
    # Если запущен в фоновой консоли без активного GUI, cx/cy могут быть 0
    if cx == 0 and cy == 0:
        return True
    if cx < margin_px or cx > (sw - margin_px) or cy < margin_px or cy > (sh - margin_px):
        print(f"\n[!] ВНИМАНИЕ: Курсор находится близко к краю экрана: ({cx}, {cy})!")
        print(f"    Разрешение: {sw}x{sh}. Рекомендуется переместить курсор ближе к центру экрана,")
        print("    иначе при движении курсор упрётся в стенку монитора и траектория обрежется!\n")
        return False
    return True


# ---------------------------------------------------------------------------
# Черный ящик (Flight Recorder)
# ---------------------------------------------------------------------------
class FlightRecorder:
    def __init__(self, filename: str = LOG_FILENAME):
        self.filename = filename
        self.records = []
        self.start_time = time.perf_counter()
        self.last_tick_time = self.start_time
        self.last_cursor = get_cursor_pos()

    def record(self, cmd_dx: int, cmd_dy: int):
        now = time.perf_counter()
        t_ms = (now - self.start_time) * 1000.0
        dt_ms = (now - self.last_tick_time) * 1000.0
        self.last_tick_time = now

        cx, cy = get_cursor_pos()
        scr_dx = cx - self.last_cursor[0]
        scr_dy = cy - self.last_cursor[1]
        self.last_cursor = (cx, cy)

        self.records.append((
            len(self.records),
            round(t_ms, 2),
            round(dt_ms, 3),
            int(cmd_dx),
            int(cmd_dy),
            cx,
            cy,
            scr_dx,
            scr_dy
        ))

    def save_and_analyze(self, expected_px: float):
        if not self.records:
            return

        try:
            with open(self.filename, "w", encoding="utf-8") as f:
                f.write("tick,t_ms,dt_ms,cmd_dx,cmd_dy,cursor_x,cursor_y,screen_dx,screen_dy\n")
                for r in self.records:
                    f.write(f"{r[0]},{r[1]},{r[2]},{r[3]},{r[4]},{r[5]},{r[6]},{r[7]},{r[8]}\n")
            print(f"\n[+] Лог сохранён: {os.path.abspath(self.filename)} ({len(self.records)} тактов)")
        except Exception as e:
            print(f"[-] Ошибка записи лога: {e}")

        xs = [r[5] for r in self.records]
        ys = [r[6] for r in self.records]
        span_x = max(xs) - min(xs)
        span_y = max(ys) - min(ys)
        amp_x = span_x / 2.0
        amp_y = span_y / 2.0

        print("\n===================================================")
        print("          ИТОГИ ИЗМЕРЕНИЯ ЭКРАННОГО РАЗМАХА        ")
        print("===================================================")
        print(f"Целевой размер:       {expected_px:.0f} px")
        print(f"Фактический размах X: {span_x} px (Амплитуда/Радиус: {amp_x:.1f} px)")
        print(f"Фактический размах Y: {span_y} px (Амплитуда/Радиус: {amp_y:.1f} px)")
        if expected_px > 0:
            accuracy = (max(amp_x, amp_y) / expected_px * 100.0)
            print(f"Точность попадания:   {accuracy:.1f}%")
        print("===================================================\n")


# ---------------------------------------------------------------------------
# Высокоточный тактовый генератор с защитой от переполнения очередей (Anti-Burst)
# ---------------------------------------------------------------------------
class RatePacer:
    """
    Высокоточный тактовый генератор микросекундной точности (500 Гц / 1000 Гц).
    Математически исключает микро-всплески (Anti-Burst) и дрейф часов.
    """

    def __init__(self, target_hz: float = 500.0):
        self.set_hz(target_hz)
        self.tick_count = 0
        self.last_stat_time = time.perf_counter()
        self.last_stat_ticks = 0
        self.current_hz = target_hz

    def set_hz(self, target_hz: float):
        self.target_hz = float(target_hz)
        self.interval = 1.0 / self.target_hz

    def reset(self):
        now = time.perf_counter()
        self.start_time = now
        self.last_send_time = now
        self.tick_index = 0
        self.tick_count = 0
        self.last_stat_time = now
        self.last_stat_ticks = 0

    def sync(self) -> float:
        """
        Ожидает следующий такт без лавинных сбросов и залипаний.
        Гарантирует аппаратную паузу между пакетами (защита от коллизий USB-фреймов).
        """
        self.tick_index += 1
        scheduled_time = self.start_time + (self.tick_index * self.interval)
        now = time.perf_counter()

        # Anti-Burst Protection: сдвигаем базу при лагах ОС
        if now >= scheduled_time:
            self.start_time = now - ((self.tick_index - 1) * self.interval)
        else:
            remaining = scheduled_time - now
            if remaining > 0.003:
                time.sleep(remaining - 0.002)
            while time.perf_counter() < scheduled_time:
                pass

        # Аппаратный защитный зазор (Hardware Anti-Collision):
        # Между пакетами всегда должно быть >= 75% интервала (0.75 мс при 1000 Гц),
        # чтобы они никогда не слипались в один 1-мс USB-фрейм на плате ESP32.
        min_gap = self.interval * 0.75
        while (time.perf_counter() - self.last_send_time) < min_gap:
            pass

        now = time.perf_counter()
        self.last_send_time = now

        self.tick_count += 1
        dt = now - self.last_stat_time
        if dt >= 0.5:
            self.current_hz = (self.tick_count - self.last_stat_ticks) / dt
            self.last_stat_time = now
            self.last_stat_ticks = self.tick_count

        return self.current_hz


# ---------------------------------------------------------------------------
# Клиенты подключения
# ---------------------------------------------------------------------------
class MakcuRawClient:
    backend_name = "RAW UDP (чистые сокеты + Q16)"

    def __init__(self, ip: str = TARGET_IP, port: int = PORT, pixel_scale: float = 4.35, use_move_now: bool = True):
        self.target = (ip, port)
        self.pixel_scale = pixel_scale
        self.use_move_now = use_move_now
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.sock.ioctl(0x9800000C, False)
        except Exception:
            pass
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
        from abcurves_substepper import Q16Accumulator
        self.acc = Q16Accumulator()

    def sync_screen(self, width: int, height: int):
        cmd = f"m.screen({width},{height})".encode("ascii")
        frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"k" + cmd
        packet = b"\x55" + secrets.token_bytes(8) + frame
        self.sock.sendto(packet, self.target)
        time.sleep(0.05)

    def _send_move(self, dx: int, dy: int):
        if dx == 0 and dy == 0:
            return
        if self.use_move_now:
            cmd = f"m.move_now({int(dx)},{int(dy)})\r\n".encode("ascii")
            frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"\x6B" + cmd
        else:
            payload = struct.pack("<hh", int(dx), int(dy))
            frame = b"\xDE\xAD\x04\x00\x18" + payload
        packet = b"\x55" + secrets.token_bytes(8) + frame
        self.sock.sendto(packet, self.target)

    def step_smooth(self, smooth_dx_mickeys: float, smooth_dy_mickeys: float) -> tuple[int, int]:
        dx, dy = self.acc.step(smooth_dx_mickeys, smooth_dy_mickeys)
        self._send_move(dx, dy)
        return dx, dy

    def move(self, dx: int, dy: int):
        self._send_move(dx, dy)

    def set_move_mask(self, left: bool = False, right: bool = False, down: bool = False, up: bool = False):
        payload = bytes([1 if left else 0, 1 if right else 0, 1 if down else 0, 1 if up else 0])
        frame = b"\xDE\xAD\x04\x00\x16" + payload
        packet = b"\x55" + secrets.token_bytes(8) + frame
        self.sock.sendto(packet, self.target)

    def close(self):
        self.sock.close()


class MakcuAbcurvesClient:
    backend_name = "ABCurves Global Renderer + mak-suite (UDP RAW)"

    def __init__(self, ip: str = TARGET_IP, port: int = PORT, pixel_scale: float = 4.35, use_move_now: bool = True):
        self.target = (ip, port)
        self.pixel_scale = pixel_scale
        self.use_move_now = use_move_now
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.sock.ioctl(0x9800000C, False)
        except Exception:
            pass
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
        self.substepper = ABCurvesSubStepper(pixel_scale=pixel_scale)

    def sync_screen(self, width: int, height: int):
        cmd = f"m.screen({width},{height})".encode("ascii")
        frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"k" + cmd
        packet = b"\x55" + secrets.token_bytes(8) + frame
        self.sock.sendto(packet, self.target)
        time.sleep(0.05)

    def _send_move(self, dx: int, dy: int):
        if dx == 0 and dy == 0:
            return
        if self.use_move_now:
            cmd = f"m.move_now({int(dx)},{int(dy)})\r\n".encode("ascii")
            frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"\x6B" + cmd
        else:
            payload = struct.pack("<hh", int(dx), int(dy))
            frame = b"\xDE\xAD\x04\x00\x18" + payload
        packet = b"\x55" + secrets.token_bytes(8) + frame
        self.sock.sendto(packet, self.target)

    def step_smooth(self, smooth_dx_mickeys: float, smooth_dy_mickeys: float) -> tuple[int, int]:
        dx, dy = self.substepper.step(smooth_dx_mickeys, smooth_dy_mickeys)
        self._send_move(dx, dy)
        return dx, dy

    def move(self, dx: int, dy: int):
        self._send_move(dx, dy)

    def set_move_mask(self, left: bool = False, right: bool = False, down: bool = False, up: bool = False):
        payload = bytes([1 if left else 0, 1 if right else 0, 1 if down else 0, 1 if up else 0])
        frame = b"\xDE\xAD\x04\x00\x16" + payload
        packet = b"\x55" + secrets.token_bytes(8) + frame
        self.sock.sendto(packet, self.target)

    def close(self):
        self.sock.close()


# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Выполнение паттернов
# ---------------------------------------------------------------------------

def run_pattern_line(
    client,
    pacer: RatePacer,
    duration: float = 12.0,
    axis: str = "x",
    amplitude_px: float = 250.0,
    period_sec: float = 1.6,
    pixel_scale: float = 2.17,
):
    check_screen_margin(amplitude_px + 50.0)
    recorder = FlightRecorder(LOG_FILENAME)
    counts_amplitude = amplitude_px * pixel_scale

    title = "ВЛЕВО <-> ВПРАВО" if axis == "x" else "ВВЕРХ <-> ВНИЗ"
    print(f"\n[>] Запущен режим: {title} (Амплитуда: ~{amplitude_px:.0f} px, Размах: ~{amplitude_px*2:.0f} px, Частота: {pacer.target_hz:.0f} Гц)")
    print("   [REC] Идёт запись в лог. Нажмите Ctrl+C для завершения.\n")

    hz = pacer.target_hz
    dt = 1.0 / hz
    omega = (2.0 * math.pi) / period_sec

    last_val = 0.0
    start_time = time.perf_counter()
    end_time = start_time + duration
    pacer.reset()

    try:
        tick = 0
        while time.perf_counter() < end_time:
            tick += 1
            t = tick * dt
            curr_val = counts_amplitude * math.sin(omega * t)

            if hasattr(client, "step_smooth"):
                smooth_delta = curr_val - last_val
                cmd_dx, cmd_dy = client.step_smooth(
                    smooth_delta if axis == "x" else 0.0,
                    smooth_delta if axis == "y" else 0.0,
                )
            else:
                delta = int(round(curr_val - last_val))
                cmd_dx = delta if axis == "x" else 0
                cmd_dy = delta if axis == "y" else 0
                if delta != 0:
                    client.move(cmd_dx, cmd_dy)

            last_val = curr_val
            recorder.record(cmd_dx, cmd_dy)
            cur_hz = pacer.sync()

            if pacer.tick_count % 150 == 0:
                elapsed = time.perf_counter() - start_time
                print(f"\r  [Монитор] {cur_hz:5.1f} Гц | Прошло: {elapsed:4.1f}с | Шаг: ({cmd_dx:+3d}, {cmd_dy:+3d})", end="", flush=True)

    except KeyboardInterrupt:
        pass
    finally:
        recorder.save_and_analyze(amplitude_px)


def run_pattern_circle(
    client,
    pacer: RatePacer,
    duration: float = 15.0,
    radius_px: float = 250.0,
    period_sec: float = 2.0,
    pixel_scale: float = 2.17,
):
    check_screen_margin(radius_px + 50.0)
    recorder = FlightRecorder(LOG_FILENAME)
    counts_radius = radius_px * pixel_scale

    print(f"\n[>] Запущен режим: КРУГ (Радиус: ~{radius_px:.0f} px, Диаметр: ~{radius_px*2:.0f} px, Частота: {pacer.target_hz:.0f} Гц)")
    print("   [REC] Идёт запись в лог. Нажмите Ctrl+C для завершения.\n")

    hz = pacer.target_hz
    dt = 1.0 / hz
    omega = (2.0 * math.pi) / period_sec

    last_x = counts_radius
    last_y = 0.0

    start_time = time.perf_counter()
    end_time = start_time + duration
    pacer.reset()

    try:
        tick = 0
        while time.perf_counter() < end_time:
            tick += 1
            t = tick * dt

            theta = omega * t
            curr_x = counts_radius * math.cos(theta)
            curr_y = counts_radius * math.sin(theta)

            if hasattr(client, "step_smooth"):
                smooth_dx = curr_x - last_x
                smooth_dy = curr_y - last_y
                cmd_dx, cmd_dy = client.step_smooth(smooth_dx, smooth_dy)
            else:
                dx = int(round(curr_x - last_x))
                dy = int(round(curr_y - last_y))
                cmd_dx, cmd_dy = dx, dy
                if dx != 0 or dy != 0:
                    client.move(dx, dy)

            last_x = curr_x
            last_y = curr_y
            recorder.record(cmd_dx, cmd_dy)
            cur_hz = pacer.sync()

            if pacer.tick_count % 150 == 0:
                elapsed = time.perf_counter() - start_time
                print(f"\r  [Монитор] {cur_hz:5.1f} Гц | Прошло: {elapsed:4.1f}с | Шаг: ({cmd_dx:+3d}, {cmd_dy:+3d})", end="", flush=True)

    except KeyboardInterrupt:
        pass
    finally:
        recorder.save_and_analyze(radius_px)


def run_pattern_infinity(
    client,
    pacer: RatePacer,
    duration: float = 15.0,
    scale_px: float = 220.0,
    period_sec: float = 2.2,
    pixel_scale: float = 2.17,
):
    check_screen_margin(scale_px + 50.0)
    recorder = FlightRecorder(LOG_FILENAME)
    counts_scale = scale_px * pixel_scale

    print(f"\n[>] Запущен режим: ВОСЬМЁРКА (Масштаб: ~{scale_px:.0f} px, Частота: {pacer.target_hz:.0f} Гц)")
    print("   [REC] Идёт запись в лог. Нажмите Ctrl+C для завершения.\n")

    hz = pacer.target_hz
    dt = 1.0 / hz
    omega = (2.0 * math.pi) / period_sec

    last_x = 0.0
    last_y = 0.0

    start_time = time.perf_counter()
    end_time = start_time + duration
    pacer.reset()

    try:
        tick = 0
        while time.perf_counter() < end_time:
            tick += 1
            t = tick * dt

            t_param = omega * t
            denom = 1.0 + math.sin(t_param) ** 2
            curr_x = counts_scale * math.cos(t_param) / denom
            curr_y = counts_scale * math.sin(t_param) * math.cos(t_param) / denom

            if hasattr(client, "step_smooth"):
                smooth_dx = curr_x - last_x
                smooth_dy = curr_y - last_y
                cmd_dx, cmd_dy = client.step_smooth(smooth_dx, smooth_dy)
            else:
                dx = int(round(curr_x - last_x))
                dy = int(round(curr_y - last_y))
                cmd_dx, cmd_dy = dx, dy
                if dx != 0 or dy != 0:
                    client.move(dx, dy)

            last_x = curr_x
            last_y = curr_y
            recorder.record(cmd_dx, cmd_dy)
            cur_hz = pacer.sync()

            if pacer.tick_count % 150 == 0:
                elapsed = time.perf_counter() - start_time
                print(f"\r  [Монитор] {cur_hz:5.1f} Гц | Прошло: {elapsed:4.1f}с | Шаг: ({cmd_dx:+3d}, {cmd_dy:+3d})", end="", flush=True)

    except KeyboardInterrupt:
        pass
    finally:
        recorder.save_and_analyze(scale_px)


def run_pattern_step(
    client,
    pacer: RatePacer,
    delta_px: float = 250.0,
    duration_ms: float = 250.0,
    axis: str = "x",
    pixel_scale: float = 2.17,
):
    check_screen_margin(abs(delta_px) + 50.0)
    recorder = FlightRecorder(LOG_FILENAME)
    title = f"ПРЯМОЙ ШАГ {delta_px:+.0f} px по {axis.upper()}"
    print(f"\n[>] Запущен режим: {title} (Длительность: {duration_ms:.0f} мс, Частота: {pacer.target_hz:.0f} Гц)")
    print("   [REC] Идёт запись в лог...\n")

    dx_px = delta_px if axis == "x" else 0.0
    dy_px = delta_px if axis == "y" else 0.0

    if hasattr(client, "substepper"):
        steps = client.substepper.generate_steps(
            dx_px,
            dy_px,
            duration_ms=duration_ms,
            hz=pacer.target_hz,
        )
    else:
        from abcurves_substepper import Q16Accumulator

        acc = Q16Accumulator()
        total_mickeys_x = dx_px * pixel_scale
        total_mickeys_y = dy_px * pixel_scale
        ticks_per_ms = pacer.target_hz / 1000.0
        n = max(5, int(round(duration_ms * ticks_per_ms)))
        steps = []
        slice_x = total_mickeys_x / n
        slice_y = total_mickeys_y / n
        for _ in range(n):
            steps.append(acc.step(slice_x, slice_y))
        rx, ry = acc.step(0.0, 0.0)
        if rx != 0 or ry != 0:
            steps.append((rx, ry))

    pacer.reset()
    for cmd_dx, cmd_dy in steps:
        if cmd_dx != 0 or cmd_dy != 0:
            client.move(cmd_dx, cmd_dy)
        recorder.record(cmd_dx, cmd_dy)
        pacer.sync()

    recorder.save_and_analyze(abs(delta_px))


def get_device_mouse_spread() -> int | None:
    try:
        sys.path.insert(0, str(Path(__file__).parent / "mak-suite" / "python"))
        from makxd import ConnectionConfig, create_controller, UdpWireMode
        cfg = ConnectionConfig.udp(host=TARGET_IP, port=PORT, mode=UdpWireMode.RAW)
        dev = create_controller(connection=cfg)
        snap = dev.settings.read()
        return snap.settings.mouse_spread_percent
    except Exception:
        return None


def set_device_mouse_spread(percent: int, save_to_nor: bool = False) -> tuple[bool, str]:
    try:
        sys.path.insert(0, str(Path(__file__).parent / "mak-suite" / "python"))
        from makxd import ConnectionConfig, create_controller, UdpWireMode, SettingsSection
        cfg = ConnectionConfig.udp(host=TARGET_IP, port=PORT, mode=UdpWireMode.RAW)
        dev = create_controller(connection=cfg)
        snap = dev.settings.read()
        target_val = max(0, min(100, int(percent)))
        snap.settings.mouse_spread_percent = target_val
        snap = dev.settings.apply(snap, SettingsSection.MOUSE)
        if save_to_nor:
            dev.settings.save(snap, SettingsSection.MOUSE)
            return True, f"Успешно применено и сохранено в NOR-flash: {snap.settings.mouse_spread_percent}%"
        return True, f"Успешно применено live: {snap.settings.mouse_spread_percent}% (без записи в flash)"
    except Exception as e:
        return False, f"Ошибка применения настроек: {e}"


def create_client(backend_choice: str, pixel_scale: float = 2.17, use_move_now: bool = True):
    if backend_choice == "2":
        return MakcuRawClient(pixel_scale=pixel_scale, use_move_now=use_move_now)
    return MakcuAbcurvesClient(pixel_scale=pixel_scale, use_move_now=use_move_now)


def main():
    screen_w, screen_h = get_screen_resolution()
    pixel_scale = calculate_pixel_scale(screen_w, screen_h)

    print("===================================================")
    print("   MAKCU UDP: КАЛИБРОВАННЫЙ ЭКРАННЫЙ ТЕСТЕР        ")
    print("===================================================")
    print(f"[*] Дисплей: {screen_w}x{screen_h} | Калибровочный масштаб: {pixel_scale:.2f}x")
    print("1. ABCurves + mak-suite (Global Renderer H80 + UDP RAW) [По умолчанию / РЕКОМЕНДУЕТСЯ]")
    print("2. Pure mak-suite (RAW UDP + Q16)")
    print("===================================================")

    init_backend = input("Выберите бэкенд (1 или 2, Enter=1): ").strip()
    client = create_client(init_backend, pixel_scale=pixel_scale, use_move_now=True)

    # Синхронизируем разрешение экрана с платой
    client.sync_screen(screen_w, screen_h)

    # Загружаем текущий spread с платы
    current_spread = get_device_mouse_spread()

    # По умолчанию 500 Гц (аппаратный Sweet Spot)
    current_hz = 500.0
    pacer = RatePacer(current_hz)

    try:
        while True:
            cmd_mode_str = "m.move_now (Обход 8мс очереди)" if getattr(client, "use_move_now", False) else "0x18 MOVE (Очередь MAKOS 8мс)"
            spread_str = f"{current_spread}%" if current_spread is not None else "N/A"

            menu = f"""
===================================================
      MAKCU UDP: ЭКРАННЫЙ ТЕСТЕР (ЧЕСТНЫЕ ПИКСЕЛИ)
  Бэкенд:     [{client.backend_name}]
  Команда:    [{cmd_mode_str}]
  Сглаживание:[Spread: {spread_str}] (Аппаратное слияние векторов)
  Частота:    [{pacer.target_hz:.0f} Гц]  (Интервал: {pacer.interval*1000:.2f} мс)
  Разрешение: [{screen_w}x{screen_h}]  (Масштаб: {pixel_scale:.2f} отсчётов/пиксель)
===================================================
1. Влево <-> Вправо (Честная амплитуда 250 px, размах 500 px)
2. Вверх <-> Вниз (Честная амплитуда 250 px, размах 500 px)
3. Малый круг (Честный радиус 120 px)
4. БОЛЬШОЙ КРУГ (Честный радиус 250 px, диаметр 500 px)
5. ОГРОМНЫЙ КРУГ (Честный радиус 450 px, диаметр 900 px)
6. Восьмёрка (Infinity)
7. БЕСКОНЕЧНЫЙ круг (для проверки движения собственной рукой)
8. ИЗМЕНИТЬ ЧАСТОТУ (500 Гц [Sweet Spot] / 1000 Гц / 250 Гц)
9. НАСТРОЙКА МАСШТАБА (Текущий: {pixel_scale:.2f}x)
10. СМЕНИТЬ БЭКЕНД (ABCurves + mak-suite <-> RAW UDP)
11. ПЕРЕКЛЮЧИТЬ КОМАНДУ (m.move_now <-> 0x18 MOVE)
12. НАСТРОЙКА MOUSE SPREAD (Текущий: {spread_str}) [Аппаратное слияние]
13. ПРЯМОЙ ШАГ A->B (250 px через Global Renderer)
14. ЗАПУСТИТЬ ВИЗУАЛИЗАТОР ПОЛОТНА (Экранный холст в реальном времени)
0. Выход
===================================================
"""
            print(menu)
            choice = input("Выберите действие (0-14): ").strip()

            if choice == "1":
                run_pattern_line(client, pacer, duration=15.0, axis="x", amplitude_px=250.0, period_sec=1.6, pixel_scale=pixel_scale)
            elif choice == "2":
                run_pattern_line(client, pacer, duration=15.0, axis="y", amplitude_px=250.0, period_sec=1.6, pixel_scale=pixel_scale)
            elif choice == "3":
                run_pattern_circle(client, pacer, duration=15.0, radius_px=120.0, period_sec=1.5, pixel_scale=pixel_scale)
            elif choice == "4":
                run_pattern_circle(client, pacer, duration=15.0, radius_px=250.0, period_sec=2.0, pixel_scale=pixel_scale)
            elif choice == "5":
                run_pattern_circle(client, pacer, duration=15.0, radius_px=450.0, period_sec=2.8, pixel_scale=pixel_scale)
            elif choice == "6":
                run_pattern_infinity(client, pacer, duration=15.0, scale_px=220.0, period_sec=2.2, pixel_scale=pixel_scale)
            elif choice == "7":
                run_pattern_circle(client, pacer, duration=999999.0, radius_px=250.0, period_sec=2.0, pixel_scale=pixel_scale)
            elif choice == "8":
                print("\nВыберите целевую частоту пакетов:")
                print("1. 500 Гц  (РЕКОМЕНДУЕТСЯ: Sweet Spot, нулевой оверфлоу lwIP, идеальная плавность)")
                print("2. 1000 Гц (Интервал 1.0 мс — нативный режим высокой частоты)")
                print("3. 250 Гц  (Интервал 4.0 мс — максимальная стабильность)")
                hz_choice = input("Выбор (1/2/3): ").strip()
                if hz_choice == "2":
                    pacer.set_hz(1000.0)
                elif hz_choice == "3":
                    pacer.set_hz(250.0)
                else:
                    pacer.set_hz(500.0)
                print(f"[+] Частота установлена на: {pacer.target_hz:.0f} Гц")
            elif choice == "9":
                print(f"\nТекущий масштаб: {pixel_scale:.2f}x")
                print("1. 2.17x (Калибровка 4K 3840x2160, mouse_spread=0) [ПО УМОЛЧАНИЮ]")
                print("2. 1.45x (Калибровка 2K 2560x1440)")
                print("3. 1.00x (Калибровка FullHD 1920x1080)")
                print("4. 4.35x (Устаревший масштаб для прошивки с 17-мс фильтром)")
                sc_choice = input("Выбор (1-4): ").strip()
                if sc_choice == "2":
                    pixel_scale = 1.45
                elif sc_choice == "3":
                    pixel_scale = 1.00
                elif sc_choice == "4":
                    pixel_scale = 4.35
                else:
                    pixel_scale = 2.17
                print(f"[+] Масштаб установлен на: {pixel_scale:.2f}x")
            elif choice == "10":
                client.close()
                new_choice = "2" if isinstance(client, MakcuAbcurvesClient) else "1"
                print(f"\n[*] Переключение бэкенда...")
                client = create_client(new_choice, pixel_scale=pixel_scale, use_move_now=client.use_move_now)
                client.sync_screen(screen_w, screen_h)
                print(f"[+] Бэкенд изменён на: {client.backend_name}")
            elif choice == "11":
                client.use_move_now = not client.use_move_now
                mode_str = "m.move_now (прямой 1-мс репорт, обход очереди)" if client.use_move_now else "0x18 MOVE (стандартный опкод, очередь 8мс)"
                print(f"\n[+] Режим команды изменён на: {mode_str}")
            elif choice == "12":
                print(f"\n--- НАСТРОЙКА АППАРАТНОГО СГЛАЖИВАНИЯ / СЛИЯНИЯ ВЕКТОРОВ (MOUSE SPREAD) ---")
                print(f"Текущее значение на плате: {spread_str}")
                print("Справка по режимам:")
                print("  0%  — сглаживание отключено (0 мс lag, но раздельные пакеты вызывают Bursts при движении рукой)")
                print("  5%  — мягкое слияние векторов (~1-2 мс окно, устраняет Bursts без блокировки мыши!) [РЕКОМЕНДУЕТСЯ]")
                print("  10% — среднее слияние (~3-4 мс окно)")
                print("  50% — старый заводской фильтр MAKCU (17 мс, устаревший)")
                val_str = input("Введите новый процент (0-100, Enter для отмены): ").strip()
                if val_str.isdigit():
                    new_val = int(val_str)
                    save_choice = input("Сохранить перманентно в NOR-flash платы? (y/n, Enter=n): ").strip().lower() == "y"
                    ok, msg = set_device_mouse_spread(new_val, save_to_nor=save_choice)
                    print(f"[{'+' if ok else '-'}] {msg}")
                    if ok:
                        current_spread = new_val
            elif choice == "13":
                run_pattern_step(client, pacer, delta_px=250.0, duration_ms=250.0, axis="x", pixel_scale=pixel_scale)
            elif choice == "14":
                import subprocess
                canvas_script = Path(__file__).parent / "mouse_trajectory_canvas.py"
                print(f"\n[*] Запуск визуализатора полотна: {canvas_script}...")
                subprocess.Popen([sys.executable, str(canvas_script)])
                print("[+] Визуализатор запущен в отдельном окне!")
            elif choice == "0":
                print("Выход.")
                break
            else:
                print("Неверный выбор, попробуйте снова.")

    finally:
        client.close()
        if _winmm is not None:
            try:
                _winmm.timeEndPeriod(1)
            except Exception:
                pass


if __name__ == "__main__":
    main()
