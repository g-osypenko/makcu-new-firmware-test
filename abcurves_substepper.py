"""
ABCurves SubStepper — Нативный C-рантайм рендерер движений мыши.

Архитектура:
1. ABCurves Global Renderer (renderer_global_h80.bin + abcurves_renderer.dll):
   Нативная C-нейросеть GRU (34 362 обученных веса). Принимает непрерывные
   смещения (dx, dy) и формирует естественные отчёты сенсора мыши с биомеханическим
   квантованием и микродинамикой.

2. Q16 Fixed-Point Fallback:
   32-битный дельта-аккумулятор с сохранением остатков на случай резервного режима.
"""

from __future__ import annotations

import math
from pathlib import Path
import sys
from typing import Sequence

import numpy as np

# Подключение каталога ABCurves
_ABC_ROOT = Path(__file__).resolve().parent / "ABCurves"
if str(_ABC_ROOT) not in sys.path:
    sys.path.insert(0, str(_ABC_ROOT))

try:
    from abcurves.portable_renderer import (
        CONTEXT_TICKS,
        PortableRendererModel,
        PreparedRendererContext,
    )
    _ABC_AVAILABLE = True
except Exception:
    _ABC_AVAILABLE = False


class Q16Accumulator:
    """
    Прецизионный дельта-аккумулятор с точностью Q16.
    Накапливает непрерывные дробные смещения и квантует их в целочисленные
    HID-отчёты без потери малейших долей сенсора.
    """

    __slots__ = ("acc_x", "acc_y")

    def __init__(self) -> None:
        self.acc_x: float = 0.0
        self.acc_y: float = 0.0

    def reset(self) -> None:
        self.acc_x = 0.0
        self.acc_y = 0.0

    def step(self, dx_mickeys: float, dy_mickeys: float) -> tuple[int, int]:
        self.acc_x += dx_mickeys
        self.acc_y += dy_mickeys

        out_x = round(self.acc_x)
        out_y = round(self.acc_y)

        self.acc_x -= out_x
        self.acc_y -= out_y

        return int(out_x), int(out_y)


