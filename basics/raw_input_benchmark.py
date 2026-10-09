#!/usr/bin/env python3
"""
ПРЕЦИЗИОННЫЙ RAW INPUT GUI БЕНЧМАРК MAKCU V4 (1000 ГЦ НАПРЯМУЮ ИЗ ОКНА).

Все тесты запускаются прямо кнопками в окне или горячими клавишами (P, 1..9, 0, S, C, Esc).
Поддерживаются глобальные горячие клавиши и фоновый захват мыши (RIDEV_INPUTSINK),
что позволяет запускать тесты клавишами, пока активно другое окно (например, Google Chrome с тестером полинга).
"""

import ctypes
from ctypes import wintypes
import math
from pathlib import Path
import secrets
import socket
import struct
import sys
import threading
import time
import traceback
from typing import Optional

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
gdi32 = ctypes.windll.gdi32
winmm = ctypes.windll.winmm
winmm.timeBeginPeriod(1)

TARGET = ("192.168.50.175", 8080)

# ---------------------------------------------------------------------------
# Пути и импорт компонентов MAKCU
# ---------------------------------------------------------------------------
_CUR_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _CUR_DIR.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))
if str(_CUR_DIR) not in sys.path:
    sys.path.insert(0, str(_CUR_DIR))
_MAK_SUITE_PATH = _REPO_ROOT / "mak-suite" / "python"
if str(_MAK_SUITE_PATH) not in sys.path:
    sys.path.insert(0, str(_MAK_SUITE_PATH))

try:
    from basics.makcu_human_driver import MakcuHumanDriver, enable_combat_mode
except ImportError:
    from makcu_human_driver import MakcuHumanDriver, enable_combat_mode

# ---------------------------------------------------------------------------
# Win32 x64 ABI Signatures
# ---------------------------------------------------------------------------
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
kernel32.GetModuleHandleW.restype = wintypes.HINSTANCE

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)

user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.DefWindowProcW.restype = wintypes.LPARAM

user32.CreateWindowExW.restype = wintypes.HWND
user32.CreateWindowExW.argtypes = [
    wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wintypes.HWND, wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID
]

user32.RegisterClassExW.argtypes = [ctypes.c_void_p]
user32.RegisterClassExW.restype = wintypes.ATOM

user32.RegisterRawInputDevices.argtypes = [ctypes.c_void_p, wintypes.UINT, wintypes.UINT]
user32.RegisterRawInputDevices.restype = wintypes.BOOL

user32.GetRawInputData.argtypes = [ctypes.c_void_p, wintypes.UINT, ctypes.c_void_p, ctypes.POINTER(wintypes.UINT), wintypes.UINT]
user32.GetRawInputData.restype = wintypes.UINT

user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = ctypes.c_longlong

user32.SetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
user32.SetWindowTextW.restype = wintypes.BOOL

user32.RegisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int, wintypes.UINT, wintypes.UINT]
user32.RegisterHotKey.restype = wintypes.BOOL

user32.UnregisterHotKey.argtypes = [wintypes.HWND, ctypes.c_int]
user32.UnregisterHotKey.restype = wintypes.BOOL

gdi32.CreateFontW.argtypes = [
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
    wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, wintypes.LPCWSTR
]
gdi32.CreateFontW.restype = wintypes.HFONT

class WNDCLASSEXW(ctypes.Structure):
    _fields_ = [
        ('cbSize', wintypes.UINT),
        ('style', wintypes.UINT),
        ('lpfnWndProc', WNDPROC),
        ('cbClsExtra', ctypes.c_int),
        ('cbWndExtra', ctypes.c_int),
        ('hInstance', wintypes.HINSTANCE),
        ('hIcon', wintypes.HICON),
        ('hCursor', wintypes.HICON),
        ('hbrBackground', wintypes.HBRUSH),
        ('lpszMenuName', wintypes.LPCWSTR),
        ('lpszClassName', wintypes.LPCWSTR),
        ('hIconSm', wintypes.HICON),
    ]

class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [
        ('dwType', wintypes.DWORD),
        ('dwSize', wintypes.DWORD),
        ('hDevice', wintypes.HANDLE),
        ('wParam', wintypes.WPARAM),
    ]

class RAWMOUSE(ctypes.Structure):
    class _DUMMYUNIONNAME(ctypes.Union):
        class _DUMMYSTRUCTNAME(ctypes.Structure):
            _fields_ = [
                ('usButtonFlags', wintypes.USHORT),
                ('usButtonData', wintypes.USHORT),
            ]
        _fields_ = [
            ('ulButtons', wintypes.ULONG),
            ('struct', _DUMMYSTRUCTNAME),
        ]
    _anonymous_ = ('union',)
    _fields_ = [
        ('usFlags', wintypes.USHORT),
        ('union', _DUMMYUNIONNAME),
        ('ulRawButtons', wintypes.ULONG),
        ('lLastX', ctypes.c_long),
        ('lLastY', ctypes.c_long),
        ('ulExtraInformation', wintypes.ULONG),
    ]

class RAWINPUT(ctypes.Structure):
    class _DUMMYUNIONNAME(ctypes.Union):
        _fields_ = [
            ('mouse', RAWMOUSE),
        ]
    _anonymous_ = ('union',)
    _fields_ = [
        ('header', RAWINPUTHEADER),
        ('union', _DUMMYUNIONNAME),
    ]

class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [
        ('usUsagePage', wintypes.USHORT),
        ('usUsage', wintypes.USHORT),
        ('dwFlags', wintypes.DWORD),
        ('hwndTarget', wintypes.HWND),
    ]


