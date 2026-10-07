"""
MAKCU Mouse Trajectory Visualizer (Отказоустойчивая версия 60 FPS)
Отображает в реальном времени координаты и траекторию мыши,
которые пришли в Windows и были фактически исполнены операционной системой.

Ключевые оптимизации:
1. Защита от утечек памяти: Путь рисуется единым полилайном (без накопления тысяч элементов в Canvas).
2. Полная защита от C-крашей: Отключен рекурсивный сплайн smooth=True, вызывавший переполнение стека Tcl.
3. Безопасное закрытие окна (WM_DELETE_WINDOW): Корректная остановка потока 2000 Гц без висячих таймеров.
4. Поддержка русской и английской раскладок для горячих клавиш.
5. Автоматическое логирование сбоев в visualizer_crash.log.
"""

from __future__ import annotations

import collections
import ctypes
import math
import os
from pathlib import Path
import sys
import threading
import time
import tkinter as tk
import traceback

# ---------------------------------------------------------------------------
# Логирование критических ошибок
# ---------------------------------------------------------------------------
def _global_exception_handler(exc_type, exc_value, exc_tb):
    try:
        log_path = Path(__file__).parent / "visualizer_crash.log"
        with open(log_path, "w", encoding="utf-8") as f:
            f.write(f"CRASH AT {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            traceback.print_exception(exc_type, exc_value, exc_tb, file=f)
    except Exception:
        pass
    sys.__excepthook__(exc_type, exc_value, exc_tb)

sys.excepthook = _global_exception_handler


# ---------------------------------------------------------------------------
# Аппаратная оптимизация таймеров и High-DPI Windows
# ---------------------------------------------------------------------------
_winmm = None
if sys.platform == "win32":
    try:
        ctypes.windll.user32.SetProcessDpiAwarenessContext(-4)  # Per-Monitor V2
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass

    try:
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00000080)  # HIGH_PRIORITY_CLASS
        _winmm = ctypes.WinDLL("winmm")
        _winmm.timeBeginPeriod(1)
    except Exception:
        pass


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def get_real_screen_size() -> tuple[int, int]:
    if sys.platform == "win32":
        try:
            u32 = ctypes.windll.user32
            return int(u32.GetSystemMetrics(0)), int(u32.GetSystemMetrics(1))
        except Exception:
            pass
    return 3840, 2160


def get_current_cursor_pos() -> tuple[int, int]:
    if sys.platform == "win32":
        try:
            pt = POINT()
            ctypes.windll.user32.GetCursorPos(ctypes.byref(pt))
            return int(pt.x), int(pt.y)
        except Exception:
            pass
    return 0, 0


# ---------------------------------------------------------------------------
# Высокочастотный фоновый захват движений мыши Windows (2000 Гц)
# ---------------------------------------------------------------------------
class MouseMotionSampler(threading.Thread):
    def __init__(self, sample_rate_hz: float = 2000.0) -> None:
        super().__init__(daemon=True)
        self.sample_rate_hz = float(sample_rate_hz)
        self.interval = 1.0 / self.sample_rate_hz
        self.running = True
        self.paused = False

        self.events_queue = collections.deque(maxlen=10000)
        self._lock = threading.Lock()

        self.reports_count = 0
        self.last_hz_calc_time = time.perf_counter()
        self.last_hz_reports = 0
        self.current_win_hz = 0.0

    def run(self) -> None:
        if sys.platform == "win32":
            try:
                k32 = ctypes.WinDLL("kernel32")
                k32.SetThreadPriority(k32.GetCurrentThread(), 2)  # HIGHEST
            except Exception:
                pass

        last_x, last_y = get_current_cursor_pos()
        last_time = time.perf_counter()
        next_sample = last_time + self.interval

        pt = POINT()
        u32 = ctypes.windll.user32

        while self.running:
            now = time.perf_counter()

            if not self.paused:
                u32.GetCursorPos(ctypes.byref(pt))
                cx, cy = int(pt.x), int(pt.y)

                if cx != last_x or cy != last_y:
                    dt = now - last_time
                    last_time = now
                    dist = math.hypot(cx - last_x, cy - last_y)
                    speed = (dist / dt) if dt > 0 else 0.0

                    with self._lock:
                        self.events_queue.append((cx, cy, dt, now, speed))
                        self.reports_count += 1

                    last_x, last_y = cx, cy

                if now - self.last_hz_calc_time >= 0.25:
                    d_sec = now - self.last_hz_calc_time
                    with self._lock:
                        d_rep = self.reports_count - self.last_hz_reports
                        self.current_win_hz = d_rep / d_sec if d_sec > 0 else 0.0
                        self.last_hz_reports = self.reports_count
                    self.last_hz_calc_time = now

            next_sample += self.interval
            if now > next_sample:
                next_sample = now + self.interval

            while time.perf_counter() < next_sample:
                time.sleep(0)

    def pop_all_events(self) -> list[tuple[int, int, float, float, float]]:
        with self._lock:
            if not self.events_queue:
                return []
            events = list(self.events_queue)
            self.events_queue.clear()
            return events

    def get_hz(self) -> float:
        with self._lock:
            return self.current_win_hz

    def stop(self) -> None:
        self.running = False