class ABCurvesSubStepper:
    """
    Адаптер субстеппера для MAKCU UDP контроллера.
    """

    DEFAULT_MODEL_PATH = _ABC_ROOT / "models" / "renderer_global_h80.bin"
    DEFAULT_DLL_PATH = _ABC_ROOT / "abcurves" / "_native" / "abcurves_renderer.dll"
    DEFAULT_PIXEL_SCALE = 2.17  # Калибровка для 4K Ultra HD (3840x2160, mouse_spread=0)

    def __init__(
        self,
        model_path: str | Path | None = None,
        dll_path: str | Path | None = None,
        pixel_scale: float = DEFAULT_PIXEL_SCALE,
        default_seed: int = 42,
    ) -> None:
        self.pixel_scale = float(pixel_scale)
        self.default_seed = int(default_seed)
        self.accumulator = Q16Accumulator()

        self.neural_available = False
        self.model = None
        self.prepared_context = None
        self.stream = None

        if _ABC_AVAILABLE:
            m_path = Path(model_path or self.DEFAULT_MODEL_PATH).resolve()
            d_path = Path(dll_path or self.DEFAULT_DLL_PATH).resolve()
            if m_path.is_file() and d_path.is_file():
                try:
                    self.model = PortableRendererModel(m_path, library=d_path)
                    neutral_reports = np.zeros((CONTEXT_TICKS, 2), dtype=np.int16)
                    self.prepared_context = self.model.prepare_context(neutral_reports)
                    self.stream = self.prepared_context.begin_stream(event_seed=self.default_seed)
                    self.neural_available = True
                except Exception as e:
                    print(f"[!] Предупреждение: ошибка инициализации Global Renderer ABCurves: {e}")

    def reset(self) -> None:
        """Сбрасывает поток Global Renderer к начальному состоянию."""
        if self.prepared_context is not None:
            try:
                self.stream = self.prepared_context.begin_stream(event_seed=self.default_seed)
            except Exception:
                pass
        self.accumulator.reset()

    def reset_stream(self, seed: int | None = None) -> None:
        """Сбрасывает поток Global Renderer с новым seed."""
        if self.prepared_context is not None:
            s_val = self.default_seed if seed is None else int(seed)
            try:
                self.stream = self.prepared_context.begin_stream(event_seed=s_val)
            except Exception:
                pass
        self.accumulator.reset()

    def px_to_mickeys(self, px: float) -> float:
        """Переводит экранные пиксели в физические отсчёты сенсора (mickeys)."""
        return px * self.pixel_scale

    def mickeys_to_px(self, mickeys: float) -> float:
        """Переводит физические отсчёты сенсора в экранные пиксели."""
        return mickeys / self.pixel_scale if self.pixel_scale != 0 else 0.0

    def step(self, smooth_dx_mickeys: float, smooth_dy_mickeys: float) -> tuple[int, int]:
        """
        Рендерит непрерывное смещение через C-рантайм ABCurves Global Renderer (renderer_global_h80.bin).
        """
        if self.stream is not None:
            try:
                report = self.stream.step([float(smooth_dx_mickeys), float(smooth_dy_mickeys)])
                return int(report[0]), int(report[1])
            except Exception:
                pass
        return self.accumulator.step(smooth_dx_mickeys, smooth_dy_mickeys)

    def generate_steps(
        self,
        dx_px: float,
        dy_px: float,
        duration_ms: float = 200.0,
        hz: float = 1000.0,
        **_kwargs,
    ) -> list[tuple[int, int]]:
        """
        Генерирует последовательность шагов для перемещения A->B через ABCurves Global Renderer.
        """
        ticks_per_ms = float(hz) / 1000.0
        duration = max(5, int(round(duration_ms * ticks_per_ms)))

        target_dx_mickeys = self.px_to_mickeys(dx_px)
        target_dy_mickeys = self.px_to_mickeys(dy_px)

        if duration == 0 or (abs(target_dx_mickeys) < 1e-5 and abs(target_dy_mickeys) < 1e-5):
            return [(0, 0)] * duration

        step_x = target_dx_mickeys / duration
        step_y = target_dy_mickeys / duration

        result = []
        for _ in range(duration):
            rx, ry = self.step(step_x, step_y)
            result.append((rx, ry))

        return result

    def generate_circle_steps(
        self,
        radius_px: float,
        duration_s: float = 3.0,
        period_s: float = 2.0,
        hz: float = 500.0,
        **_kwargs,
    ) -> list[tuple[int, int]]:
        """
        Генерирует чистые шаги для круговой траектории через ABCurves Global Renderer.
        Траектория центрирована относительно точки старта: X: [-R, +R], Y: [-R, +R].
        """
        total_ticks = max(10, int(round(duration_s * hz)))
        radius_mickeys = self.px_to_mickeys(radius_px)
        dt = 1.0 / hz

        steps = []
        last_x = radius_mickeys
        last_y = 0.0

        for tick in range(1, total_ticks + 1):
            t = tick * dt
            theta = (2.0 * math.pi) * (t / period_s)
            curr_x = radius_mickeys * math.cos(theta)
            curr_y = radius_mickeys * math.sin(theta)

            dx = curr_x - last_x
            dy = curr_y - last_y
            last_x = curr_x
            last_y = curr_y

            rx, ry = self.step(dx, dy)
            steps.append((rx, ry))

        return steps

    def generate_sine_steps(
        self,
        amplitude_px: float,
        duration_s: float = 3.0,
        period_s: float = 1.5,
        axis: str = "x",
        hz: float = 1000.0,
    ) -> list[tuple[int, int]]:
        """
        Генерирует идеальные шаги гармонической синусоиды без дрейфа.
        """
        total_ticks = max(10, int(round(duration_s * hz)))
        amp_mickeys = self.px_to_mickeys(amplitude_px)

        steps_per_cycle = max(10, int(round(period_s * hz)))
        omega = (2.0 * math.pi) / steps_per_cycle

        acc = Q16Accumulator()
        steps = []
        last_pos = 0.0

        for tick in range(1, total_ticks + 1):
            curr_pos = amp_mickeys * math.sin(omega * tick)
            d_pos = curr_pos - last_pos
            last_pos = curr_pos

            if axis.lower() == "x":
                rx, ry = acc.step(d_pos, 0.0)
            else:
                rx, ry = acc.step(0.0, d_pos)
            steps.append((rx, ry))

        return steps

    def generate_infinity_steps(
        self,
        scale_px: float,
        duration_s: float = 3.0,
        period_s: float = 1.8,
        hz: float = 1000.0,
    ) -> list[tuple[int, int]]:
        """
        Генерирует траекторию восьмёрки (лемниската Бернулли).
        """
        total_ticks = max(10, int(round(duration_s * hz)))
        scale_mickeys = self.px_to_mickeys(scale_px)

        steps_per_cycle = max(10, int(round(period_s * hz)))
        dt = (2.0 * math.pi) / steps_per_cycle

        acc = Q16Accumulator()
        steps = []
        last_x = 0.0
        last_y = 0.0

        for tick in range(1, total_ticks + 1):
            t_val = tick * dt
            denom = 1.0 + math.sin(t_val) ** 2
            curr_x = scale_mickeys * math.cos(t_val) / denom
            curr_y = scale_mickeys * math.sin(t_val) * math.cos(t_val) / denom

            dx = curr_x - last_x
            dy = curr_y - last_y
            last_x = curr_x
            last_y = curr_y

            rx, ry = acc.step(dx, dy)
            steps.append((rx, ry))

        return steps
