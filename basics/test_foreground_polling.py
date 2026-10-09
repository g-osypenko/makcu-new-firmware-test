#!/usr/bin/env python3
"""
ТЕСТ ЧАСТОТЫ ОПРОСА МЫШИ (FOREGROUND vs BACKGROUND / ТРОТТЛИНГ WINDOWS 11).

Демонстрирует разницу:
1. FOREGROUND (Активное окно в фокусе):
   - Windows 11 передает все отчеты напрямую без задержек (1000 Гц).
2. BACKGROUND (Фоновое окно с RIDEV_INPUTSINK при активном VS Code):
   - Windows 11 принудительно объединяет (coalesce) и троттлит отчеты до ~125 Гц (8 мс)!
"""

import ctypes
from ctypes import wintypes
import time
import threading
import sys

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
winmm = ctypes.windll.winmm
winmm.timeBeginPeriod(1)

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


class PollingWindow:
    def __init__(self, use_background_sink: bool = False):
        self.use_background_sink = use_background_sink
        self.reports = []
        self.hwnd = None
        self.running = False
        self.lock = threading.Lock()
        self.ready_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        self.ready_event.wait()

    def _wnd_proc(self, hwnd, msg, wparam, lparam):
        if msg == 0x00FF:  # WM_INPUT
            raw = RAWINPUT()
            size = wintypes.UINT(ctypes.sizeof(RAWINPUT))
            header_size = wintypes.UINT(ctypes.sizeof(RAWINPUTHEADER))
            res = user32.GetRawInputData(lparam, 0x10000003, ctypes.byref(raw), ctypes.byref(size), header_size)
            if res != 0xFFFFFFFF and raw.header.dwType == 0:
                dx = int(raw.mouse.lLastX)
                dy = int(raw.mouse.lLastY)
                if dx != 0 or dy != 0:
                    now = time.perf_counter()
                    with self.lock:
                        self.reports.append((now, dx, dy))
            return 0
        elif msg == 0x000F:  # WM_PAINT
            ps = wintypes.PAINTSTRUCT()
            hdc = user32.BeginPaint(hwnd, ctypes.byref(ps))
            # Простой вывод текста
            text = "КЛИКНИТЕ СЮДА И ШЕВЕЛИТЕ МЫШЬЮ ДЛЯ ЗАМЕРА!"
            rect = wintypes.RECT(10, 50, 480, 100)
            user32.DrawTextW(hdc, text, len(text), ctypes.byref(rect), 0x00000001)  # DT_CENTER
            user32.EndPaint(hwnd, ctypes.byref(ps))
            return 0
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def _run(self):
        self.wnd_proc_cb = WNDPROC(self._wnd_proc)
        wc = WNDCLASSEXW()
        wc.cbSize = ctypes.sizeof(WNDCLASSEXW)
        wc.lpfnWndProc = self.wnd_proc_cb
        wc.hInstance = kernel32.GetModuleHandleW(None)
        wc.hbrBackground = 6  # COLOR_WINDOW + 1
        wc.lpszClassName = f"PollWinClass_{int(time.time()*1000)}"
        user32.RegisterClassExW(ctypes.byref(wc))

        title = "ТЕСТ ЧАСТОТЫ МЫШИ (ФОКУС)" if not self.use_background_sink else "ТЕСТ ЧАСТОТЫ МЫШИ (ФОН)"
        self.hwnd = user32.CreateWindowExW(
            0x00000008,  # WS_EX_TOPMOST
            wc.lpszClassName, title,
            0x00CF0000 | 0x10000000,  # WS_OVERLAPPEDWINDOW | WS_VISIBLE
            200, 200, 500, 250, None, None, wc.hInstance, None
        )

        rid = RAWINPUTDEVICE()
        rid.usUsagePage = 0x01
        rid.usUsage = 0x02
        if self.use_background_sink:
            rid.dwFlags = 0x00000100  # RIDEV_INPUTSINK (фоновый захват)
        else:
            rid.dwFlags = 0x00000000  # Только активное окно (FOREGROUND)
        rid.hwndTarget = self.hwnd
        user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(RAWINPUTDEVICE))

        user32.ShowWindow(self.hwnd, 5)  # SW_SHOW
        user32.SetForegroundWindow(self.hwnd)
        self.running = True
        self.ready_event.set()

        msg = wintypes.MSG()
        while self.running:
            res = user32.GetMessageW(ctypes.byref(msg), 0, 0, 0)
            if res <= 0:
                break
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

    def close(self):
        self.running = False
        if self.hwnd:
            user32.PostMessageW(self.hwnd, 0x0010, 0, 0)  # WM_CLOSE


def run_test(use_background_sink: bool, test_name: str):
    print("\n" + "=" * 70)
    print(f" ЗАПУСК ТЕСТА: {test_name}")
    if use_background_sink:
        print(" [!] Режим: Фоновый перехват (RIDEV_INPUTSINK) - кликните на VS Code!")
    else:
        print(" [!] Режим: АКТИВНОЕ ОКНО - кликните на открывшееся окно и шевелите мышь внутри!")
    print("=" * 70)

    win = PollingWindow(use_background_sink=use_background_sink)
    time.sleep(1.0)
    print("\n[>>>] НАЧИНАЙТЕ НЕПРЕРЫВНО ДВИГАТЬ МЫШЬ (3 секунды)...")
    win.reports.clear()
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 3.0:
        time.sleep(0.05)

    with win.lock:
        reps = list(win.reports)
    win.close()

    count = len(reps)
    print(f"\n[*] Собрано отчетов: {count} шт. за 3.0 секунды")
    if count < 2:
        print("[!] Недостаточно данных для анализа!")
        return

    dts = [(reps[i][0] - reps[i-1][0]) * 1000.0 for i in range(1, count)]
    avg_dt = sum(dts) / len(dts)
    min_dt = min(dts)
    max_dt = max(dts)
    eff_hz = 1000.0 / avg_dt if avg_dt > 0 else 0

    hz_1000 = sum(1 for d in dts if 0.5 <= d <= 1.5)
    hz_500  = sum(1 for d in dts if 1.5 < d <= 2.5)
    hz_125  = sum(1 for d in dts if 7.0 <= d <= 9.0)

    print(f"[-] Средний интервал: {avg_dt:.3f} мс (Средняя частота: {eff_hz:.1f} Гц)")
    print(f"    Минимум: {min_dt:.3f} мс, Максимум: {max_dt:.3f} мс")
    print(f"    ~1.0 мс (1000 Гц): {hz_1000:4d} шт. ({hz_1000/len(dts)*100:5.1f}%)")
    print(f"    ~2.0 мс (500 Гц):  {hz_500:4d} шт. ({hz_500/len(dts)*100:5.1f}%)")
    print(f"    ~8.0 мс (125 Гц):  {hz_125:4d} шт. ({hz_125/len(dts)*100:5.1f}%)")
    print("=" * 70)


def main():
    print("ТЕСТ СРАВНЕНИЯ ЧАСТОТЫ ОПРОСА В ОКНЕ И В ФОНЕ")
    print("1. Тест в АКТИВНОМ ОКНЕ (Foreground)")
    print("2. Тест в ФОНЕ (RIDEV_INPUTSINK с активным VS Code)")
    ch = input("Выберите (1 или 2): ").strip()
    if ch == "1":
        run_test(False, "АКТИВНОЕ ОКНО (FOREGROUND)")
    elif ch == "2":
        run_test(True, "ФОНОВОЙ ПЕРЕХВАТ (BACKGROUND)")

if __name__ == "__main__":
    main()

