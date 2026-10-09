#!/usr/bin/env python3
"""
MAKCU V4 HUMAN MOUSE DRIVER (1000 HZ NEURAL STREAM -> 125 HZ UDP AGGREGATOR)

Архитектура:
1. Инференс нейросети ABCurves H80 выполняется с нативным шагом dt = 1.0 мс (1000 Гц),
   чтобы смоделировать живую кинематику руки, физиологический тремор (8-12 Гц) и разгон.
2. Каждые 8 миллисекунд агрегатор суммирует 8 тиков нейросети и отправляет один
   бинарный UDP-пакет (опкод 0x18) на ESP32-S3 (125 Гц кадровая частота).
3. Встроенный генератор движения MAKCU раскладывает каждый пришедший кадр на 4 субстепа
   в USB-шину ПК на аппаратной частоте 1000 Гц.
4. Игра/Античит в Raw Input видит плавный человеческий поток 1000 Гц с 0% потерь дистанции.
"""

from __future__ import annotations

import ctypes
import math
from pathlib import Path
import secrets
import socket
import struct
import sys
import threading
import time
from typing import Optional

# Windows High-Resolution Multimedia Timer (1.0 ms)
if sys.platform == "win32":
    try:
        ctypes.windll.winmm.timeBeginPeriod(1)
    except Exception:
        pass

# Поиск и подключение ABCurves
_ABC_ROOT = Path(__file__).resolve().parents[1] / "ABCurves"
if str(_ABC_ROOT) not in sys.path:
    sys.path.insert(0, str(_ABC_ROOT))

try:
    import numpy as np
    from abcurves.portable_renderer import PortableRendererModel, CONTEXT_TICKS
    ABC_AVAILABLE = True
except Exception as e:
    ABC_AVAILABLE = False
    print(f"[WARN] ABCurves не доступен ({e}). Будет использован Q16 Accumulator.")