# ---------------------------------------------------------------------------
# Главное графическое приложение (Стабильный Canvas + HUD)
# ---------------------------------------------------------------------------
class MouseTrajectoryVisualizer:
    COLOR_BG = "#0D1117"
    COLOR_CANVAS = "#161B22"
    COLOR_GRID = "#21262D"
    COLOR_AXIS = "#30363D"
    COLOR_TEXT = "#C9D1D9"
    COLOR_TEXT_MUTED = "#8B949E"
    COLOR_CYAN = "#58A6FF"
    COLOR_ACCENT = "#39D353"
    COLOR_MAGENTA = "#BC8CFF"
    COLOR_YELLOW = "#E3B341"
    COLOR_RED = "#F85149"
    COLOR_LINE = "#00FFFF"

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("MAKCU — Визуализатор траекторий мыши Windows (True Canvas)")
        self.screen_w, self.screen_h = get_real_screen_size()
        self.is_alive = True

        self.window_w = min(1500, int(self.screen_w * 0.75))
        self.window_h = min(960, int(self.screen_h * 0.75))
        self.root.geometry(f"{self.window_w}x{self.window_h}+80+60")
        self.root.configure(bg=self.COLOR_BG)

        # Безопасное закрытие окна
        self.root.protocol("WM_DELETE_WINDOW", self.exit_app)

        self.overlay_mode = False
        self.show_guides = True
        self.trail_mode = "full"  # "full" (до 3000 точек) или "fade" (500 точек)
        self.max_full_points = 3000
        self.max_fade_points = 500

        # Кольцевой буфер точек
        self.points_buffer = collections.deque(maxlen=self.max_full_points)
        self.origin_x: int | None = None
        self.origin_y: int | None = None
        self.min_x = float("inf")
        self.max_x = float("-inf")
        self.min_y = float("inf")
        self.max_y = float("-inf")
        self.total_recorded = 0

        # Запуск фонового потока
        self.sampler = MouseMotionSampler(sample_rate_hz=2000.0)
        self.sampler.start()

        self._build_ui()
        self._bind_keys()

        # Старт цикла перерисовки (60 FPS)
        self.root.after(16, self._on_ui_tick)

    def _build_ui(self) -> None:
        self.header_frame = tk.Frame(self.root, bg=self.COLOR_BG, padx=12, pady=6)
        self.header_frame.pack(side=tk.TOP, fill=tk.X)

        self.lbl_pos = tk.Label(self.header_frame, text="Координаты: ---", font=("Consolas", 10, "bold"), fg=self.COLOR_CYAN, bg=self.COLOR_BG)
        self.lbl_pos.pack(side=tk.LEFT, padx=10)

        self.lbl_delta = tk.Label(self.header_frame, text="ΔX: 0  ΔY: 0", font=("Consolas", 10), fg=self.COLOR_TEXT, bg=self.COLOR_BG)
        self.lbl_delta.pack(side=tk.LEFT, padx=10)

        self.lbl_radius = tk.Label(self.header_frame, text="Радиус от центра: 0.0 px", font=("Consolas", 10, "bold"), fg=self.COLOR_YELLOW, bg=self.COLOR_BG)
        self.lbl_radius.pack(side=tk.LEFT, padx=10)

        self.lbl_bbox = tk.Label(self.header_frame, text="Размах (ШxВ): 0x0 px", font=("Consolas", 10), fg=self.COLOR_MAGENTA, bg=self.COLOR_BG)
        self.lbl_bbox.pack(side=tk.LEFT, padx=10)

        self.lbl_hz = tk.Label(self.header_frame, text="Частота Win: 0.0 Гц", font=("Consolas", 10, "bold"), fg=self.COLOR_ACCENT, bg=self.COLOR_BG)
        self.lbl_hz.pack(side=tk.RIGHT, padx=10)

        self.lbl_count = tk.Label(self.header_frame, text="Отчетов: 0", font=("Consolas", 10), fg=self.COLOR_TEXT_MUTED, bg=self.COLOR_BG)
        self.lbl_count.pack(side=tk.RIGHT, padx=10)

        self.canvas = tk.Canvas(self.root, bg=self.COLOR_CANVAS, highlightthickness=0, cursor="crosshair")
        self.canvas.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

        self.footer_frame = tk.Frame(self.root, bg=self.COLOR_BG, padx=12, pady=6)
        self.footer_frame.pack(side=tk.BOTTOM, fill=tk.X)

        hints = (
            "[Пробел / C] Очистить | [O] Оверлей <-> Окно | [G] Кольца (120/250/450px) | "
            "[T] След (Full/Fade) | [P] Пауза | [Esc] Выход"
        )
        self.lbl_hints = tk.Label(self.footer_frame, text=hints, font=("Segoe UI", 9), fg=self.COLOR_TEXT_MUTED, bg=self.COLOR_BG)
        self.lbl_hints.pack(side=tk.LEFT)

        self.lbl_mode_status = tk.Label(self.footer_frame, text="[Окно: Центрировано]", font=("Segoe UI", 9, "bold"), fg=self.COLOR_CYAN, bg=self.COLOR_BG)
        self.lbl_mode_status.pack(side=tk.RIGHT)

    def _bind_keys(self) -> None:
        def on_key(event):
            char = event.char.lower() if event.char else ""
            keysym = event.keysym.lower()

            if keysym in ("space",) or char in ("c", "с"):  # Eng 'c' и Рус 'с'
                self.clear_canvas()
            elif char in ("o", "щ"):  # Eng 'o' и Рус 'щ'
                self.toggle_overlay()
            elif char in ("g", "п"):  # Eng 'g' и Рус 'п'
                self.toggle_guides()
            elif char in ("t", "е"):  # Eng 't' и Рус 'е'
                self.toggle_trail_mode()
            elif char in ("p", "з"):  # Eng 'p' и Рус 'з'
                self.toggle_pause()
            elif keysym == "escape":
                self.exit_app()

        self.root.bind("<Key>", on_key)

    def clear_canvas(self) -> None:
        self.points_buffer.clear()
        self.min_x = float("inf")
        self.max_x = float("-inf")
        self.min_y = float("inf")
        self.max_y = float("-inf")
        self.total_recorded = 0

        cx, cy = get_current_cursor_pos()
        self.origin_x = cx
        self.origin_y = cy

        self.lbl_pos.config(text=f"Координаты: ({cx}, {cy})")
        self.lbl_delta.config(text="ΔX: 0  ΔY: 0")
        self.lbl_radius.config(text="Радиус от центра: 0.0 px")
        self.lbl_bbox.config(text="Размах (ШxВ): 0x0 px")
        self.lbl_count.config(text="Отчетов: 0")

        self._redraw_canvas()

    def toggle_overlay(self) -> None:
        self.overlay_mode = not self.overlay_mode
        try:
            if self.overlay_mode:
                self.root.overrideredirect(True)
                self.root.geometry(f"{self.screen_w}x{self.screen_h}+0+0")
                self.root.attributes("-topmost", True)
                try:
                    self.root.attributes("-alpha", 0.75)
                except Exception:
                    pass
                self.lbl_mode_status.config(text="[Прозрачный Оверлей 4K]", fg=self.COLOR_YELLOW)
            else:
                self.root.attributes("-topmost", False)
                try:
                    self.root.attributes("-alpha", 1.0)
                except Exception:
                    pass
                self.root.overrideredirect(False)
                self.root.geometry(f"{self.window_w}x{self.window_h}+80+60")
                self.lbl_mode_status.config(text="[Окно: Центрировано]", fg=self.COLOR_CYAN)
        except Exception as e:
            print(f"[-] Ошибка переключения оверлея: {e}")

        self.clear_canvas()

    def toggle_guides(self) -> None:
        self.show_guides = not self.show_guides
        self._redraw_canvas()

    def toggle_trail_mode(self) -> None:
        if self.trail_mode == "full":
            self.trail_mode = "fade"
            self.points_buffer = collections.deque(list(self.points_buffer)[-self.max_fade_points:], maxlen=self.max_fade_points)
        else:
            self.trail_mode = "full"
            self.points_buffer = collections.deque(self.points_buffer, maxlen=self.max_full_points)
        self._redraw_canvas()

    def toggle_pause(self) -> None:
        self.sampler.paused = not self.sampler.paused
        if self.sampler.paused:
            self.lbl_mode_status.config(text="[ПАУЗА ЗАПИСИ]", fg=self.COLOR_RED)
        else:
            text = "[Прозрачный Оверлей 4K]" if self.overlay_mode else "[Окно: Центрировано]"
            self.lbl_mode_status.config(text=text, fg=self.COLOR_YELLOW if self.overlay_mode else self.COLOR_CYAN)

    def _screen_to_canvas(self, sx: int, sy: int, cw: int, ch: int) -> tuple[float, float]:
        if self.overlay_mode:
            return float(sx), float(sy)

        if self.origin_x is None:
            self.origin_x = sx
            self.origin_y = sy

        return (cw / 2.0) + (sx - self.origin_x), (ch / 2.0) + (sy - self.origin_y)

    def _draw_grid_and_guides(self, cw: int, ch: int) -> None:
        if self.overlay_mode:
            ox = float(self.origin_x if self.origin_x is not None else self.screen_w // 2)
            oy = float(self.origin_y if self.origin_y is not None else self.screen_h // 2)
        else:
            ox = cw / 2.0
            oy = ch / 2.0

        # Осевые линии
        self.canvas.create_line(ox, 0, ox, ch, fill=self.COLOR_AXIS, dash=(4, 4), width=1)
        self.canvas.create_line(0, oy, cw, oy, fill=self.COLOR_AXIS, dash=(4, 4), width=1)

        # Крест старта
        self.canvas.create_line(ox - 10, oy, ox + 10, oy, fill=self.COLOR_YELLOW, width=2)
        self.canvas.create_line(ox, oy - 10, ox, oy + 10, fill=self.COLOR_YELLOW, width=2)
        self.canvas.create_text(ox + 8, oy - 10, text="Старт (0,0)", fill=self.COLOR_YELLOW, anchor="sw", font=("Consolas", 8))

        if not self.show_guides:
            return

        guides = [
            (120.0, "120 px", "#00E5FF"),
            (250.0, "250 px", "#E3B341"),
            (450.0, "450 px", "#FF4081"),
        ]

        for radius, label, color in guides:
            x0 = ox - radius
            y0 = oy - radius
            x1 = ox + radius
            y1 = oy + radius

            self.canvas.create_oval(x0, y0, x1, y1, outline=color, width=1, dash=(3, 5))
            self.canvas.create_text(ox + radius + 4, oy - 4, text=f"R={radius:.0f} px ({label})", fill=color, anchor="nw", font=("Consolas", 8))

    def _redraw_canvas(self) -> None:
        """Перерисовывает полотно с постоянным числом объектов (без утечек памяти)."""
        if not self.is_alive:
            return
        cw = self.canvas.winfo_width()
        ch = self.canvas.winfo_height()
        if cw <= 10 or ch <= 10:
            return

        self.canvas.delete("all")
        self._draw_grid_and_guides(cw, ch)

        if len(self.points_buffer) >= 2:
            coords = []
            for p in self.points_buffer:
                px, py = self._screen_to_canvas(p[0], p[1], cw, ch)
                coords.extend([px, py])

            # Рисуем единый полилайн БЕЗ smooth=True (исключает краш рекурсии Tcl/Tk)
            if len(coords) >= 4:
                self.canvas.create_line(*coords, fill=self.COLOR_LINE, width=2, smooth=False, tags="path")

        # Bounding Box
        if self.min_x != float("inf") and self.max_x != float("-inf") and self.min_x < self.max_x:
            b_x0, b_y0 = self._screen_to_canvas(self.min_x, self.min_y, cw, ch)
            b_x1, b_y1 = self._screen_to_canvas(self.max_x, self.max_y, cw, ch)
            span_w = self.max_x - self.min_x
            span_h = self.max_y - self.min_y
            self.canvas.create_rectangle(b_x0, b_y0, b_x1, b_y1, outline="#8B949E", dash=(2, 4), width=1)
            self.canvas.create_text(b_x0, b_y0 - 4, text=f"{span_w}x{span_h} px (Радиус: {max(span_w, span_h)/2:.1f} px)", fill="#8B949E", anchor="sw", font=("Consolas", 8))

    def _on_ui_tick(self) -> None:
        if not self.is_alive:
            return

        try:
            new_events = self.sampler.pop_all_events()
            cw = self.canvas.winfo_width()
            ch = self.canvas.winfo_height()

            if new_events and cw > 10 and ch > 10:
                for ev in new_events:
                    sx, sy, dt, t_stamp, speed = ev
                    if self.origin_x is None:
                        self.origin_x = sx
                        self.origin_y = sy

                    self.min_x = min(self.min_x, sx)
                    self.max_x = max(self.max_x, sx)
                    self.min_y = min(self.min_y, sy)
                    self.max_y = max(self.max_y, sy)
                    self.total_recorded += 1

                    self.points_buffer.append(ev)

                # Полная перерисовка полотна в 60 FPS (быстро и стабильно)
                self._redraw_canvas()

                # Телеметрия
                last_ev = self.points_buffer[-1]
                cur_x, cur_y = last_ev[0], last_ev[1]
                dx = cur_x - (self.origin_x if self.origin_x is not None else cur_x)
                dy = cur_y - (self.origin_y if self.origin_y is not None else cur_y)
                r_dist = math.hypot(dx, dy)
                span_w = (self.max_x - self.min_x) if self.max_x >= self.min_x else 0
                span_h = (self.max_y - self.min_y) if self.max_y >= self.min_y else 0

                self.lbl_pos.config(text=f"Координаты: ({cur_x}, {cur_y})")
                self.lbl_delta.config(text=f"ΔX: {dx:+4d}  ΔY: {dy:+4d}")
                self.lbl_radius.config(text=f"Радиус: {r_dist:5.1f} px")
                self.lbl_bbox.config(text=f"Размах (ШxВ): {span_w}x{span_h} px (Радиус: {max(span_w, span_h)/2:.1f} px)")
                self.lbl_count.config(text=f"Отчетов: {self.total_recorded}")

            current_hz = self.sampler.get_hz()
            self.lbl_hz.config(text=f"Частота Win: {current_hz:5.1f} Гц")

        except Exception as e:
            print(f"[-] Ошибка в тике UI: {e}")

        if self.is_alive:
            self.root.after(16, self._on_ui_tick)

    def exit_app(self) -> None:
        self.is_alive = False
        self.sampler.stop()
        try:
            self.root.destroy()
        except Exception:
            pass


def main():
    root = tk.Tk()
    app = MouseTrajectoryVisualizer(root)
    try:
        root.mainloop()
    except Exception as e:
        _global_exception_handler(*sys.exc_info())
    finally:
        app.exit_app()
        if _winmm is not None:
            try:
                _winmm.timeEndPeriod(1)
            except Exception:
                pass


if __name__ == "__main__":
    main()
