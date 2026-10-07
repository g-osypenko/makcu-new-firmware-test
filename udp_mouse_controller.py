"""
MAKCU UDP Mouse Controller (Стабильная версия 500 Гц / 1000 Гц)
(Single PC / Pure Ethernet / UdpWireMode.RAW / Без COM-портов)

Ключевые инженерные решения:
1. Защита от переполнения очередей (Anti-Burst Protection):
   Таймер жестко контролирует минимальный межимпульсный интервал, предотвращая
   отправку пачек пакетов с нулевой задержкой при лагах планировщика Windows.
2. Пропуск нулевых тиков (Zero-Suppression):
   При (dx, dy) == (0, 0) сетевой пакет не формируется, что оставляет шину USB-хоста
   ESP32 свободной для физической мыши и исключает заикания курсора.
3. Надежные частотные режимы:
   - 500 Гц (Интервал 2.0 мс) — аппаратный Sweet Spot для сетевого стека lwIP ESP32.
   - 1000 Гц (Интервал 1.0 мс) — нативный режим высокой частоты.
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
from typing import Sequence

# Принудительно включаем UTF-8 вывод в консоли Windows
if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

from abcurves_substepper import ABCurvesSubStepper

TARGET_IP = "192.168.50.175"
PORT = 8080

# ---------------------------------------------------------------------------
# Аппаратная оптимизация таймеров Windows
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
    """Возвращает реальное физическое разрешение экрана."""
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


def get_cursor_position() -> tuple[int, int]:
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


class RatePacer:
    """
    Высокоточный тактовый генератор микросекундной точности (500 Гц / 1000 Гц).
    Математически исключает микро-всплески (Anti-Burst) и дрейф часов.
    """

    def __init__(self, target_hz: float = 500.0) -> None:
        self.set_hz(target_hz)
        self.reset()

    def set_hz(self, target_hz: float) -> None:
        self.target_hz = float(target_hz)
        self.interval = 1.0 / self.target_hz

    def reset(self) -> None:
        now = time.perf_counter()
        self.start_time = now
        self.last_send_time = now
        self.tick_index = 0

    def sync(self) -> None:
        """
        Ожидает следующий такт.
        Гарантирует, что пакеты отправляются ровно по расписанию,
        а при задержке ОС расписание сдвигается вперед без лавинных всплесков.
        """
        self.tick_index += 1
        scheduled_time = self.start_time + (self.tick_index * self.interval)
        now = time.perf_counter()

        # Защита от лавинного сброса (Anti-Burst Protection):
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

        self.last_send_time = time.perf_counter()


class MakcuUdpController:
    """
    Надежный UDP-контроллер мыши MAKCU (UdpWireMode.RAW) с поддержкой
    субстеппера ABCurves и стабильным тактированием на 500 Гц и 1000 Гц.
    """

    BUTTON_OPCODES = {
        "left": 0x11,
        "right": 0x12,
        "middle": 0x13,
    }

    def __init__(
        self,
        ip: str = TARGET_IP,
        port: int = PORT,
        target_hz: float = 500.0,
        pixel_scale: float | None = None,
        auto_sync_screen: bool = True,
    ) -> None:
        self.target = (ip, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.sock.ioctl(0x9800000C, False)  # SIO_UDP_CONNRESET fix
        except Exception:
            pass
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)

        self.screen_w, self.screen_h = get_screen_resolution()
        if pixel_scale is not None:
            self.pixel_scale = float(pixel_scale)
        else:
            if self.screen_w >= 3840 or self.screen_h >= 2160:
                self.pixel_scale = 2.17  # 4K Ultra HD (mouse_spread=0)
            elif self.screen_w >= 2560 or self.screen_h >= 1440:
                self.pixel_scale = 1.45  # 2K QHD
            else:
                self.pixel_scale = 1.00  # FullHD

        self.substepper = ABCurvesSubStepper(pixel_scale=self.pixel_scale)
        self.pacer = RatePacer(target_hz)

        if auto_sync_screen:
            self.sync_screen(self.screen_w, self.screen_h)

    def __enter__(self) -> MakcuUdpController:
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def close(self) -> None:
        try:
            self.sock.close()
        except Exception:
            pass

    def _send_raw_packet(self, opcode: int, payload: bytes = b"") -> None:
        nonce = secrets.token_bytes(8)
        frame = b"\xDE\xAD" + len(payload).to_bytes(2, "little") + bytes([opcode]) + payload
        packet = b"\x55" + nonce + frame
        self.sock.sendto(packet, self.target)

    def sync_screen(self, width: int, height: int) -> None:
        """Синхронизирует экран с платой MAKCU."""
        cmd = f"m.screen({width},{height})".encode("ascii")
        self._send_raw_packet(0x6B, cmd)
        time.sleep(0.05)  # Пауза для завершения обновления в ESP32

    def ping_device(self, timeout: float = 1.0) -> bool:
        """Проверяет доступность платы (DEVICE запрос opcode 0x02)."""
        self.sock.settimeout(timeout)
        try:
            self._send_raw_packet(0x02, b"")
            resp, _ = self.sock.recvfrom(512)
            return len(resp) >= 12 and resp.startswith(b"\x55") and b"\xDE\xAD" in resp
        except Exception:
            return False
        finally:
            self.sock.settimeout(None)

    def move_instant(self, dx: int, dy: int) -> None:
        """Мгновенное одиночное смещение в отсчётах (mickeys)."""
        if dx == 0 and dy == 0:
            return
        payload = struct.pack("<hh", int(dx), int(dy))
        self._send_raw_packet(0x18, payload)

    def move_steps(self, steps: Sequence[tuple[int, int]]) -> int:
        """
        Передаёт последовательность 1-мс шагов на плату MAKCU с тактированием pacer.
        Пропускает нулевые тики для разгрузки сетевого буфера ESP32.
        """
        sent_count = 0
        self.pacer.reset()

        for dx, dy in steps:
            if dx != 0 or dy != 0:
                payload = struct.pack("<hh", int(dx), int(dy))
                self._send_raw_packet(0x18, payload)
                sent_count += 1
            self.pacer.sync()

        return sent_count

    def move_rel(
        self,
        dx_px: float,
        dy_px: float,
        duration_ms: float = 200.0,
        profile: str = "min_jerk",
    ) -> int:
        """
        Плавное перемещение курсора на (dx_px, dy_px) экранных пикселей.
        Использует человекоподобную кинематику ABCurves Minimum Jerk.
        """
        steps = self.substepper.generate_steps(
            dx_px=dx_px,
            dy_px=dy_px,
            duration_ms=duration_ms,
            profile=profile,
        )
        return self.move_steps(steps)

    def move_circle(
        self,
        radius_px: float = 150.0,
        duration_s: float = 3.0,
        period_s: float = 1.5,
    ) -> int:
        """Выполняет идеальное круговое движение заданного радиуса."""
        steps = self.substepper.generate_circle_steps(
            radius_px=radius_px,
            duration_s=duration_s,
            period_s=period_s,
            hz=self.pacer.target_hz,
        )
        return self.move_steps(steps)

    def move_sine(
        self,
        amplitude_px: float = 200.0,
        duration_s: float = 3.0,
        period_s: float = 1.5,
        axis: str = "x",
    ) -> int:
        """Выполняет гармоническое синусоидальное движение."""
        steps = self.substepper.generate_sine_steps(
            amplitude_px=amplitude_px,
            duration_s=duration_s,
            period_s=period_s,
            axis=axis,
            hz=self.pacer.target_hz,
        )
        return self.move_steps(steps)

    def move_infinity(
        self,
        scale_px: float = 200.0,
        duration_s: float = 3.0,
        period_s: float = 1.8,
    ) -> int:
        """Выполняет движение по траектории восьмёрки."""
        steps = self.substepper.generate_infinity_steps(
            scale_px=scale_px,
            duration_s=duration_s,
            period_s=period_s,
            hz=self.pacer.target_hz,
        )
        return self.move_steps(steps)

    def press(self, button: str = "left") -> None:
        opcode = self.BUTTON_OPCODES.get(button.lower(), 0x11)
        self._send_raw_packet(opcode, b"\x01")

    def release(self, button: str = "left") -> None:
        opcode = self.BUTTON_OPCODES.get(button.lower(), 0x11)
        self._send_raw_packet(opcode, b"\x00")

    def click(self, button: str = "left", hold_ms: float = 50.0) -> None:
        self.press(button)
        time.sleep(max(1.0, hold_ms) / 1000.0)
        self.release(button)

    def set_move_mask(
        self,
        left: bool = False,
        right: bool = False,
        down: bool = False,
        up: bool = False,
    ) -> None:
        """
        Аппаратная маска движения физической мыши (MAK_API Opcode 0x16).
        Блокирует перемещение от руки на Host-чипе ESP32, полностью
        исключая Burst-конфликты физического и софтверного отчетов в USB.
        """
        payload = bytes([1 if left else 0, 1 if right else 0, 1 if down else 0, 1 if up else 0])
        self._send_raw_packet(0x16, payload)


def main():
    print("=================================================================")
    print("      MAKCU UDP CONTROLLER (СТАБИЛЬНЫЙ РЕЖИМ 500 ГЦ)             ")
    print("=================================================================")

    with MakcuUdpController(target_hz=500.0) as mc:
        print(f"[*] Цель: {mc.target[0]}:{mc.target[1]} (UdpWireMode.RAW)")
        print(f"[*] Экран: {mc.screen_w}x{mc.screen_h} | Масштаб: {mc.pixel_scale:.2f}x | Частота: {mc.pacer.target_hz:.0f} Гц")

        if mc.ping_device():
            print("[+] Связь с платой MAKCU установлена!")
        else:
            print("[!] Внимание: плата не ответила на ping, продолжаю тест.")

        print("\n[*] 1. Плавный квадрат (150 px, Minimum Jerk, 250 мс на грань)...")
        for dx, dy in [(150, 0), (0, 150), (-150, 0), (0, -150)]:
            sent = mc.move_rel(dx, dy, duration_ms=250.0)
            print(f"    Грань ({dx:+4d}, {dy:+4d}) px -> отправлено {sent} пакетов")
            time.sleep(0.08)

        print("\n[*] 2. Плавный круг (Радиус 150 px, 2.5 секунды)...")
        sent = mc.move_circle(radius_px=150.0, duration_s=2.5, period_s=2.5)
        print(f"[+] Круг завершён! Отправлено {sent} пакетов.")

        print("\n[*] 3. Тест клика ЛКМ...")
        mc.click("left")
        print("[+] Клик выполнен!")

    print("\n=================================================================")
    print("             ТЕСТ ЗАВЕРШЁН БЕЗ СБОЕВ И ЗАВИСАНИЙ!                ")
    print("=================================================================")


if __name__ == "__main__":
    main()
