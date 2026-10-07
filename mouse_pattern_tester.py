"""
MAKCU UDP Mouse Pattern Tester — Профессиональный калиброванный движок
Особенности:
1. Автоматическая калибровка 4K/2K/FullHD монитора:
   - Размах 250 px на 4K экране теперь РЕАЛЬНО проходит 250 экранных пикселей.
   - Масштаб: ~4.35 отсчёта на 1 пиксель (подтверждено лётным логом).
2. Синхронизация km.screen(W, H) с платой MAKCU.
3. Оптимальная рабочая частота 500 Гц (шаг ~10 отсчётов/такт, исключает затупы фильтра).
4. Поддержка обоих бэкендов: RAW UDP (чистые сокеты) и mak-suite SDK (makxd).
5. Покадровый Flight Recorder для фиксации точности движения.
"""

import ctypes
import math
import os
import secrets
import socket
import struct
import sys
import time

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
    """Определяет физическое разрешение экрана с учётом DPI."""
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
    return 1920, 1080


def get_cursor_pos() -> tuple[int, int]:
    """Считывает реальные экранные координаты курсора через Win32 API."""
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
    """
    Калибровка пересчёта экранных пикселей в отсчёты мыши (counts/pixel):
    - 4K (3840x2160): ~4.35 отсчёта на 1 пиксель (замерено по логу).
    - 2K (2560x1440): ~2.90 отсчёта на 1 пиксель.
    - FullHD (1920x1080): ~1.20 отсчёта на 1 пиксель.
    """
    if screen_w >= 3840 or screen_h >= 2160:
        return 4.35
    elif screen_w >= 2560 or screen_h >= 1440:
        return 2.90
    else:
        return 1.20


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
            print(f"\n[+] Полный лог сохранён: {os.path.abspath(self.filename)} ({len(self.records)} тактов)")
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
        print(f"Целевая амплитуда:    {expected_px:.0f} пикселей")
        print(f"Фактический размах X: {span_x} px (Амплитуда: {amp_x:.1f} px)")
        print(f"Фактический размах Y: {span_y} px (Амплитуда: {amp_y:.1f} px)")
        accuracy = (max(amp_x, amp_y) / expected_px * 100.0) if expected_px > 0 else 0
        print(f"Точность попадания:   {accuracy:.1f}%")
        print("===================================================\n")


# ---------------------------------------------------------------------------
# Тактовый генератор
# ---------------------------------------------------------------------------
class RatePacer:
    def __init__(self, target_hz: float = 500.0):
        self.interval = 1.0 / target_hz
        self.target_hz = target_hz
        self.next_tick = time.perf_counter() + self.interval
        self.tick_count = 0
        self.start_time = time.perf_counter()
        self.last_stat_time = self.start_time
        self.last_stat_ticks = 0
        self.current_hz = target_hz

    def set_hz(self, target_hz: float):
        self.target_hz = target_hz
        self.interval = 1.0 / target_hz
        self.next_tick = time.perf_counter() + self.interval

    def sync(self) -> float:
        while time.perf_counter() < self.next_tick:
            pass

        now = time.perf_counter()
        self.next_tick += self.interval
        if now - self.next_tick > 0.005:
            self.next_tick = now + self.interval

        self.tick_count += 1
        dt = now - self.last_stat_time
        if dt >= 0.4:
            self.current_hz = (self.tick_count - self.last_stat_ticks) / dt
            self.last_stat_time = now
            self.last_stat_ticks = self.tick_count

        return self.current_hz