# ---------------------------------------------------------------------------
# Packet Generators
# ---------------------------------------------------------------------------
def make_opcode_18_packet(dx: int, dy: int) -> bytes:
    payload = struct.pack("<hh", int(dx), int(dy))
    frame = b"\xDE\xAD\x04\x00\x18" + payload
    return b"\x55" + secrets.token_bytes(8) + frame

def make_move_now_packet(dx: int, dy: int) -> bytes:
    cmd = f"m.move_now({int(dx)},{int(dy)})\r\n".encode("ascii")
    frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"\x6B" + cmd
    return b"\x55" + secrets.token_bytes(8) + frame


# ---------------------------------------------------------------------------
# Настройки платы MAKCU (NOR Flash / Live Tuning через makxd)
# ---------------------------------------------------------------------------
def get_device_mouse_spread(ip: str = TARGET[0], port: int = TARGET[1]) -> Optional[int]:
    """Считывает текущее значение mouse_spread_percent из NOR Flash платы MAKCU."""
    try:
        from makxd import ConnectionConfig, create_controller, UdpWireMode
        cfg = ConnectionConfig.udp(host=ip, port=port, mode=UdpWireMode.RAW)
        dev = create_controller(connection=cfg)
        try:
            snap = dev.settings.read()
            return int(snap.settings.mouse_spread_percent)
        finally:
            dev.disconnect()
    except Exception:
        return None


def set_device_mouse_spread(
    percent: int,
    ip: str = TARGET[0],
    port: int = TARGET[1],
    save_to_nor: bool = True,
) -> tuple[bool, str, int]:
    """
    Применяет mouse_spread_percent на плате (live) и сохраняет в NOR Flash.
    0% = сплайн отключен (0 мс задержка, прямая выдача в USB HID).
    """
    try:
        from makxd import ConnectionConfig, create_controller, UdpWireMode, SettingsSection
        cfg = ConnectionConfig.udp(host=ip, port=port, mode=UdpWireMode.RAW)
        dev = create_controller(connection=cfg)
        try:
            snap = dev.settings.read()
            target_val = max(0, min(100, int(percent)))
            snap.settings.mouse_spread_percent = target_val
            snap = dev.settings.apply(snap, SettingsSection.MOUSE)
            if save_to_nor:
                dev.settings.save(snap, SettingsSection.MOUSE)
                return True, f"Успешно применено и сохранено в NOR Flash: {snap.settings.mouse_spread_percent}%", snap.settings.mouse_spread_percent
            return True, f"Успешно применено live: {snap.settings.mouse_spread_percent}%", snap.settings.mouse_spread_percent
        finally:
            dev.disconnect()
    except Exception as e:
        return False, f"Ошибка записи mouse_spread: {e}", -1