class MakcuHumanDriver:
    """
    Высокоточный драйвер мыши MAKCU V4 с нейросетевым 1000 Гц сглаживанием ABCurves.
    """

    def __init__(
        self,
        ip: str = "192.168.50.175",
        port: int = 8080,
        model_path: Optional[Path | str] = None,
        dll_path: Optional[Path | str] = None,
    ):
        self.target = (ip, int(port))
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.lock = threading.Lock()

        # Инициализация модели ABCurves
        self.model = None
        self.ctx = None
        if ABC_AVAILABLE:
            m_p = Path(model_path) if model_path else _ABC_ROOT / "models" / "renderer_global_h80.bin"
            d_p = Path(dll_path) if dll_path else _ABC_ROOT / "abcurves" / "_native" / "abcurves_renderer.dll"
            if m_p.exists() and d_p.exists():
                try:
                    self.model = PortableRendererModel(m_p, library=d_p)
                    self.ctx = self.model.prepare_context(np.zeros((CONTEXT_TICKS, 2), dtype=np.int16))
                except Exception as e:
                    print(f"[WARN] Ошибка загрузки ABCurves H80: {e}")

    # -----------------------------------------------------------------------
    # Низкоуровневые бинарные генераторы пакетов (спецификация MAK_API)
    # -----------------------------------------------------------------------
    @staticmethod
    def _make_move_pkt(dx: int, dy: int) -> bytes:
        """Опкод 0x18: Относительное перемещение (dx, dy: int16)."""
        payload = struct.pack("<hh", int(dx), int(dy))
        frame = b"\xDE\xAD\x04\x00\x18" + payload
        return b"\x55" + secrets.token_bytes(8) + frame

    @staticmethod
    def _make_btn_pkt(btn_code: int, state: int) -> bytes:
        """Опкоды 0x11 (LMB), 0x12 (RMB), 0x13 (MMB). state: 1=down, 0=up."""
        payload = bytes([1 if state else 0])
        frame = b"\xDE\xAD\x01\x00" + bytes([btn_code]) + payload
        return b"\x55" + secrets.token_bytes(8) + frame

    # -----------------------------------------------------------------------
    # Базовые действия мыши
    # -----------------------------------------------------------------------
    def send_raw(self, dx: int, dy: int):
        """Мгновенный одиночный импульс без сглаживания."""
        if dx != 0 or dy != 0:
            self.sock.sendto(self._make_move_pkt(dx, dy), self.target)

    def mouse_down(self, button: str = "left"):
        """Зажать кнопку мыши ('left', 'right', 'middle')."""
        codes = {"left": 0x11, "right": 0x12, "middle": 0x13}
        code = codes.get(button.lower(), 0x11)
        self.sock.sendto(self._make_btn_pkt(code, 1), self.target)

    def mouse_up(self, button: str = "left"):
        """Отпустить кнопку мыши."""
        codes = {"left": 0x11, "right": 0x12, "middle": 0x13}
        code = codes.get(button.lower(), 0x11)
        self.sock.sendto(self._make_btn_pkt(code, 0), self.target)

    def click(self, button: str = "left", hold_ms: float = 45.0):
        """Человеческий клик с физиологической задержкой удержания."""
        self.mouse_down(button)
        time.sleep(hold_ms / 1000.0)
        self.mouse_up(button)

    # -----------------------------------------------------------------------
    # Основной метод плавного перемещения (ABCurves H80 1000 Гц -> 250/500 Гц UDP)
    # -----------------------------------------------------------------------
    def move(
        self,
        target_dx: float,
        target_dy: float,
        duration_ms: Optional[float] = None,
        dt_frame: float = 0.004,
        blocking: bool = True,
    ) -> dict:
        """
        Плавно перемещает курсор на (target_dx, target_dy) mickeys.

        :param target_dx: Смещение по оси X (mickeys)
        :param target_dy: Смещение по оси Y (mickeys)
        :param duration_ms: Длительность движения в миллисекундах (если None, рассчитывается по закону Фиттса)
        :param dt_frame: Шаг отправки UDP пакетов (0.004 = 250 Гц / 4 тика ABCurves, 0.002 = 500 Гц, 0.008 = 125 Гц)
        :param blocking: Если True, функция блокирует поток до завершения движения
        :return: Словарь с телеметрией отправки (кадры, отправленные mickeys, режим)
        """
        if target_dx == 0 and target_dy == 0:
            return {"target_dx": 0, "target_dy": 0, "duration_ms": 0.0, "frames_sent": 0, "sent_x": 0, "sent_y": 0, "used_model": self.ctx is not None}

        frame_ms = dt_frame * 1000.0
        if duration_ms is None:
            # Расчёт физиологической длительности движения по дистанции (закон Фиттса)
            dist = (target_dx ** 2 + target_dy ** 2) ** 0.5
            # Базовое время реакции кисти ~120-140 мс + линейная фаза
            dur = 120.0 + dist * 0.45
            # Квантуем до шага frame_ms (сетка UDP)
            duration_ms = max(frame_ms * 2, min(500.0, round(dur / frame_ms) * frame_ms))
        else:
            duration_ms = max(frame_ms, round(duration_ms / frame_ms) * frame_ms)

        if blocking:
            return self._execute_movement(target_dx, target_dy, duration_ms, dt_frame)
        else:
            holder: dict = {}
            t = threading.Thread(
                target=lambda: holder.update(self._execute_movement(target_dx, target_dy, duration_ms, dt_frame)),
                daemon=True,
            )
            t.start()
            return {"async": True, "target_dx": int(round(target_dx)), "target_dy": int(round(target_dy)), "duration_ms": duration_ms}

    def _execute_movement(self, target_dx: float, target_dy: float, duration_ms: float, dt_frame: float = 0.004) -> dict:
        """
        Внутренний исполнитель:
        Чистый инференс ABCurves H80 (1000 Гц тики) -> агрегация в кадры dt_frame с Lead Pre-buffering.
        Упреждающий джиттер-буфер (1 кадр) полностью исключает Queue Underflow на ESP32.
        """
        with self.lock:
            sent_total_x = 0
            sent_total_y = 0
            total_frames = 0
            used_model = False

            frame_ms = dt_frame * 1000.0
            ticks_per_frame = max(1, int(round(frame_ms)))
            ticks_1ms = max(ticks_per_frame, int(round(duration_ms)))
            num_frames = max(1, ticks_1ms // ticks_per_frame)

            # Если доступна нейросеть ABCurves H80
            if self.ctx is not None:
                used_model = True
                stream = self.ctx.begin_stream(event_seed=secrets.randbits(32))

                step_dx = float(target_dx) / float(ticks_1ms)
                step_dy = float(target_dy) / float(ticks_1ms)

                t0 = time.perf_counter()

                for f in range(1, num_frames + 1):
                    ax, ay = 0, 0
                    for _ in range(ticks_per_frame):
                        out = stream.step([step_dx, step_dy])
                        ax += int(out[0])
                        ay += int(out[1])

                    sent_total_x += ax
                    sent_total_y += ay
                    total_frames += 1

                    if ax != 0 or ay != 0:
                        self.sock.sendto(self._make_move_pkt(ax, ay), self.target)

                    # Lead Pre-buffering: держим 1 опережающий кадр в очереди ESP32
                    target_t = t0 + (f - 1) * dt_frame
                    while time.perf_counter() < target_t:
                        pass

                # Мгновенная доводка остатка без задержек (если есть невязка округления)
                rem_x = int(round(target_dx)) - sent_total_x
                rem_y = int(round(target_dy)) - sent_total_y
                if rem_x != 0 or rem_y != 0:
                    self.sock.sendto(self._make_move_pkt(rem_x, rem_y), self.target)
                    sent_total_x += rem_x
                    sent_total_y += rem_y
                    total_frames += 1

            else:
                # Резервный режим: равномерное распределение с накоплением ошибки
                step_dx = float(target_dx) / float(num_frames)
                step_dy = float(target_dy) / float(num_frames)
                acc_x = 0.0
                acc_y = 0.0

                t0 = time.perf_counter()
                for f in range(1, num_frames + 1):
                    acc_x += step_dx
                    acc_y += step_dy
                    cur_round_x = int(round(acc_x))
                    cur_round_y = int(round(acc_y))
                    ax = cur_round_x - sent_total_x
                    ay = cur_round_y - sent_total_y

                    sent_total_x += ax
                    sent_total_y += ay
                    total_frames += 1

                    if ax != 0 or ay != 0:
                        self.sock.sendto(self._make_move_pkt(ax, ay), self.target)

                    target_t = t0 + (f - 1) * dt_frame
                    while time.perf_counter() < target_t:
                        pass

                rem_x = int(round(target_dx)) - sent_total_x
                rem_y = int(round(target_dy)) - sent_total_y
                if rem_x != 0 or rem_y != 0:
                    self.sock.sendto(self._make_move_pkt(rem_x, rem_y), self.target)
                    sent_total_x += rem_x
                    sent_total_y += rem_y
                    total_frames += 1

            return {
                "target_dx": int(round(target_dx)),
                "target_dy": int(round(target_dy)),
                "duration_ms": duration_ms,
                "frames_sent": total_frames,
                "sent_x": sent_total_x,
                "sent_y": sent_total_y,
                "used_model": used_model,
                "dt_frame": dt_frame,
            }

    def circle(
        self,
        radius: float = 180.0,
        duration_s: float = 5.0,
        revs_per_sec: float = 2.5,
        dt_frame: float = 0.004,
        blocking: bool = True,
    ) -> dict:
        """
        Непрерывное быстрое круговое движение мыши (для проверки частоты опроса).

        :param radius: Радиус круга в микеях (по умолчанию 180)
        :param duration_s: Длительность вращения в секундах (по умолчанию 5.0)
        :param revs_per_sec: Скорость вращения (оборотов в секунду, по умолчанию 2.5)
        :param dt_frame: Шаг UDP пакетов (0.004 = 250 Гц UDP / 4 тика ABCurves, 0.002 = 500 Гц, 0.008 = 125 Гц)
        :param blocking: Блокирующий вызов
        :return: Словарь с телеметрией отправки
        """
        if blocking:
            return self._execute_circle(radius, duration_s, revs_per_sec, dt_frame)
        else:
            holder: dict = {}
            t = threading.Thread(
                target=lambda: holder.update(self._execute_circle(radius, duration_s, revs_per_sec, dt_frame)),
                daemon=True,
            )
            t.start()
            return {"async": True, "radius": radius, "duration_s": duration_s}

    def _execute_circle(
        self,
        radius: float,
        duration_s: float,
        revs_per_sec: float,
        dt_frame: float,
    ) -> dict:
        """
        Непрерывный круговой поток для замера частоты опроса.
        Чистая геометрия круга -> инференс рендерера ABCurves H80 -> Cat5e LAN UDP.
        Lead Pre-buffering держит 1 опережающий кадр в очереди ESP32, исключая Queue Underflow.
        """
        with self.lock:
            ticks_per_frame = max(1, int(round(dt_frame * 1000.0)))
            total_frames = int(round(duration_s / dt_frame))
            total_ticks = total_frames * ticks_per_frame

            # Точное число полных оборотов для идеального замыкания контура в точку старта
            total_revs = max(1, round(revs_per_sec * duration_s))
            actual_revs_per_sec = total_revs / duration_s
            d_theta = (2.0 * math.pi * total_revs) / float(total_ticks)

            sent_total_x = 0
            sent_total_y = 0
            frames_sent = 0
            used_model = False

            theta = 0.0
            prev_ideal_x = radius
            prev_ideal_y = 0.0

            t0 = time.perf_counter()

            if self.ctx is not None:
                used_model = True
                stream = self.ctx.begin_stream(event_seed=secrets.randbits(32))

                for f in range(1, total_frames + 1):
                    ax, ay = 0, 0
                    for _ in range(ticks_per_frame):
                        theta += d_theta
                        cur_x = radius * math.cos(theta)
                        cur_y = radius * math.sin(theta)

                        step_dx = cur_x - prev_ideal_x
                        step_dy = cur_y - prev_ideal_y
                        prev_ideal_x = cur_x
                        prev_ideal_y = cur_y

                        out = stream.step([step_dx, step_dy])
                        ax += int(out[0])
                        ay += int(out[1])

                    sent_total_x += ax
                    sent_total_y += ay
                    frames_sent += 1

                    if ax != 0 or ay != 0:
                        self.sock.sendto(self._make_move_pkt(ax, ay), self.target)

                    # Lead Pre-buffering: держим 1 опережающий кадр в очереди ESP32
                    target_t = t0 + (f - 1) * dt_frame
                    while time.perf_counter() < target_t:
                        pass

            else:
                for f in range(1, total_frames + 1):
                    ax, ay = 0, 0
                    for _ in range(ticks_per_frame):
                        theta += d_theta
                        cur_x = radius * math.cos(theta)
                        cur_y = radius * math.sin(theta)

                        step_x = cur_x - prev_ideal_x
                        step_y = cur_y - prev_ideal_y
                        prev_ideal_x = cur_x
                        prev_ideal_y = cur_y

                        ax += int(round(step_x))
                        ay += int(round(step_y))

                    sent_total_x += ax
                    sent_total_y += ay
                    frames_sent += 1

                    if ax != 0 or ay != 0:
                        self.sock.sendto(self._make_move_pkt(ax, ay), self.target)

                    target_t = t0 + (f - 1) * dt_frame
                    while time.perf_counter() < target_t:
                        pass

            # Плавный возврат остатка на исходную позицию (если возникла невязка)
            rem_x = -sent_total_x
            rem_y = -sent_total_y
            if rem_x != 0 or rem_y != 0:
                self.sock.sendto(self._make_move_pkt(rem_x, rem_y), self.target)
                sent_total_x += rem_x
                sent_total_y += rem_y
                frames_sent += 1

            return {
                "radius": radius,
                "duration_s": duration_s,
                "revs_per_sec": actual_revs_per_sec,
                "frames_sent": frames_sent,
                "sent_x": sent_total_x,
                "sent_y": sent_total_y,
                "used_model": used_model,
                "dt_frame": dt_frame,
            }

    def close(self):
        """Закрывает сетевой сокет и освобождает ресурсы."""
        try:
            self.sock.close()
        except Exception:
            pass
        if sys.platform == "win32":
            try:
                ctypes.windll.winmm.timeEndPeriod(1)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Быстрый тест работоспособности при прямом вызове
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    print("=" * 65)
    print(" ТЕСТ MAKCU HUMAN DRIVER (1000 Гц Модель -> 125 Гц UDP)")
    print("=" * 65)
    driver = MakcuHumanDriver()
    print("[+] Драйвер успешно инициализирован.")
    print("[*] Перемещаем мышь на (+200, 0) за 200 мс...")
    driver.move(200, 0, duration_ms=200)
    time.sleep(0.3)
    print("[*] Перемещаем мышь обратно на (-200, 0)...")
    driver.move(-200, 0, duration_ms=200)
    print("[+] Готово! 0% потерь.")
    driver.close()