# ---------------------------------------------------------------------------
# Бэкенды подключения (RAW UDP и mak-suite SDK)
# ---------------------------------------------------------------------------
class MakcuRawClient:
    backend_name = "RAW UDP (чистые сокеты)"

    def __init__(self, ip: str = TARGET_IP, port: int = PORT):
        self.target = (ip, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.sock.ioctl(0x9800000C, False)
        except Exception:
            pass

    def sync_screen(self, width: int, height: int):
        """Передаёт реальное разрешение экрана на плату MAKCU."""
        cmd = f"m.screen({width},{height})".encode("ascii")
        frame = b"\xDE\xAD" + len(cmd).to_bytes(2, "little") + b"k" + cmd
        packet = b"\x55" + secrets.token_bytes(8) + frame
        self.sock.sendto(packet, self.target)

    def move(self, dx: int, dy: int):
        if dx == 0 and dy == 0:
            return
        nonce = secrets.token_bytes(8)
        payload = struct.pack("<hh", int(dx), int(dy))
        frame = b"\xDE\xAD\x04\x00\x18" + payload
        packet = b"\x55" + nonce + frame
        self.sock.sendto(packet, self.target)

    def close(self):
        self.sock.close()


class MakcuSdkClient:
    backend_name = "mak-suite SDK (makxd)"

    def __init__(self, ip: str = TARGET_IP, port: int = PORT):
        sdk_path = os.path.abspath(r"c:\makcu-new-firmware-test\mak-suite\python")
        if sdk_path not in sys.path:
            sys.path.insert(0, sdk_path)

        from makxd import create_controller, ConnectionConfig, UdpWireMode

        print(f"[*] Подключение mak-suite SDK к {ip}:{port}...")
        cfg = ConnectionConfig.udp(
            host=ip,
            port=port,
            mode=UdpWireMode.RAW
        )
        self.device = create_controller(connection=cfg)
        print(f"[+] SDK подключён! Версия прошивки: {self.device.firmware_version()}")

    def sync_screen(self, width: int, height: int):
        cmd = f"m.screen({width},{height})".encode("ascii")
        try:
            self.device.transport.send_mak_api(0x6B, cmd, wait_response=False)
        except Exception:
            pass

    def move(self, dx: int, dy: int):
        if dx == 0 and dy == 0:
            return
        self.device.move(int(dx), int(dy))

    def close(self):
        try:
            self.device.disconnect()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Паттерны с экранной калибровкой
# ---------------------------------------------------------------------------

def run_pattern_line(client, pacer: RatePacer, duration: float = 12.0, axis: str = "x", amplitude_px: float = 250.0, period_sec: float = 1.4, pixel_scale: float = 4.35):
    recorder = FlightRecorder(LOG_FILENAME)
    counts_amplitude = amplitude_px * pixel_scale

    title = "ВЛЕВО <-> ВПРАВО" if axis == "x" else "ВВЕРХ <-> ВНИЗ"
    print(f"\n[>] Запущен режим: {title} (Экранный размах: ~{amplitude_px*2:.0f} px, Частота: {pacer.target_hz:.0f} Гц)")
    print("   [REC] Идёт запись в лог. Нажмите Ctrl+C для завершения.\n")

    steps_per_period = int(pacer.target_hz * period_sec)
    phase_step = (2 * math.pi) / steps_per_period

    phase = 0.0
    last_val = 0.0
    start_time = time.perf_counter()
    end_time = start_time + duration

    try:
        while time.perf_counter() < end_time:
            phase += phase_step
            curr_val = counts_amplitude * math.sin(phase)
            delta = int(round(curr_val - last_val))

            cmd_dx = delta if axis == "x" else 0
            cmd_dy = delta if axis == "y" else 0

            if delta != 0:
                client.move(cmd_dx, cmd_dy)
                last_val += delta

            recorder.record(cmd_dx, cmd_dy)
            hz = pacer.sync()

            if pacer.tick_count % 100 == 0:
                t = time.perf_counter() - start_time
                print(f"\r  [Монитор] {hz:5.1f} Гц | Прошло: {t:4.1f}с | Шаг: {delta:+3d} отсчётов", end="", flush=True)

    except KeyboardInterrupt:
        pass
    finally:
        recorder.save_and_analyze(amplitude_px)


def run_pattern_circle(client, pacer: RatePacer, duration: float = 15.0, radius_px: float = 250.0, period_sec: float = 1.6, pixel_scale: float = 4.35):
    recorder = FlightRecorder(LOG_FILENAME)
    counts_radius = radius_px * pixel_scale

    print(f"\n[>] Запущен режим: КРУГ (Экранный радиус: ~{radius_px:.0f} px, Частота: {pacer.target_hz:.0f} Гц)")
    print("   [REC] Идёт запись в лог. Нажмите Ctrl+C для завершения.\n")

    steps_per_rev = int(pacer.target_hz * period_sec)
    d_theta = (2 * math.pi) / steps_per_rev

    theta = 0.0
    last_x = counts_radius * math.cos(0.0)
    last_y = counts_radius * math.sin(0.0)

    start_time = time.perf_counter()
    end_time = start_time + duration

    try:
        while time.perf_counter() < end_time:
            theta += d_theta
            curr_x = counts_radius * math.cos(theta)
            curr_y = counts_radius * math.sin(theta)

            dx = int(round(curr_x - last_x))
            dy = int(round(curr_y - last_y))

            if dx != 0 or dy != 0:
                client.move(dx, dy)
                last_x += dx
                last_y += dy

            recorder.record(dx, dy)
            hz = pacer.sync()

            if pacer.tick_count % 100 == 0:
                t = time.perf_counter() - start_time
                print(f"\r  [Монитор] {hz:5.1f} Гц | Прошло: {t:4.1f}с | Шаг: ({dx:+3d}, {dy:+3d})", end="", flush=True)

    except KeyboardInterrupt:
        pass
    finally:
        recorder.save_and_analyze(radius_px)


def run_pattern_infinity(client, pacer: RatePacer, duration: float = 15.0, scale_px: float = 220.0, period_sec: float = 1.8, pixel_scale: float = 4.35):
    recorder = FlightRecorder(LOG_FILENAME)
    counts_scale = scale_px * pixel_scale

    print(f"\n[>] Запущен режим: ВОСЬМЁРКА (Масштаб: ~{scale_px:.0f} px, Частота: {pacer.target_hz:.0f} Гц)")
    print("   [REC] Идёт запись в лог. Нажмите Ctrl+C для завершения.\n")

    steps_per_cycle = int(pacer.target_hz * period_sec)
    dt_param = (2 * math.pi) / steps_per_cycle

    t_param = 0.0
    last_x = 0.0
    last_y = 0.0

    start_time = time.perf_counter()
    end_time = start_time + duration

    try:
        while time.perf_counter() < end_time:
            t_param += dt_param
            denom = 1 + math.sin(t_param) ** 2
            curr_x = counts_scale * math.cos(t_param) / denom
            curr_y = counts_scale * math.sin(t_param) * math.cos(t_param) / denom

            dx = int(round(curr_x - last_x))
            dy = int(round(curr_y - last_y))

            if dx != 0 or dy != 0:
                client.move(dx, dy)
                last_x += dx
                last_y += dy

            recorder.record(dx, dy)
            hz = pacer.sync()

            if pacer.tick_count % 100 == 0:
                t = time.perf_counter() - start_time
                print(f"\r  [Монитор] {hz:5.1f} Гц | Прошло: {t:4.1f}с | Шаг: ({dx:+3d}, {dy:+3d})", end="", flush=True)

    except KeyboardInterrupt:
        pass
    finally:
        recorder.save_and_analyze(scale_px)


def create_client(backend_choice: str):
    if backend_choice == "2":
        try:
            return MakcuSdkClient()
        except Exception as e:
            print(f"[-] Ошибка подключения через mak-suite SDK: {e}")
            print("[*] Переключение на RAW UDP (чистые сокеты)...")
            return MakcuRawClient()
    return MakcuRawClient()


def main():
    screen_w, screen_h = get_screen_resolution()
    pixel_scale = calculate_pixel_scale(screen_w, screen_h)

    print("===================================================")
    print("   MAKCU UDP: КАЛИБРОВАННЫЙ ЭКРАННЫЙ ТЕСТЕР        ")
    print("===================================================")
    print(f"[*] Дисплей: {screen_w}x{screen_h} | Калибровочный масштаб: {pixel_scale:.2f}x")
    print("1. RAW UDP (чистые сокеты) [По умолчанию]")
    print("2. mak-suite SDK (официальная библиотека)")
    print("===================================================")

    init_backend = input("Выберите бэкенд (1 или 2, Enter=1): ").strip()
    client = create_client(init_backend)

    # Синхронизируем разрешение экрана с платой
    client.sync_screen(screen_w, screen_h)

    # По умолчанию 500 Гц (шаг ~8-12 отсчетов, исключает размазывание фильтра)
    current_hz = 500.0
    pacer = RatePacer(current_hz)

    try:
        while True:
            menu = f"""
===================================================
      MAKCU UDP: ЭКРАННЫЙ ТЕСТЕР (ЧЕСТНЫЕ ПИКСЕЛИ)
  Бэкенд:     [{client.backend_name}]
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
8. ИЗМЕНИТЬ ЧАСТОТУ (500 Гц / 250 Гц / 1000 Гц)
9. НАСТРОЙКА МАСШТАБА (Текущий: {pixel_scale:.2f}x)
10. СМЕНИТЬ БЭКЕНД (RAW UDP <-> mak-suite)
0. Выход
===================================================
"""
            print(menu)
            choice = input("Выберите действие (0-10): ").strip()

            if choice == "1":
                run_pattern_line(client, pacer, duration=15.0, axis="x", amplitude_px=250.0, period_sec=1.4, pixel_scale=pixel_scale)
            elif choice == "2":
                run_pattern_line(client, pacer, duration=15.0, axis="y", amplitude_px=250.0, period_sec=1.4, pixel_scale=pixel_scale)
            elif choice == "3":
                run_pattern_circle(client, pacer, duration=15.0, radius_px=120.0, period_sec=1.3, pixel_scale=pixel_scale)
            elif choice == "4":
                run_pattern_circle(client, pacer, duration=15.0, radius_px=250.0, period_sec=1.6, pixel_scale=pixel_scale)
            elif choice == "5":
                run_pattern_circle(client, pacer, duration=15.0, radius_px=450.0, period_sec=1.8, pixel_scale=pixel_scale)
            elif choice == "6":
                run_pattern_infinity(client, pacer, duration=15.0, scale_px=220.0, period_sec=1.8, pixel_scale=pixel_scale)
            elif choice == "7":
                run_pattern_circle(client, pacer, duration=999999.0, radius_px=250.0, period_sec=1.6, pixel_scale=pixel_scale)
            elif choice == "8":
                print("\nВыберите целевую частоту пакетов:")
                print("1. 500 Гц  (РЕКОМЕНДУЕТСЯ: крупный шаг ~10 counts, ноль затупов фильтра)")
                print("2. 250 Гц  (Интервал 4.0 мс — максимальная стабильность)")
                print("3. 1000 Гц (Интервал 1.0 мс — родная частота MAKCU)")
                hz_choice = input("Выбор (1/2/3): ").strip()
                if hz_choice == "2":
                    pacer.set_hz(250.0)
                elif hz_choice == "3":
                    pacer.set_hz(1000.0)
                else:
                    pacer.set_hz(500.0)
                print(f"[+] Частота установлена на: {pacer.target_hz:.0f} Гц")
            elif choice == "9":
                print(f"\nТекущий масштаб: {pixel_scale:.2f}x")
                print("1. 4.35x (Калибровка для 4K 3840x2160)")
                print("2. 2.90x (Калибровка для 2K 2560x1440)")
                print("3. 1.20x (Калибровка для FullHD 1920x1080)")
                print("4. 1.00x (Чистые отсчёты без масштабирования)")
                sc_choice = input("Выбор (1-4): ").strip()
                if sc_choice == "2":
                    pixel_scale = 2.90
                elif sc_choice == "3":
                    pixel_scale = 1.20
                elif sc_choice == "4":
                    pixel_scale = 1.00
                else:
                    pixel_scale = 4.35
                print(f"[+] Масштаб установлен на: {pixel_scale:.2f}x")
            elif choice == "10":
                client.close()
                new_choice = "2" if isinstance(client, MakcuRawClient) else "1"
                print(f"\n[*] Переключение бэкенда...")
                client = create_client(new_choice)
                client.sync_screen(screen_w, screen_h)
                print(f"[+] Бэкенд изменён на: {client.backend_name}")
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