# ---------------------------------------------------------------------------
# Precision Benchmark GUI Window
# ---------------------------------------------------------------------------
class BenchmarkApp:
    def __init__(self):
        self.lock = threading.Lock()
        self.reports: list[tuple[float, int, int]] = []
        self.button_events: list[tuple[float, int]] = []
        self.hwnd = None
        self.edit_hwnd = None
        self.running = False
        self.test_busy = False
        self.log_lines: list[str] = []
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Боевой драйвер мыши MAKCU V4 с нейросетью ABCurves H80
        self.driver = MakcuHumanDriver(ip=TARGET[0], port=TARGET[1])
        self.combat_status = getattr(self.driver, "combat_status", {})

        # Аппаратный фильтр mouse_spread на ESP32-S3
        self.current_spread: Optional[int] = None
        self.btn_spread_hwnd = None
        self.registered_hotkeys: list[int] = []

        # Создаем окно
        self.ready_event = threading.Event()
        self.ui_thread = threading.Thread(target=self._run_ui, daemon=True)
        self.ui_thread.start()
        self.ready_event.wait(timeout=3.0)

        # Асинхронное чтение текущего mouse_spread с платы
        threading.Thread(target=self._init_spread_state, daemon=True).start()

    def log(self, text: str = ""):
        print(text)
        with self.lock:
            self.log_lines.append(text)
            if len(self.log_lines) > 400:
                self.log_lines = self.log_lines[-350:]
            full = "\r\n".join(self.log_lines)
        if self.edit_hwnd:
            user32.SetWindowTextW(self.edit_hwnd, full)
            user32.SendMessageW(self.edit_hwnd, 0x0115, 7, 0)  # SB_BOTTOM

    def clear_log(self):
        with self.lock:
            self.log_lines.clear()
        if self.edit_hwnd:
            user32.SetWindowTextW(self.edit_hwnd, "")

    def reset_reports(self):
        with self.lock:
            self.reports.clear()
            self.button_events.clear()

    def get_captured_stats(self) -> tuple[int, int, int, list[float], list[tuple[float, int]]]:
        with self.lock:
            reps = list(self.reports)
            btns = list(self.button_events)
        sum_dx = sum(r[1] for r in reps)
        sum_dy = sum(r[2] for r in reps)
        count = len(reps)
        dts = []
        for i in range(1, len(reps)):
            dts.append((reps[i][0] - reps[i-1][0]) * 1000.0)
        return sum_dx, sum_dy, count, dts, btns

    def _wnd_proc(self, hwnd, msg, wparam, lparam):
        if msg == 0x00FF:  # WM_INPUT
            raw = RAWINPUT()
            size = wintypes.UINT(ctypes.sizeof(RAWINPUT))
            header_size = wintypes.UINT(ctypes.sizeof(RAWINPUTHEADER))
            res = user32.GetRawInputData(lparam, 0x10000003, ctypes.byref(raw), ctypes.byref(size), header_size)
            if res != 0xFFFFFFFF and raw.header.dwType == 0:
                dx = int(raw.mouse.lLastX)
                dy = int(raw.mouse.lLastY)
                btn_flags = int(raw.mouse.struct.usButtonFlags)
                now = time.perf_counter()
                with self.lock:
                    if dx != 0 or dy != 0:
                        self.reports.append((now, dx, dy))
                    if btn_flags != 0:
                        self.button_events.append((now, btn_flags))
            return 0

        elif msg == 0x0111:  # WM_COMMAND
            cmd_id = wparam & 0xFFFF
            # Игнорируем любые уведомления от Edit control (ID 200)
            if cmd_id == 200 or lparam == self.edit_hwnd:
                return 0
            notify_code = (wparam >> 16) & 0xFFFF
            # Обрабатываем только нажатия кнопок (BN_CLICKED == 0) в диапазоне 100..115
            if notify_code == 0 and (100 <= cmd_id <= 115):
                self._on_action(cmd_id)
            return 0

        elif msg == 0x0312:  # WM_HOTKEY (глобальные горячие клавиши, когда активно другое окно)
            hk_id = int(wparam)
            if hk_id in (100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 112, 113, 114):
                self._on_action(hk_id)
            elif hk_id == 115:                 # Хоткей 'K' -> режим хаоса 114
                self._on_action(114)
            elif 201 <= hk_id <= 209:
                self._on_action(hk_id - 100)  # Numpad 1..9 -> cmd 101..109
            elif hk_id == 212:
                self._on_action(112)          # Numpad 0 -> cmd 112
            elif hk_id == 214:                 # Numpad * -> режим хаоса 114
                self._on_action(114)
            elif hk_id == 110:
                self.clear_log()
            return 0

        elif msg == 0x0100:  # WM_KEYDOWN (горячие клавиши при фокусе в окне)
            vk = wparam
            if vk == ord('P'): self._on_action(100)
            elif vk == ord('1'): self._on_action(101)
            elif vk == ord('2'): self._on_action(102)
            elif vk == ord('3'): self._on_action(103)
            elif vk == ord('4'): self._on_action(104)
            elif vk == ord('5'): self._on_action(105)
            elif vk == ord('6'): self._on_action(106)
            elif vk == ord('7'): self._on_action(107)
            elif vk == ord('8'): self._on_action(108)
            elif vk == ord('9'): self._on_action(109)
            elif vk == ord('0'): self._on_action(112)
            elif vk in (ord('W'), ord('K')): self._on_action(114)
            elif vk == ord('S'): self._on_action(113)
            elif vk == ord('C'): self.clear_log()
            elif vk == 27:  # Esc
                user32.PostMessageW(hwnd, 0x0010, 0, 0)
            return 0

        elif msg == 0x0010:  # WM_CLOSE
            self.running = False
            user32.DestroyWindow(hwnd)
            return 0

        elif msg == 0x0002:  # WM_DESTROY
            self._unregister_hotkeys()
            user32.PostQuitMessage(0)
            return 0

        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def _unregister_hotkeys(self):
        if self.hwnd:
            for hk_id in self.registered_hotkeys:
                try:
                    user32.UnregisterHotKey(self.hwnd, hk_id)
                except Exception:
                    pass
        self.registered_hotkeys.clear()

    def _run_ui(self):
        self.wnd_proc_cb = WNDPROC(self._wnd_proc)
        wc = WNDCLASSEXW()
        wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
        wc.lpfnWndProc = self.wnd_proc_cb
        wc.hInstance = kernel32.GetModuleHandleW(None)
        wc.hbrBackground = 6  # COLOR_WINDOW + 1
        wc.lpszClassName = f"MakcuBenchWin_{secrets.token_hex(4)}"
        user32.RegisterClassExW(ctypes.byref(wc))

        # Главное окно (960 x 680)
        self.hwnd = user32.CreateWindowExW(
            0x00000008,  # WS_EX_TOPMOST
            wc.lpszClassName, 'MAKCU V4 - Precision Raw Input Benchmark (1000 Hz GUI)',
            0x00CF0000 | 0x10000000,  # WS_OVERLAPPEDWINDOW | WS_VISIBLE
            100, 60, 960, 680, None, None, wc.hInstance, None
        )

        # Фоновый перехват мыши (RIDEV_INPUTSINK) - собирает отчеты даже когда фокус в Chrome
        RIDEV_INPUTSINK = 0x00000100
        rid = RAWINPUTDEVICE()
        rid.usUsagePage = 0x01
        rid.usUsage = 0x02
        rid.dwFlags = RIDEV_INPUTSINK
        rid.hwndTarget = self.hwnd
        user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(RAWINPUTDEVICE))

        # Глобальные горячие клавиши (работают когда активно другое окно, например Google Chrome)
        MOD_NOREPEAT = 0x4000
        hotkey_bindings = [
            (100, ord('P')),
            (101, ord('1')), (102, ord('2')), (103, ord('3')), (104, ord('4')), (105, ord('5')),
            (106, ord('6')), (107, ord('7')), (108, ord('8')), (109, ord('9')), (112, ord('0')),
            (114, ord('W')), (115, ord('K')),
            (113, ord('S')), (110, ord('C')),
            # Numpad (удобно при тестировании в стороннем окне)
            (201, 0x61), (202, 0x62), (203, 0x63), (204, 0x64), (205, 0x65),
            (206, 0x66), (207, 0x67), (208, 0x68), (209, 0x69), (212, 0x60),
            (214, 0x6A), # Numpad * -> режим хаоса
        ]
        for hk_id, vk in hotkey_bindings:
            if user32.RegisterHotKey(self.hwnd, hk_id, MOD_NOREPEAT, vk):
                self.registered_hotkeys.append(hk_id)

        font_gui = gdi32.CreateFontW(15, 0, 0, 0, 600, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI")
        font_mono = gdi32.CreateFontW(15, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, "Consolas")

        # Создание кнопок слева
        buttons = [
            (100, "[P] Замер мыши от руки (3 сек, 1000 Гц)"),
            (101, "[1] Одиночный 300 (0x18 MOVE)"),
            (102, "[2] Серия 125 Гц (30 по 10) 0x18"),
            (103, "[3] Серия 250 Гц (30 по 10) 0x18"),
            (104, "[4] Серия 500 Гц (30 по 10) [Коллизии!]"),
            (105, "[5] Серия 1000 Гц (30 по 10) [Коллизии!]"),
            (106, "[6] Драйвер: move(+300, 0) [Авто-Фиттс]"),
            (107, "[7] Драйвер: move(+300, 0) [Фикс 200 мс]"),
            (108, "[8] Драйвер: move(+150, +75) [Диагональ]"),
            (109, "[9] Драйвер: Флик (+200, -100) + Выстрел"),
            (112, "[0] Драйвер: Быстрый круг 5 сек (125 Гц)"),
            (114, "[W] Широкий хаос 5 сек (Keymap Labs)"),
            (113, "[S] mouse_spread: ... (Чтение)"),
            (110, "[C] Очистить лог"),
            (111, "[Esc] Выход"),
        ]

        btn_y = 10
        for b_id, b_text in buttons:
            btn_hwnd = user32.CreateWindowExW(
                0, 'BUTTON', b_text,
                0x50010000,  # WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON
                15, btn_y, 320, 34,
                self.hwnd, ctypes.cast(b_id, wintypes.HMENU), wc.hInstance, None
            )
            user32.SendMessageW(btn_hwnd, 0x0030, font_gui, 1)  # WM_SETFONT
            if b_id == 113:
                self.btn_spread_hwnd = btn_hwnd
            btn_y += 39

        # Журнал результатов справа (Edit multiline readonly)
        self.edit_hwnd = user32.CreateWindowExW(
            0x00000200,  # WS_EX_CLIENTEDGE
            'EDIT', '',
            0x50200844 | 0x00800000,  # WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_BORDER
            350, 10, 575, 615,
            self.hwnd, ctypes.cast(200, wintypes.HMENU), wc.hInstance, None
        )
        user32.SendMessageW(self.edit_hwnd, 0x0030, font_mono, 1)
        user32.SendMessageW(self.edit_hwnd, 0x00C5, 2000000, 0)  # EM_SETLIMITTEXT

        user32.ShowWindow(self.hwnd, 5)
        user32.SetForegroundWindow(self.hwnd)
        self.running = True
        self.ready_event.set()

        self.log("=" * 65)
        self.log(" MAKCU V4 - 1000 HZ PRECISION RAW INPUT BENCHMARK")
        self.log("=" * 65)
        self.log("[+] Боевой режим Windows (Combat Mode) АКТИВИРОВАН:")
        c_prio = self.combat_status.get('high_priority', False)
        c_eco = self.combat_status.get('ecoqos_disabled', False)
        c_mmcss = self.combat_status.get('mmcss_games', False)
        self.log(f"    MMCSS: {c_mmcss} | High Priority: {c_prio} | EcoQoS Off: {c_eco}")
        self.log("[+] Фоновый перехват (RIDEV_INPUTSINK) и глобальные горячие клавиши активны!")
        self.log("[+] Можно открыть Chrome (тестер полинга) и запускать тесты прямо там:")
        self.log("    [W] или [K] - ШИРОКИЙ ХАОС 5 СЕК (размашистый тест под Keymap Labs)")
        self.log("    [7] / Numpad 7 - тест фиксированного времени 200 мс (100% точность)")
        self.log("    [0] / Numpad 0 - быстрый круг 5 сек (125 Гц UDP / 500 Гц HID)")
        self.log("    [P] - замер физической мыши (3 сек)")
        self.log("    [1]..[5] или Numpad 1..5 - аппаратные стресс-тесты шины MAKCU")
        self.log("    [6], [8], [9] - боевые движения Human Driver")
        self.log("    [S] - смена mouse_spread (0% сплайн выкл / 5% / 50%)")
        self.log("    [C] - очистка лога | [Esc] - выход")
        self.log("=" * 65)

        msg = wintypes.MSG()
        while self.running:
            res = user32.GetMessageW(ctypes.byref(msg), 0, 0, 0)
            if res <= 0:
                break
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

    def _on_action(self, cmd_id: int):
        if cmd_id not in (100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114):
            return
        if cmd_id == 110:
            self.clear_log()
            return
        if cmd_id == 111:
            self.running = False
            user32.PostMessageW(self.hwnd, 0x0010, 0, 0)
            return

        if self.test_busy:
            return  # Игнорируем повторные клики пока тест не завершен

        self.test_busy = True
        threading.Thread(target=self._worker, args=(cmd_id,), daemon=True).start()

    def _worker(self, cmd_id: int):
        enable_combat_mode()
        if self.hwnd:
            user32.SetWindowTextW(self.hwnd, "MAKCU V4 - [ТЕСТ ВЫПОЛНЯЕТСЯ...]")
        try:
            if cmd_id == 100:
                self._run_physical_mouse(duration_s=3.0)
            elif cmd_id == 101:
                self._run_benchmark_test("Одиночный 300 (0x18)", 300, 1, 0, False)
            elif cmd_id == 102:
                self._run_benchmark_test("Серия 125 Гц (0x18)", 300, 30, 125, False)
            elif cmd_id == 103:
                self._run_benchmark_test("Серия 250 Гц (0x18)", 300, 30, 250, False)
            elif cmd_id == 104:
                self._run_benchmark_test("Серия 500 Гц (0x18) - Коллизии!", 300, 30, 500, False)
            elif cmd_id == 105:
                self._run_benchmark_test("Серия 1000 Гц (0x18) - 80% Дроп!", 300, 30, 1000, False)
            elif cmd_id == 106:
                self._run_driver_move("Драйвер: Авто-расчёт времени (Закон Фиттса)", 300.0, 0.0, None)
            elif cmd_id == 107:
                self._run_driver_move("Драйвер: Фиксированное время 200 мс", 300.0, 0.0, 200.0)
            elif cmd_id == 108:
                self._run_driver_move("Драйвер: Диагональный вектор (150, 75)", 150.0, 75.0, None)
            elif cmd_id == 109:
                self._run_driver_flick_and_shot(200.0, -100.0)
            elif cmd_id == 112:
                self._run_driver_circle(duration_s=5.0, radius=180.0, revs_per_sec=2.5)
            elif cmd_id == 114:
                self._run_driver_chaotic_sweep(duration_s=5.0, amplitude_x=450.0, amplitude_y=300.0)
            elif cmd_id == 113:
                self._toggle_mouse_spread()
        finally:
            self.test_busy = False
            if self.hwnd:
                user32.SetWindowTextW(self.hwnd, "MAKCU V4 - Precision Raw Input Benchmark (1000 Hz GUI)")

    def _get_spread_btn_text(self, val: Optional[int]) -> str:
        if val is None:
            return "[S] mouse_spread: ? (Чтение...)"
        if val == 0:
            return "[S] mouse_spread: 0% (Сплайн ВЫКЛ)"
        if val == 5:
            return "[S] mouse_spread: 5% (Мягкое 2мс)"
        if val == 50:
            return "[S] mouse_spread: 50% (Заводское 17мс)"
        return f"[S] mouse_spread: {val}%"

    def _init_spread_state(self):
        time.sleep(0.3)
        val = get_device_mouse_spread(TARGET[0], TARGET[1])
        self.current_spread = val
        if self.btn_spread_hwnd and val is not None:
            user32.SetWindowTextW(self.btn_spread_hwnd, self._get_spread_btn_text(val))
        if val is not None:
            if val == 0:
                self.log(f"[+] Аппаратный mouse_spread: 0% [Сплайн ВЫКЛ, 0 мс лаг, прямая выдача]")
            else:
                self.log(f"[!] Аппаратный mouse_spread: {val}% [Сплайн ВКЛ, задержка до 17 мс! Нажмите [S] для 0%]")
        else:
            self.log("[-] Не удалось получить mouse_spread с платы (проверьте сеть).")

    def _toggle_mouse_spread(self):
        self.log("\n" + "=" * 65)
        self.log(" НАСТРОЙКА ПЛАТЫ: mouse_spread (Аппаратное слияние / Сплайн)")
        self.log("=" * 65)
        curr = self.current_spread
        if curr is None:
            self.log("[*] Чтение текущего состояния платы...")
            curr = get_device_mouse_spread(TARGET[0], TARGET[1])
            self.current_spread = curr

        if curr == 0:
            next_val = 5
        elif curr == 5:
            next_val = 50
        elif curr == 50:
            next_val = 0
        else:
            next_val = 0

        modes = {
            0: "0% (Сплайн ОТКЛЮЧЕН, 0 мс задержки, прямая выдача в USB HID)",
            5: "5% (Мягкое слияние векторов ~1-2 мс)",
            50: "50% (Старый заводской фильтр MAKCU, размазывание до 17 мс)",
        }
        desc = modes.get(next_val, f"{next_val}%")

        self.log(f"[*] Смена значения: {curr if curr is not None else '?'}% -> {next_val}%")
        self.log(f"[*] Режим: {desc}")
        self.log("[*] Запись в NOR Flash памяти ESP32-S3...")

        ok, msg, actual = set_device_mouse_spread(next_val, TARGET[0], TARGET[1], save_to_nor=True)
        if ok:
            self.current_spread = actual
            if self.btn_spread_hwnd:
                user32.SetWindowTextW(self.btn_spread_hwnd, self._get_spread_btn_text(actual))
            self.log(f"[+] {msg}")
            if actual == 0:
                self.log("[+] РЕЗУЛЬТАТ: Аппаратный сплайн отключен. Отчеты идут 1:1 без сглаживания!")
            else:
                self.log(f"[+] РЕЗУЛЬТАТ: Установлен аппаратный фильтр слияния {actual}%.")
        else:
            self.log(f"[-] {msg}")
        self.log("=" * 65)

    def _log_timing_stats(self, dts: list[float], rec_count: int, elapsed_s: Optional[float] = None):
        """Единый стандарт вывода USB HID таймингов для всех тестов."""
        if not dts or rec_count == 0:
            self.log("[-] Тайминги USB HID: отчетов не зафиксировано")
            return

        avg_dt = sum(dts) / len(dts)
        min_dt = min(dts)
        max_dt = max(dts)
        eff_hz = 1000.0 / avg_dt if avg_dt > 0 else 0.0

        hz_1000 = sum(1 for d in dts if 0.5 <= d <= 1.5)
        hz_500  = sum(1 for d in dts if 1.5 < d <= 3.5)
        hz_125  = sum(1 for d in dts if 6.0 <= d <= 10.0)
        bursts  = sum(1 for d in dts if d < 0.5)

        self.log("[-] Тайминги USB HID:")
        if elapsed_s is not None and elapsed_s > 0:
            rate = rec_count / elapsed_s
            self.log(f"    Средний интервал:   {avg_dt:.3f} мс (Интервалы: {eff_hz:.1f} Гц | Поток: {rate:.1f} отч/с)")
        else:
            self.log(f"    Средний интервал:   {avg_dt:.3f} мс (Частота: {eff_hz:.1f} Гц)")
        self.log(f"    Разброс интервалов: min={min_dt:.3f} мс, max={max_dt:.3f} мс")
        self.log(f"    ~1.0 мс (1000 Гц):  {hz_1000:4d} шт. ({hz_1000/len(dts)*100:5.1f}%)")
        self.log(f"    ~2.0 мс (500 Гц):   {hz_500:4d} шт. ({hz_500/len(dts)*100:5.1f}%)")
        self.log(f"    ~8.0 мс (125 Гц):   {hz_125:4d} шт. ({hz_125/len(dts)*100:5.1f}%)")
        self.log(f"    Пачки (<0.5 мс):    {bursts:4d} шт. ({bursts/len(dts)*100:5.1f}%)")

    def _run_physical_mouse(self, duration_s: float = 3.0):
        self.log("\n" + "=" * 65)
        self.log(f" ТЕСТ: Замер физической мыши ({duration_s:.1f} сек)")
        self.log(" >>> ШЕВЕЛИТЕ МЫШЬЮ РУКОЙ ПРЯМО СЕЙЧАС! <<<")
        self.log(" Схема: Сенсор мыши -> USB HID контроллер -> Windows Raw Input")
        self.log("=" * 65)
        time.sleep(0.3)
        self.reset_reports()

        t0 = time.perf_counter()
        while time.perf_counter() - t0 < duration_s:
            time.sleep(0.05)
        elapsed_s = time.perf_counter() - t0

        sum_x, sum_y, rec_count, dts, _ = self.get_captured_stats()
        self.log(f"[*] Собрано отчетов USB HID:  {rec_count} шт. (dx={sum_x:+d}, dy={sum_y:+d})")
        self.log(f"[+] Длительность замера:      {elapsed_s:.2f} сек")
        self._log_timing_stats(dts, rec_count, elapsed_s)
        self.log("=" * 65)

    def _run_benchmark_test(self, title: str, total_mickeys: int, count: int, hz: int, use_move_now: bool):
        proto_name = "m.move_now" if use_move_now else "0x18 MOVE"
        step = int(total_mickeys / count)
        dt = 1.0 / hz if hz > 0 else 0

        self.log("\n" + "=" * 65)
        self.log(f" ТЕСТ: {title}")
        self.log(f" Параметры: Дистанция={total_mickeys} mickeys, Частота={hz} Гц (шаг {dt*1000:.2f} мс)")
        self.log(f" Схема: UDP {proto_name} -> ESP32-S3 -> 1000 Гц USB HID")
        self.log("=" * 65)

        time.sleep(0.15)
        self.reset_reports()

        t0 = time.perf_counter()
        for i in range(1, count + 1):
            pkt = make_move_now_packet(step, 0) if use_move_now else make_opcode_18_packet(step, 0)
            self.sock.sendto(pkt, TARGET)
            target_t = t0 + i * dt
            while time.perf_counter() < target_t:
                pass
        elapsed_s = time.perf_counter() - t0

        time.sleep(0.15)
        sum_dx, sum_dy, rec_count, dts, _ = self.get_captured_stats()
        loss_mickeys = total_mickeys - sum_dx
        loss_percent = (loss_mickeys / total_mickeys) * 100.0 if total_mickeys else 0

        self.log(f"[*] Отправлено по UDP:        {total_mickeys:+4d} mickeys ({count} пакетов)")
        self.log(f"[+] Получено в Windows RAW:   {sum_dx:+4d} mickeys ({rec_count} отчетов USB HID)")
        self.log(f"[-] Ошибка доставки:          dx={-loss_mickeys:+d} mickeys ({loss_percent:.1f}% потерь)")

        if loss_mickeys == 0:
            self.log("[+] РЕЗУЛЬТАТ: 100.0% доставка, абсолютная точность (0 ошибок)!")
        else:
            self.log(f"[!] РЕЗУЛЬТАТ: Потеряно {loss_mickeys} mickeys ({loss_percent:.1f}% пути)!")

        self._log_timing_stats(dts, rec_count, elapsed_s)
        self.log("=" * 65)

    def _run_driver_move(self, title: str, dx: float, dy: float, dur_ms: Optional[float] = None):
        self.log("\n" + "=" * 65)
        self.log(f" ТЕСТ: {title}")
        dur_desc = f"{dur_ms:.0f} мс" if dur_ms is not None else "Авто (Фиттс)"
        self.log(f" Параметры: Вектор=({dx:+.0f}, {dy:+.0f}) mickeys, Тайминг={dur_desc}")
        self.log(" Схема: ABCurves H80 (1000 Гц) -> 125 Гц UDP (FreeRTOS) -> 500 Гц USB HID")
        self.log("=" * 65)

        time.sleep(0.15)
        self.reset_reports()

        t_start = time.perf_counter()
        meta = self.driver.move(dx, dy, duration_ms=dur_ms, dt_frame=0.008, blocking=True)
        elapsed_ms = (time.perf_counter() - t_start) * 1000.0

        # Ожидаем завершения очереди USB
        time.sleep(0.15)

        sum_x, sum_y, rec_count, dts, _ = self.get_captured_stats()
        diff_x = sum_x - int(round(dx))
        diff_y = sum_y - int(round(dy))

        mode_str = "ABCurves H80 (Нейросеть GRU)" if meta.get("used_model") else "Q16 Накопитель"
        calc_dur = meta.get("duration_ms", 0.0)
        frames = meta.get("frames_sent", 0)

        dt_f = meta.get("dt_frame", 0.008)
        hz_net = int(round(1.0 / dt_f)) if dt_f > 0 else 125
        self.log(f"[*] Сглаживание:              {mode_str}")
        self.log(f"[*] Тайминг движения:         {calc_dur:.0f} мс (факт {elapsed_ms:.1f} мс)")
        self.log(f"[*] Кадров отправлено по UDP: {frames} шт. (сетка {dt_f*1000:.1f} мс / {hz_net} Гц)")
        self.log(f"[*] Отправлено драйвером:     {meta.get('sent_x', 0):+4d} X, {meta.get('sent_y', 0):+4d} Y")
        self.log(f"[+] Получено в Windows RAW:   {sum_x:+4d} X, {sum_y:+4d} Y ({rec_count} отчетов USB HID)")
        self.log(f"[-] Ошибка позиционирования:  dx={diff_x:+d} mickeys, dy={diff_y:+d} mickeys")

        if diff_x == 0 and diff_y == 0:
            self.log("[+] РЕЗУЛЬТАТ: 100.0% доставка, абсолютная точность (0 ошибок)!")
        else:
            self.log(f"[!] РЕЗУЛЬТАТ: Ошибка позиционирования (dx={diff_x:+d}, dy={diff_y:+d})!")

        self._log_timing_stats(dts, rec_count, elapsed_ms / 1000.0)
        self.log("=" * 65)

    def _run_driver_flick_and_shot(self, dx: float = 200.0, dy: float = -100.0):
        self.log("\n" + "=" * 65)
        self.log(" ТЕСТ: Боевой флик + Выстрел (ЛКМ) + Возврат")
        self.log(f" Параметры: Флик=({dx:+.0f}, {dy:+.0f}) mickeys -> Клик (hold 45 мс) -> Возврат")
        self.log(" Схема: ABCurves H80 (1000 Гц) -> 125 Гц UDP (FreeRTOS) -> 500 Гц USB HID")
        self.log("=" * 65)

        time.sleep(0.15)
        self.reset_reports()

        self.log("[*] Шаг 1: Выполняем боевой флик на цель...")
        t0 = time.perf_counter()
        meta = self.driver.move(dx, dy, dt_frame=0.008, blocking=True)
        t_move = (time.perf_counter() - t0) * 1000.0

        self.log("[*] Шаг 2: Производим выстрел (аппаратный клик ЛКМ)...")
        t_clk0 = time.perf_counter()
        self.driver.click("left", hold_ms=45.0)
        t_clk = (time.perf_counter() - t_clk0) * 1000.0

        time.sleep(0.15)
        sum_x, sum_y, rec_count, dts, btns = self.get_captured_stats()

        diff_x = sum_x - int(round(dx))
        diff_y = sum_y - int(round(dy))

        lmb_downs = sum(1 for _, b in btns if b & 0x0001)
        lmb_ups = sum(1 for _, b in btns if b & 0x0002)

        self.log(f"[*] Флик выполнен за {t_move:.1f} мс ({meta.get('frames_sent', 0)} кадров UDP)")
        self.log(f"[*] Отправлено драйвером:     {meta.get('sent_x', 0):+4d} X, {meta.get('sent_y', 0):+4d} Y")
        self.log(f"[+] Получено в Windows RAW:   {sum_x:+4d} X, {sum_y:+4d} Y ({rec_count} отчетов USB HID)")
        self.log(f"[-] Ошибка позиционирования:  dx={diff_x:+d} mickeys, dy={diff_y:+d} mickeys")

        if diff_x == 0 and diff_y == 0:
            self.log("[+] РЕЗУЛЬТАТ: 100.0% доставка, абсолютная точность (0 ошибок)!")
        else:
            self.log(f"[!] РЕЗУЛЬТАТ: Ошибка позиционирования (dx={diff_x:+d}, dy={diff_y:+d})!")

        self.log(f"[+] Клик ЛКМ в Raw Input:     DOWN={lmb_downs} шт., UP={lmb_ups} шт. ({t_clk:.1f} мс)")
        if lmb_downs >= 1 and lmb_ups >= 1:
            self.log("[+] Выстрел зарегистрирован Windows Raw Input с аппаратного микроконтроллера!")

        self._log_timing_stats(dts, rec_count, (t_move + t_clk) / 1000.0)

        # Шаг 3: Возвращаем курсор на исходную позицию...
        self.log("[*] Шаг 3: Возвращаем курсор на исходную позицию...")
        time.sleep(0.3)
        self.driver.move(-dx, -dy, duration_ms=200.0, dt_frame=0.008, blocking=True)
        self.log("[+] Мышь аккуратно возвращена в точку старта.")
        self.log("=" * 65)

    def _run_driver_circle(self, duration_s: float = 5.0, radius: float = 180.0, revs_per_sec: float = 2.5):
        self.log("\n" + "=" * 65)
        self.log(f" ТЕСТ: Быстрое круговое движение драйвера ({duration_s:.1f} сек)")
        dt_target = 0.008
        hz_target = int(round(1.0 / dt_target))
        self.log(f" Схема: ABCurves H80 (1000 Гц) -> {hz_target} Гц UDP (FreeRTOS) -> 500 Гц USB HID")
        self.log("=" * 65)

        time.sleep(0.2)
        self.reset_reports()

        self.log(f"[*] Драйвер выполняет непрерывное вращение {duration_s:.1f} сек...")
        t_start = time.perf_counter()
        meta = self.driver.circle(radius=radius, duration_s=duration_s, revs_per_sec=revs_per_sec, dt_frame=dt_target, blocking=True)
        elapsed_s = time.perf_counter() - t_start

        # Ожидаем завершения очереди USB
        time.sleep(0.2)

        sum_x, sum_y, rec_count, dts, _ = self.get_captured_stats()
        diff_x = sum_x
        diff_y = sum_y

        mode_str = "ABCurves H80 (Нейросеть GRU)" if meta.get("used_model") else "Q16 Накопитель"
        dt_actual = meta.get("dt_frame", dt_target)
        hz_actual = int(round(1.0 / dt_actual)) if dt_actual > 0 else hz_target

        self.log(f"[*] Сглаживание:              {mode_str}")
        self.log(f"[*] Тайминг движения:         {duration_s*1000:.0f} мс (факт {elapsed_s*1000:.1f} мс)")
        self.log(f"[*] Кадров отправлено по UDP: {meta.get('frames_sent', 0)} шт. (сетка {dt_actual*1000:.1f} мс / {hz_actual} Гц)")
        self.log(f"[*] Отправлено драйвером:     {meta.get('sent_x', 0):+4d} X, {meta.get('sent_y', 0):+4d} Y")
        self.log(f"[+] Получено в Windows RAW:   {sum_x:+4d} X, {sum_y:+4d} Y ({rec_count} отчетов USB HID)")
        self.log(f"[-] Дрейф курсора от старта:  dx={diff_x:+d} mickeys, dy={diff_y:+d} mickeys")

        if abs(diff_x) <= 2 and abs(diff_y) <= 2:
            self.log("[+] РЕЗУЛЬТАТ: 100.0% доставка, контур замкнут (0 ошибок)!")
        else:
            self.log(f"[!] РЕЗУЛЬТАТ: Дрейф курсора (dx={diff_x:+d}, dy={diff_y:+d})!")

        self._log_timing_stats(dts, rec_count, elapsed_s)
        self.log("=" * 65)

    def _run_driver_chaotic_sweep(self, duration_s: float = 5.0, amplitude_x: float = 450.0, amplitude_y: float = 300.0):
        self.log("\n" + "=" * 65)
        self.log(f" ТЕСТ: Широкие хаотичные движения драйвера ({duration_s:.1f} сек)")
        self.log(f" Параметры: Размах=(±{amplitude_x:.0f} X, ±{amplitude_y:.0f} Y), Тайминг={duration_s*1000:.0f} мс")
        self.log(" Назначение: Замер частоты опроса в Keymap Labs / MouseRateChecker")
        self.log(" Схема: ABCurves H80 (1000 Гц) -> 125 Гц UDP (FreeRTOS) -> 500 Гц USB HID")
        self.log("=" * 65)

        time.sleep(0.2)
        self.reset_reports()

        self.log(f"[*] Драйвер выполняет непрерывный размашистый хаотичный поток {duration_s:.1f} сек...")
        t_start = time.perf_counter()
        meta = self.driver.chaotic_sweep(duration_s=duration_s, amplitude_x=amplitude_x, amplitude_y=amplitude_y, dt_frame=0.008, blocking=True)
        elapsed_s = time.perf_counter() - t_start

        # Ожидаем завершения очереди USB
        time.sleep(0.2)

        sum_x, sum_y, rec_count, dts, _ = self.get_captured_stats()
        diff_x = sum_x
        diff_y = sum_y

        mode_str = "ABCurves H80 (Нейросеть GRU)" if meta.get("used_model") else "Q16 Накопитель"
        dt_actual = meta.get("dt_frame", 0.008)
        hz_actual = int(round(1.0 / dt_actual)) if dt_actual > 0 else 125

        self.log(f"[*] Сглаживание:              {mode_str}")
        self.log(f"[*] Тайминг движения:         {duration_s*1000:.0f} мс (факт {elapsed_s*1000:.1f} мс)")
        self.log(f"[*] Кадров отправлено по UDP: {meta.get('frames_sent', 0)} шт. (сетка {dt_actual*1000:.1f} мс / {hz_actual} Гц)")
        self.log(f"[*] Отправлено драйвером:     {meta.get('sent_x', 0):+4d} X, {meta.get('sent_y', 0):+4d} Y")
        self.log(f"[+] Получено в Windows RAW:   {sum_x:+4d} X, {sum_y:+4d} Y ({rec_count} отчетов USB HID)")
        self.log(f"[-] Дрейф курсора от старта:  dx={diff_x:+d} mickeys, dy={diff_y:+d} mickeys")

        if abs(diff_x) <= 2 and abs(diff_y) <= 2:
            self.log("[+] РЕЗУЛЬТАТ: 100.0% доставка, контур замкнут (0 ошибок)!")
        else:
            self.log(f"[!] РЕЗУЛЬТАТ: Дрейф курсора (dx={diff_x:+d}, dy={diff_y:+d})!")

        self._log_timing_stats(dts, rec_count, elapsed_s)
        self.log("=" * 65)

    def run(self):
        try:
            while self.running:
                time.sleep(0.1)
        finally:
            self.running = False
            self._unregister_hotkeys()
            self.sock.close()
            if hasattr(self, 'driver') and self.driver:
                self.driver.close()


def main():
    app = BenchmarkApp()
    app.run()


if __name__ == "__main__":
    main()
