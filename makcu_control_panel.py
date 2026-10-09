"""
MAKCU + ABCurves H80 Control Panel — Автономное настольное приложение (GUI)
Чистый Windows-интерфейс без зависимостей от VS Code.
Поддерживает 500 Гц / 1000 Гц, m.move_now, честный масштаб 0.67x / 2.17x,
плавающий центр руки (Floating Center) и мгновенный СТОП.
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
import threading
import time
import tkinter as tk
from tkinter import ttk, messagebox

# ---------------------------------------------------------------------------
# Разрешение путей ресурсов (для автономного .exe в PyInstaller)
# ---------------------------------------------------------------------------
def get_resource_path(relative: str) -> Path:
    if hasattr(sys, "_MEIPASS"):
        base = Path(sys._MEIPASS)
    else:
        base = Path(__file__).resolve().parent
    return (base / relative).resolve()

# Подключение каталога ABCurves
_abc_dir = get_resource_path("ABCurves")
if str(_abc_dir) not in sys.path:
    sys.path.insert(0, str(_abc_dir))

try:
    from abcurves.portable_renderer import PortableRendererModel, CONTEXT_TICKS
    import numpy as np
    _ABC_OK = True
except Exception as e:
    _ABC_OK = False
    _ABC_ERR = str(e)

# High-DPI и 1 мс таймеры Windows
_winmm = None
if sys.platform == "win32":
    try:
        ctypes.windll.user32.SetProcessDPIAware()
    except Exception:
        pass
    try:
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00000080)  # HIGH_PRIORITY
        _winmm = ctypes.WinDLL("winmm")
        _winmm.timeBeginPeriod(1)
    except Exception:
        pass


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def get_cursor_pos() -> tuple[int, int]:
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
            return int(u32.GetSystemMetrics(0)), int(u32.GetSystemMetrics(1))
        except Exception:
            pass
    return 3840, 2160


# ---------------------------------------------------------------------------
# Ядро контроллера MAKCU UDP RAW
# ---------------------------------------------------------------------------
class MakcuEngine:
    def __init__(self, ip: str = "192.168.50.175", port: int = 8080):
        self.target = (ip, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.sock.ioctl(0x9800000C, False)
        except Exception:
            pass
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)

        self.stream = None
        self.storage_ptr = 0
        if _ABC_OK:
            m_path = get_resource_path("ABCurves/models/renderer_global_h80.bin")
            d_path = get_resource_path("ABCurves/abcurves/_native/abcurves_renderer.dll")
            if m_path.is_file() and d_path.is_file():
                try:
                    self.model = PortableRendererModel(m_path, library=d_path)
                    neutral = np.zeros((CONTEXT_TICKS, 2), dtype=np.int16)
                    self.ctx = self.model.prepare_context(neutral)
                    self.stream = self.ctx.begin_stream(event_seed=42)
                    self.storage_ptr = ctypes.cast(self.stream._storage, ctypes.c_void_p).value
                except Exception as ex:
                    print(f"[!] Ошибка загрузки рантайма ABCurves: {ex}")

    def reset_stream(self, seed: int = 42):
        if self.stream is not None:
            try:
                self.stream = self.ctx.begin_stream(event_seed=seed)
                self.storage_ptr = ctypes.cast(self.stream._storage, ctypes.c_void_p).value
            except Exception:
                pass

    def ping(self, timeout: float = 0.3) -> bool:
        nonce = secrets.token_bytes(8)
        cmd = b"m.device()\r\n"
        frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"k" + cmd
        packet = b"\x55" + nonce + frame
        self.sock.settimeout(timeout)
        try:
            self.sock.sendto(packet, self.target)
            resp, _ = self.sock.recvfrom(512)
            return len(resp) >= 12 and b"\xDE\xAD" in resp
        except Exception:
            return False
        finally:
            self.sock.settimeout(None)

    def get_device_info(self) -> str:
        nonce = secrets.token_bytes(8)
        cmd = b"m.device()\r\n"
        frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"k" + cmd
        packet = b"\x55" + nonce + frame
        self.sock.settimeout(0.3)
        try:
            self.sock.sendto(packet, self.target)
            resp, _ = self.sock.recvfrom(512)
            if b"\xDE\xAD" in resp:
                idx = resp.find(b"\xDE\xAD")
                text = resp[idx+4:].decode(errors="ignore").strip().replace(">>>", "")
                return text
        except Exception:
            pass
        finally:
            self.sock.settimeout(None)
        return "N/A"

    def send_move(self, dx: int, dy: int, use_move_now: bool = True):
        if dx == 0 and dy == 0:
            return
        nonce = secrets.token_bytes(8)
        if use_move_now:
            cmd = f"m.move_now({int(dx)},{int(dy)})\r\n".encode("ascii")
            frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"\x6B" + cmd
        else:
            payload = struct.pack("<hh", int(dx), int(dy))
            frame = b"\xDE\xAD\x04\x00\x18" + payload
        packet = b"\x55" + nonce + frame
        self.sock.sendto(packet, self.target)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Графический интерфейс Tkinter (Dark Theme)
# ---------------------------------------------------------------------------
class MakcuApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("MAKCU V4 + ABCurves H80 Control Panel")
        self.geometry("640x760")
        self.resizable(False, False)

        # Темная тема
        self.configure(bg="#1E1E24")
        self.style = ttk.Style(self)
        self.style.theme_use("clam")
        self._configure_styles()

        self.engine = MakcuEngine()
        self.stop_event = threading.Event()
        self.worker_thread = None

        self.sw, self.sh = get_screen_size()

        # Параметры по умолчанию
        self.var_hz = tk.StringVar(value="500")
        self.var_scale = tk.StringVar(value="0.67")
        self.var_cmd = tk.StringVar(value="move_now")
        self.var_status = tk.StringVar(value="Готов к работе")
        self.var_metrics = tk.StringVar(value="Частота: 0 Гц | R: 0 px | v_r: 0.0")

        self._build_ui()
        self.protocol("WM_DELETE_WINDOW", self._on_close)

        # Фоновый опрос связи при старте
        threading.Thread(target=self._check_connection, daemon=True).start()

    def _configure_styles(self):
        bg = "#1E1E24"
        card_bg = "#2A2A32"
        accent = "#00ADB5"
        text = "#EEEEEE"

        self.style.configure(".", background=bg, foreground=text, font=("Segoe UI", 10))
        self.style.configure("TLabel", background=bg, foreground=text)
        self.style.configure("Card.TFrame", background=card_bg, relief="flat")
        self.style.configure("Header.TLabel", font=("Segoe UI", 13, "bold"), foreground=accent)
        self.style.configure("SubHeader.TLabel", font=("Segoe UI", 9), foreground="#AAAAAA")
        self.style.configure("Metrics.TLabel", font=("Consolas", 10, "bold"), foreground="#00FFCC", background=card_bg)

        # Стили кнопок
        self.style.configure(
            "Action.TButton",
            font=("Segoe UI", 10, "bold"),
            background="#393E46",
            foreground="#EEEEEE",
            borderwidth=1,
            focuscolor="none"
        )
        self.style.map("Action.TButton", background=[("active", accent), ("disabled", "#222222")])

        self.style.configure(
            "Stop.TButton",
            font=("Segoe UI", 11, "bold"),
            background="#D72323",
            foreground="#FFFFFF",
            borderwidth=1,
            focuscolor="none"
        )
        self.style.map("Stop.TButton", background=[("active", "#F05454")])

    def _build_ui(self):
        main = ttk.Frame(self, padding=16)
        main.pack(fill="both", expand=True)

        # 1. Заголовок и статус платы
        header_frame = ttk.Frame(main)
        header_frame.pack(fill="x", pady=(0, 12))

        ttk.Label(header_frame, text="MAKCU V4 + ABCurves H80", style="Header.TLabel").pack(anchor="w")
        ttk.Label(
            header_frame,
            text=f"Дисплей: {self.sw}x{self.sh} | Связь: Pure UDP RAW (192.168.50.175:8080)",
            style="SubHeader.TLabel"
        ).pack(anchor="w")

        # 2. Карточка статуса соединения
        status_card = ttk.Frame(main, style="Card.TFrame", padding=10)
        status_card.pack(fill="x", pady=6)

        self.lbl_conn = ttk.Label(status_card, text="● Проверка связи с платой...", font=("Segoe UI", 10, "bold"))
        self.lbl_conn.pack(anchor="w")
        self.lbl_dev_info = ttk.Label(status_card, text="Дескриптор: ожидание...", foreground="#AAAAAA")
        self.lbl_dev_info.pack(anchor="w")

        # 3. Настройки инжекта
        cfg_card = ttk.Frame(main, style="Card.TFrame", padding=10)
        cfg_card.pack(fill="x", pady=6)

        # Частота
        row1 = ttk.Frame(cfg_card, style="Card.TFrame")
        row1.pack(fill="x", pady=3)
        ttk.Label(row1, text="Частота пакетов:", width=18).pack(side="left")
        for val, txt in [("500", "500 Гц (Sweet Spot)"), ("1000", "1000 Гц"), ("250", "250 Гц")]:
            ttk.Radiobutton(row1, text=txt, value=val, variable=self.var_hz).pack(side="left", padx=6)

        # Масштаб (Scale)
        row2 = ttk.Frame(cfg_card, style="Card.TFrame")
        row2.pack(fill="x", pady=3)
        ttk.Label(row2, text="Масштаб (mickeys/px):", width=18).pack(side="left")
        for val, txt in [("0.67", "0.67x (Честный 1:1, 4K)"), ("2.17", "2.17x (Legacy)"), ("1.00", "1.00x")]:
            ttk.Radiobutton(row2, text=txt, value=val, variable=self.var_scale).pack(side="left", padx=6)

        # Команда
        row3 = ttk.Frame(cfg_card, style="Card.TFrame")
        row3.pack(fill="x", pady=3)
        ttk.Label(row3, text="Транспорт MAKOS:", width=18).pack(side="left")
        ttk.Radiobutton(row3, text="m.move_now (обход 8 мс очереди)", value="move_now", variable=self.var_cmd).pack(side="left", padx=6)
        ttk.Radiobutton(row3, text="0x18 MOVE (бинарный 125 Гц)", value="move_0x18", variable=self.var_cmd).pack(side="left", padx=6)

        # 4. Кнопки действий (Паттерны)
        act_card = ttk.Frame(main, style="Card.TFrame", padding=12)
        act_card.pack(fill="x", pady=8)
        ttk.Label(act_card, text="ТЕСТОВЫЕ ТРАЕКТОРИИ (Свободная рука + Floating Center):", font=("Segoe UI", 10, "bold"), foreground="#00ADB5").pack(anchor="w", pady=(0, 8))

        grid = ttk.Frame(act_card, style="Card.TFrame")
        grid.pack(fill="x")

        ttk.Button(grid, text="⭕ Большой круг (250 px)", style="Action.TButton", command=lambda: self._start_pattern("circle_250")).grid(row=0, column=0, padx=4, pady=4, sticky="ew")
        ttk.Button(grid, text="⭕ Малый круг (120 px)", style="Action.TButton", command=lambda: self._start_pattern("circle_120")).grid(row=0, column=1, padx=4, pady=4, sticky="ew")

        ttk.Button(grid, text="↔ Влево <-> Вправо (250 px)", style="Action.TButton", command=lambda: self._start_pattern("line_x")).grid(row=1, column=0, padx=4, pady=4, sticky="ew")
        ttk.Button(grid, text="↕ Вверх <-> Вниз (250 px)", style="Action.TButton", command=lambda: self._start_pattern("line_y")).grid(row=1, column=1, padx=4, pady=4, sticky="ew")

        ttk.Button(grid, text="♾ Восьмёрка (Infinity 220 px)", style="Action.TButton", command=lambda: self._start_pattern("infinity")).grid(row=2, column=0, padx=4, pady=4, sticky="ew")
        ttk.Button(grid, text="🔄 Бесконечный круг (Loop)", style="Action.TButton", command=lambda: self._start_pattern("circle_loop")).grid(row=2, column=1, padx=4, pady=4, sticky="ew")

        grid.columnconfigure(0, weight=1)
        grid.columnconfigure(1, weight=1)

        # Кнопка СТОП
        ttk.Button(act_card, text="🛑 ОСТАНОВИТЬ ДВИЖЕНИЕ (СТОП)", style="Stop.TButton", command=self._stop_pattern).pack(fill="x", pady=(10, 0))

        # 5. Метрики в реальном времени
        m_card = ttk.Frame(main, style="Card.TFrame", padding=10)
        m_card.pack(fill="x", pady=6)
        ttk.Label(m_card, textvariable=self.var_status, font=("Segoe UI", 10, "bold")).pack(anchor="w")
        ttk.Label(m_card, textvariable=self.var_metrics, style="Metrics.TLabel").pack(anchor="w", pady=(4, 0))

    def _check_connection(self):
        ok = self.engine.ping()
        info = self.engine.get_device_info() if ok else "Плата не отвечает"
        self.after(0, lambda: self._update_conn_ui(ok, info))

    def _update_conn_ui(self, ok: bool, info: str):
        if ok:
            self.lbl_conn.config(text="● MAKCU: На связи (UDP RAW активен)", foreground="#00FF66")
            self.lbl_dev_info.config(text=f"Дескриптор: {info}")
        else:
            self.lbl_conn.config(text="● MAKCU: Нет связи на 192.168.50.175:8080", foreground="#FF3333")
            self.lbl_dev_info.config(text="Проверьте Ethernet-кабель и питание платы")

    def _start_pattern(self, pattern_type: str):
        if self.worker_thread and self.worker_thread.is_alive():
            self._stop_pattern()
            time.sleep(0.05)

        self.stop_event.clear()
        hz = float(self.var_hz.get())
        scale = float(self.var_scale.get())
        use_move_now = (self.var_cmd.get() == "move_now")

        self.engine.reset_stream()

        self.worker_thread = threading.Thread(
            target=self._pattern_worker,
            args=(pattern_type, hz, scale, use_move_now),
            daemon=True
        )
        self.worker_thread.start()

    def _stop_pattern(self):
        self.stop_event.set()
        self.var_status.set("Остановлено.")
        self.var_metrics.set("Частота: 0 Гц | R: 0 px | v_r: 0.0")

    def _pattern_worker(self, ptype: str, hz: float, scale: float, use_move_now: bool):
        self.var_status.set(f"Воспроизведение: {ptype} ({hz:.0f} Гц, scale={scale:.2f}x)...")

        dt = 1.0 / hz
        interval = dt
        min_gap = interval * 0.75

        # Floating Center: центр плавает вместе со свободным движением руки
        start_cx, start_cy = get_cursor_pos()
        hand_offset_x = 0.0
        hand_offset_y = 0.0
        last_real_cursor = (start_cx, start_cy)

        # Параметры траектории
        if ptype == "circle_250":
            radius_px = 250.0
            period_s = 2.0
            duration_s = 15.0
        elif ptype == "circle_120":
            radius_px = 120.0
            period_s = 1.4
            duration_s = 15.0
        elif ptype == "circle_loop":
            radius_px = 250.0
            period_s = 2.0
            duration_s = 999999.0
        elif ptype in ("line_x", "line_y"):
            amp_px = 250.0
            period_s = 1.6
            duration_s = 15.0
        elif ptype == "infinity":
            scale_px = 220.0
            period_s = 2.2
            duration_s = 15.0
        else:
            return

        omega = (2.0 * math.pi) / period_s
        start_time = time.perf_counter()
        end_time = start_time + duration_s
        last_send = start_time
        tick = 0

        # Геометрическое состояние
        if ptype.startswith("circle"):
            radius_mickeys = radius_px * scale
            last_gx = radius_mickeys
            last_gy = 0.0
        elif ptype == "infinity":
            scale_mickeys = scale_px * scale
            last_gx = 0.0
            last_gy = 0.0
        else:
            amp_mickeys = amp_px * scale
            last_gval = 0.0

        pacer_start = time.perf_counter()

        while not self.stop_event.is_set() and time.perf_counter() < end_time:
            tick += 1
            t = tick * dt

            # Аналитический расчет дельты генератора
            if ptype.startswith("circle"):
                theta = omega * t
                curr_gx = radius_mickeys * math.cos(theta)
                curr_gy = radius_mickeys * math.sin(theta)
                dx_in = curr_gx - last_gx
                dy_in = curr_gy - last_gy
                last_gx, last_gy = curr_gx, curr_gy

            elif ptype == "infinity":
                denom = 1.0 + math.sin(omega * t) ** 2
                curr_gx = scale_mickeys * math.cos(omega * t) / denom
                curr_gy = scale_mickeys * math.sin(omega * t) * math.cos(omega * t) / denom
                dx_in = curr_gx - last_gx
                dy_in = curr_gy - last_gy
                last_gx, last_gy = curr_gx, curr_gy

            elif ptype == "line_x":
                curr_val = amp_mickeys * math.sin(omega * t)
                dx_in = curr_val - last_gval
                dy_in = 0.0
                last_gval = curr_val

            elif ptype == "line_y":
                curr_val = amp_mickeys * math.sin(omega * t)
                dx_in = 0.0
                dy_in = curr_val - last_gval
                last_gval = curr_val

            # Шаг нейрорендерера ABCurves H80
            # Если 500 Гц, делаем два 1-мс субшага для идеального биомеханического тремора
            if self.engine.stream is not None:
                if hz <= 500.0:
                    s1 = self.engine.stream.step([dx_in * 0.5, dy_in * 0.5])
                    s2 = self.engine.stream.step([dx_in * 0.5, dy_in * 0.5])
                    cmd_dx = int(s1[0] + s2[0])
                    cmd_dy = int(s1[1] + s2[1])
                else:
                    rep = self.engine.stream.step([dx_in, dy_in])
                    cmd_dx = int(rep[0])
                    cmd_dy = int(rep[1])
            else:
                cmd_dx = int(round(dx_in))
                cmd_dy = int(round(dy_in))

            # Отправка по сети
            self.engine.send_move(cmd_dx, cmd_dy, use_move_now=use_move_now)

            # Синхронизация Floating Center (учет движения руки)
            now_cx, now_cy = get_cursor_pos()
            scr_dx = now_cx - last_real_cursor[0]
            scr_dy = now_cy - last_real_cursor[1]
            last_real_cursor = (now_cx, now_cy)

            # Вычитаем из экранного смещения вклад скрипта, чтобы получить чистый дрейф руки
            expected_px_x = (cmd_dx / scale) if scale > 0 else 0
            expected_px_y = (cmd_dy / scale) if scale > 0 else 0
            hand_dx = scr_dx - expected_px_x
            hand_dy = scr_dy - expected_px_y
            hand_offset_x += hand_dx
            hand_offset_y += hand_dy

            # Тактирование RatePacer
            scheduled = pacer_start + (tick * interval)
            now = time.perf_counter()
            if now >= scheduled:
                pacer_start = now - ((tick - 1) * interval)
            else:
                rem = scheduled - now
                if rem > 0.003:
                    time.sleep(rem - 0.002)
                while time.perf_counter() < scheduled:
                    pass

            while (time.perf_counter() - last_send) < min_gap:
                pass
            last_send = time.perf_counter()

            # Обновление метрик в GUI каждые 50 тактов (100 мс)
            if tick % 50 == 0:
                cur_center_x = start_cx + hand_offset_x - (radius_px if ptype.startswith("circle") else 0)
                cur_center_y = start_cy + hand_offset_y
                dist = math.hypot(now_cx - cur_center_x, now_cy - cur_center_y)
                vr = (scr_dx * (now_cx - cur_center_x) + scr_dy * (now_cy - cur_center_y)) / dist if dist > 1e-3 else 0.0
                self.var_metrics.set(
                    f"Такт: {tick:4d} | R(орбита): {dist:5.1f} px | v_r: {vr:+4.1f} | шаг: ({cmd_dx:+2d},{cmd_dy:+2d})"
                )

        if not self.stop_event.is_set():
            self.var_status.set("Траектория завершена.")
            self.var_metrics.set("Готов к следующему запуску.")

    def _on_close(self):
        self._stop_pattern()
        self.engine.close()
        if _winmm is not None:
            try:
                _winmm.timeEndPeriod(1)
            except Exception:
                pass
        self.destroy()


def main():
    app = MakcuApp()
    app.mainloop()


if __name__ == "__main__":
    main()

