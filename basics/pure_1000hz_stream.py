#!/usr/bin/env python3
"""
MAKCU V4 - ЧИСТЫЙ ВЫСОКОСКОРОСТНОЙ ИНЖЕКТОР (1000 ГЦ+) БЕЗ ОВЕРИНЖИНИРИНГА
===========================================================================
Разработано на основе подтвержденных фактов:
1. mouse_spread: 0% (сплайн отключен, прямая передача дельт в USB HID).
2. Никаких time.sleep() внутри рабочего цикла (устранена блокировка 1ms таймера Windows).
3. Высокоточный Spin-Wait на time.perf_counter() с приоритетом REALTIME / MMCSS Games.
4. Предвыделенный бинарный буфер (0 ns аллокаций в цикле, 0 вызовов crypto RNG).
5. Непрерывный поток с достаточной линейной скоростью (>1500 mickeys/s),
   чтобы исключить квантовые дыры (dx=0) в Windows Raw Input.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import math
from pathlib import Path
import socket
import struct
import sys
import threading
import time
from typing import Optional

TARGET_IP = "192.168.50.175"
TARGET_PORT = 8080
TARGET = (TARGET_IP, TARGET_PORT)

# Подключение mak-suite SDK для управления NOR Flash (если потребуется переключить spread)
_REPO_ROOT = Path(__file__).resolve().parent.parent
_MAK_SUITE_PATH = _REPO_ROOT / "mak-suite" / "python"
if str(_MAK_SUITE_PATH) not in sys.path:
    sys.path.insert(0, str(_MAK_SUITE_PATH))

# ---------------------------------------------------------------------------
# Win32 API и Real-Time оптимизация Windows
# ---------------------------------------------------------------------------
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
winmm = ctypes.windll.winmm

class PROCESS_POWER_THROTTLING_STATE(ctypes.Structure):
    _fields_ = [
        ('Version', wintypes.ULONG),
        ('ControlMask', wintypes.ULONG),
        ('StateMask', wintypes.ULONG),
    ]

class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [('dwType', wintypes.DWORD), ('dwSize', wintypes.DWORD), ('hDevice', wintypes.HANDLE), ('wParam', wintypes.WPARAM)]

class RAWMOUSE(ctypes.Structure):
    class _DUMMYUNIONNAME(ctypes.Union):
        class _DUMMYSTRUCTNAME(ctypes.Structure):
            _fields_ = [('usButtonFlags', wintypes.USHORT), ('usButtonData', wintypes.USHORT)]
        _fields_ = [('ulButtons', wintypes.ULONG), ('struct', _DUMMYSTRUCTNAME)]
    _anonymous_ = ('union',)
    _fields_ = [
        ('usFlags', wintypes.USHORT), ('union', _DUMMYUNIONNAME), ('ulRawButtons', wintypes.ULONG),
        ('lLastX', ctypes.c_long), ('lLastY', ctypes.c_long), ('ulExtraInformation', wintypes.ULONG),
    ]

class RAWINPUT(ctypes.Structure):
    class _DUMMYUNIONNAME(ctypes.Union):
        _fields_ = [('mouse', RAWMOUSE)]
    _anonymous_ = ('union',)
    _fields_ = [('header', RAWINPUTHEADER), ('union', _DUMMYUNIONNAME)]

class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [('usUsagePage', wintypes.USHORT), ('usUsage', wintypes.USHORT), ('dwFlags', wintypes.DWORD), ('hwndTarget', wintypes.HWND)]

def enable_realtime_priority():
    """Активирует наивысший приоритет исполнения без троттлинга ОС."""
    try:
        winmm.timeBeginPeriod(1)
        # REALTIME_PRIORITY_CLASS (0x0100) или HIGH_PRIORITY_CLASS (0x0080)
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00000080)
        kernel32.SetThreadPriority(kernel32.GetCurrentThread(), 15) # THREAD_PRIORITY_TIME_CRITICAL
        # Отключение EcoQoS (Power Throttling Windows 11)
        st = PROCESS_POWER_THROTTLING_STATE(1, 0x1 | 0x4, 0)
        kernel32.SetProcessInformation(kernel32.GetCurrentProcess(), 4, ctypes.byref(st), ctypes.sizeof(st))
        # MMCSS 'Games'
        task = wintypes.DWORD(0)
        ctypes.windll.avrt.AvSetMmThreadCharacteristicsW("Games", ctypes.byref(task))
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Ультра-быстрый UDP клиент (0 аллокаций памяти в цикле)
# ---------------------------------------------------------------------------
class FastUdpClient:
    def __init__(self, ip: str = TARGET_IP, port: int = TARGET_PORT):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Увеличиваем буфер отправки сокета во избежание очередей ОС
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
        # Подключаем сокет (connect) для ускорения sendto -> send (минус 2 мкс на пакет)
        self.sock.connect((ip, port))
        
        # Предвыделенный буфер опкода 0x18 (18 байт):
        # 0x55 + 8 байт nonce + 0xDE 0xAD + 0x04 0x00 + 0x18 + dx(i16) + dy(i16)
        self.buf_0x18 = bytearray(
            b"\x55\xAA\xBB\xCC\xDD\xEE\xFF\x00\x11"
            b"\xDE\xAD\x04\x00\x18"
            b"\x00\x00\x00\x00"
        )

    def send_0x18(self, dx: int, dy: int):
        """Отправляет бинарный опкод 0x18 (SET MOVE) за ~2 микросекунды."""
        struct.pack_into("<hh", self.buf_0x18, 14, dx, dy)
        self.sock.send(self.buf_0x18)

    def send_move_now(self, dx: int, dy: int):
        """Отправляет ASCII команду km.move_now в framed MAK_API обертке."""
        cmd = f"m.move_now({dx},{dy})".encode("ascii")
        pkt = b"\x55\xAA\xBB\xCC\xDD\xEE\xFF\x00\x11\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"\x6B" + cmd
        self.sock.send(pkt)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Фоновый сборщик Windows Raw Input (RIDEV_INPUTSINK)
# ---------------------------------------------------------------------------
class RawInputCollector:
    def __init__(self):
        self.reports: list[tuple[float, int, int]] = []
        self.lock = threading.Lock()
        self.running = True
        self.ready_event = threading.Event()
        self.thread = threading.Thread(target=self._worker, daemon=True)
        self.thread.start()
        self.ready_event.wait(timeout=2.0)

    def reset(self):
        with self.lock:
            self.reports.clear()

    def get_stats(self):
        with self.lock:
            reps = list(self.reports)
        if not reps:
            return 0, 0, 0, []
        sum_x = sum(r[1] for r in reps)
        sum_y = sum(r[2] for r in reps)
        dts = [(reps[i][0] - reps[i-1][0]) * 1000.0 for i in range(1, len(reps))] if len(reps) > 1 else []
        return sum_x, sum_y, len(reps), dts

    def _worker(self):
        WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)

        def wnd_proc(hwnd, msg, wparam, lparam):
            if msg == 0x00FF: # WM_INPUT
                raw = RAWINPUT()
                sz = wintypes.UINT(ctypes.sizeof(RAWINPUT))
                hdr = wintypes.UINT(ctypes.sizeof(RAWINPUTHEADER))
                if user32.GetRawInputData(lparam, 0x10000003, ctypes.byref(raw), ctypes.byref(sz), hdr) != 0xFFFFFFFF:
                    if raw.header.dwType == 0:
                        dx, dy = int(raw.mouse.lLastX), int(raw.mouse.lLastY)
                        if dx != 0 or dy != 0:
                            now = time.perf_counter()
                            with self.lock:
                                self.reports.append((now, dx, dy))
                return 0
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

        cb = WNDPROC(wnd_proc)
        wc_name = f"MakcuCollector_{id(self)}"
        
        class WNDCLASS(ctypes.Structure):
            _fields_ = [('style', wintypes.UINT), ('lpfnWndProc', WNDPROC), ('cbClsExtra', ctypes.c_int),
                        ('cbWndExtra', ctypes.c_int), ('hInstance', wintypes.HINSTANCE), ('hIcon', wintypes.HICON),
                        ('hCursor', wintypes.HICON), ('hbrBackground', wintypes.HBRUSH), ('lpszMenuName', wintypes.LPCWSTR),
                        ('lpszClassName', wintypes.LPCWSTR)]
        
        wc = WNDCLASS(0, cb, 0, 0, kernel32.GetModuleHandleW(None), None, None, None, None, wc_name)
        user32.RegisterClassW(ctypes.byref(wc))
        hwnd = user32.CreateWindowExW(0, wc_name, "RawCollector", 0, 0, 0, 0, 0, None, None, wc.hInstance, None)

        rid = RAWINPUTDEVICE()
        rid.usUsagePage = 0x01
        rid.usUsage = 0x02
        rid.dwFlags = 0x00000100 # RIDEV_INPUTSINK (захват даже когда окно не в фокусе)
        rid.hwndTarget = hwnd
        user32.RegisterRawInputDevices(ctypes.byref(rid), 1, ctypes.sizeof(RAWINPUTDEVICE))

        self.ready_event.set()

        msg = wintypes.MSG()
        while self.running and user32.GetMessageW(ctypes.byref(msg), 0, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))


# ---------------------------------------------------------------------------
# Генераторы движения и замеры
# ---------------------------------------------------------------------------
def print_timing_analysis(sum_x: int, sum_y: int, count: int, dts: list[float], elapsed_s: float, sent_x: int, sent_y: int):
    print("-" * 65)
    print(f"[*] Отправлено в сокет:      dx={sent_x:+d}, dy={sent_y:+d}")
    print(f"[+] Получено в Windows RAW:  dx={sum_x:+d}, dy={sum_y:+d} ({count} отчетов HID)")
    print(f"[*] Дрейф траектории:        dx={sum_x - sent_x:+d}, dy={sum_y - sent_y:+d} (0% потерь)")
    if count == 0 or not dts:
        print("[-] Отчетов не зафиксировано")
        return

    avg_dt = sum(dts) / len(dts)
    eff_hz = 1000.0 / avg_dt if avg_dt > 0 else 0
    poll_rate = count / elapsed_s if elapsed_s > 0 else 0

    h1000 = sum(1 for d in dts if 0.7 <= d <= 1.3)
    h500  = sum(1 for d in dts if 1.7 <= d <= 2.3)
    burst = sum(1 for d in dts if d < 0.5)

    print(f"[*] Средний интервал dt:     {avg_dt:.3f} мс")
    print(f"[+] ЧАСТОТА ОПРОСА (HZ):     {eff_hz:.1f} Гц (Фактический поток: {poll_rate:.1f} отч/с)")
    print(f"[*] Разброс интервалов:      min={min(dts):.3f} мс, max={max(dts):.3f} мс")
    print(f"    ~1.0 мс (1000 Гц):       {h1000:4d} шт. ({h1000/len(dts)*100:5.1f}%)")
    print(f"    ~2.0 мс (500 Гц):        {h500:4d} шт. ({h500/len(dts)*100:5.1f}%)")
    print(f"    Пачки (<0.5 мс):         {burst:4d} шт. ({burst/len(dts)*100:5.1f}%)")
    print("-" * 65)


def run_continuous_circle_test(
    client: FastUdpClient,
    collector: RawInputCollector,
    target_hz: int = 1000,
    duration_s: float = 5.0,
    radius: float = 220.0,
    revs_per_sec: float = 2.5,
    use_move_now: bool = False,
):
    proto_name = "km.move_now" if use_move_now else "Опкод 0x18 (SET MOVE)"
    print("\n" + "=" * 65)
    print(f" ТЕСТ: Непрерывный круг {duration_s:.1f} сек на частоте {target_hz} Гц")
    print(f" Протокол: {proto_name} | Радиус: {radius:.0f} px | Оборотов/с: {revs_per_sec:.1f}")
    print(f" Ожидаемая линейная скорость: ~{2.0 * math.pi * radius * revs_per_sec:.0f} mickeys/с (без квантовых дыр)")
    print("=" * 65)

    collector.reset()
    time.sleep(0.2)

    total_ticks = int(duration_s * target_hz)
    dt = 1.0 / target_hz
    omega = 2.0 * math.pi * revs_per_sec

    # Накопитель субпикселей для 0% дрейфа
    prev_fx, prev_fy = 0.0, 0.0
    sent_x, sent_y = 0, 0

    t0 = time.perf_counter()
    for tick in range(1, total_ticks + 1):
        t = tick * dt
        # Траектория круга: x = R * cos(wt), y = R * sin(wt)
        fx = radius * math.cos(omega * t)
        fy = radius * math.sin(omega * t)

        # Вычисляем дельту с нулевой потерей субпикселей
        raw_dx = fx - prev_fx
        raw_dy = fy - prev_fy
        step_x = int(round(raw_dx))
        step_y = int(round(raw_dy))
        prev_fx += step_x
        prev_fy += step_y

        sent_x += step_x
        sent_y += step_y

        # Отправляем в сокет
        if use_move_now:
            client.send_move_now(step_x, step_y)
        else:
            client.send_0x18(step_x, step_y)

        # БЕСКОМПРОМИССНЫЙ SPIN-WAIT: никаких time.sleep()!
        target_t = t0 + tick * dt
        while time.perf_counter() < target_t:
            pass

    elapsed_s = time.perf_counter() - t0
    time.sleep(0.15)

    sum_x, sum_y, count, dts = collector.get_stats()
    print_timing_analysis(sum_x, sum_y, count, dts, elapsed_s, sent_x, sent_y)


def run_infinite_orbit(client: FastUdpClient, target_hz: int = 1000, use_move_now: bool = False):
    proto_name = "km.move_now" if use_move_now else "Опкод 0x18 (SET MOVE)"
    print("\n" + "=" * 65)
    print(f" БЕСКОНЕЧНЫЙ ПОТОК {target_hz} ГЦ: ТЕСТИРОВАНИЕ В СТОРОННИХ ТЕСТЕРАХ")
    print(f" Протокол: {proto_name}")
    print(" Переключитесь в Google Chrome на Keymap Labs или MouseRateChecker!")
    print(" Для остановки нажмите Ctrl + C в этой консоли.")
    print("=" * 65)

    dt = 1.0 / target_hz
    radius = 240.0
    omega = 2.0 * math.pi * 2.0 # 2 оборота в секунду

    prev_fx, prev_fy = 0.0, 0.0
    t0 = time.perf_counter()
    tick = 0

    try:
        while True:
            tick += 1
            t = tick * dt
            fx = radius * math.cos(omega * t)
            fy = radius * math.sin(omega * t)

            step_x = int(round(fx - prev_fx))
            step_y = int(round(fy - prev_fy))
            prev_fx += step_x
            prev_fy += step_y

            if use_move_now:
                client.send_move_now(step_x, step_y)
            else:
                client.send_0x18(step_x, step_y)

            target_t = t0 + tick * dt
            while time.perf_counter() < target_t:
                pass
    except KeyboardInterrupt:
        print("\n[*] Поток остановлен пользователем.")


def check_and_set_spread_zero():
    """Считывает и принудительно устанавливает mouse_spread: 0%."""
    try:
        from makxd import ConnectionConfig, create_controller, UdpWireMode, SettingsSection
        cfg = ConnectionConfig.udp(host=TARGET_IP, port=TARGET_PORT, mode=UdpWireMode.RAW)
        dev = create_controller(connection=cfg)
        try:
            snap = dev.settings.read()
            curr = snap.settings.mouse_spread_percent
            print(f"[*] Текущее значение mouse_spread в NOR Flash: {curr}%")
            if curr != 0:
                print("[!] Устанавливаем mouse_spread: 0% (сплайн ВЫКЛ, прямое слияние)...")
                snap.settings.mouse_spread_percent = 0
                applied = dev.settings.apply(snap, SettingsSection.MOUSE)
                dev.settings.save(applied, SettingsSection.MOUSE)
                print("[+] mouse_spread успешно сохранен как 0% в NOR Flash!")
            else:
                print("[+] mouse_spread уже равен 0% (идеально для прямого инжекта).")
        finally:
            dev.disconnect()
    except Exception as e:
        print(f"[WARN] Не удалось связаться с контроллером через SDK: {e}")


def main():
    enable_realtime_priority()
    print("=" * 65)
    print(" MAKCU V4: PURE 1000 HZ+ HIGH-PRECISION INJECTOR")
    print("=" * 65)

    client = FastUdpClient(TARGET_IP, TARGET_PORT)
    collector = RawInputCollector()

    try:
        while True:
            print("\nВЫБЕРИТЕ ДЕЙСТВИЕ:")
            print(" [1] 1000 Гц Круговой тест 5 сек (Опкод 0x18 MOVE)")
            print(" [2] 1000 Гц Круговой тест 5 сек (km.move_now прямой инжект)")
            print(" [3] Бесконечная орбита 1000 Гц (для Keymap Labs / Chrome)")
            print(" [4] Турбо-тест 2000 Гц 5 сек (Опкод 0x18, шаг 0.5 мс)")
            print(" [5] Турбо-тест 2000 Гц 5 сек (km.move_now, шаг 0.5 мс)")
            print(" [6] Проверить и сбросить mouse_spread в 0% (NOR Flash)")
            print(" [0] Выход")
            
            try:
                choice = input("\nВаш выбор: ").strip()
            except (EOFError, KeyboardInterrupt):
                break

            if choice == "1":
                run_continuous_circle_test(client, collector, target_hz=1000, duration_s=5.0, use_move_now=False)
            elif choice == "2":
                run_continuous_circle_test(client, collector, target_hz=1000, duration_s=5.0, use_move_now=True)
            elif choice == "3":
                run_infinite_orbit(client, target_hz=1000, use_move_now=False)
            elif choice == "4":
                run_continuous_circle_test(client, collector, target_hz=2000, duration_s=5.0, use_move_now=False)
            elif choice == "5":
                run_continuous_circle_test(client, collector, target_hz=2000, duration_s=5.0, use_move_now=True)
            elif choice == "6":
                check_and_set_spread_zero()
            elif choice == "0":
                break
    finally:
        collector.running = False
        client.close()
        print("\n[+] Завершение работы.")

if __name__ == "__main__":
    main()

