#!/usr/bin/env python3
"""
MAKCU Direct Controller — Чистый контроллер по UDP и бинарным опкодам MAK_API (mak-suite).
Никакого оверинжиниринга:
- Прямой бинарный опкод 0x18 (SET MOVE) для движения мыши
- Прямой бинарный опкод 0x11 (LEFT) для кликов
- Прямой бинарный опкод 0x04 для чтения версии прошивки
- Поддержка 100 Гц, 125 Гц, 500 Гц и 1000 Гц с аппаратным микросекундным таймингом Windows
"""

import sys
import time
import math
import struct
import socket
import secrets
import ctypes

BOARD_IP = "192.168.50.175"
BOARD_PORT = 8080
TARGET = (BOARD_IP, BOARD_PORT)

# Опкоды спецификации MAK_API (mak-suite)
OPCODE_DEVICE = 0x02
OPCODE_FIRMWARE = 0x04
OPCODE_LEFT_BUTTON = 0x11
OPCODE_RIGHT_BUTTON = 0x12
OPCODE_MOVE = 0x18

# Высокоточные мультимедийные таймеры Windows (1 мс)
_winmm = None
if sys.platform == "win32":
    try:
        _winmm = ctypes.WinDLL("winmm")
        _winmm.timeBeginPeriod(1)
        kernel32 = ctypes.WinDLL("kernel32")
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00000080)  # HIGH_PRIORITY
    except Exception:
        pass


def create_udp_socket() -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
    sock.settimeout(0.5)
    return sock


def get_firmware_version(sock: socket.socket) -> int | None:
    """Запрос версии прошивки через бинарный опкод 0x04 (GET FIRMWARE_VERSION)."""
    frame = b"\xDE\xAD\x00\x00\x04"
    packet = b"\x55" + secrets.token_bytes(8) + frame
    try:
        sock.sendto(packet, TARGET)
        resp, _ = sock.recvfrom(1024)
        if len(resp) >= 18 and resp[9:11] == b"\xDE\xAD" and resp[13] == OPCODE_FIRMWARE:
            fw = struct.unpack("<I", resp[14:18])[0]
            return fw
    except Exception:
        pass
    return None


def send_move_opcode(sock: socket.socket, dx: int, dy: int):
    """
    Отправка смещения мыши через бинарный опкод 0x18:
    Формат кадра: DE AD | LEN=0x0004 | CMD=0x18 | dx:i16 dy:i16
    Обертка UDP RAW: 0x55 + 8-байт nonce + кадр
    """
    if dx == 0 and dy == 0:
        return
    payload = struct.pack("<hh", int(dx), int(dy))
    frame = b"\xDE\xAD\x04\x00\x18" + payload
    packet = b"\x55" + secrets.token_bytes(8) + frame
    sock.sendto(packet, TARGET)


def send_click_opcode(sock: socket.socket, button: str = "left"):
    """Отправка клика кнопкой через опкод 0x11 (LEFT) или 0x12 (RIGHT)."""
    cmd = OPCODE_LEFT_BUTTON if button == "left" else OPCODE_RIGHT_BUTTON
    # Нажатие (state=1)
    frame_press = b"\xDE\xAD\x01\x00" + bytes((cmd, 1))
    sock.sendto(b"\x55" + secrets.token_bytes(8) + frame_press, TARGET)
    time.sleep(0.03)
    # Отпускание (state=0)
    frame_rel = b"\xDE\xAD\x01\x00" + bytes((cmd, 0))
    sock.sendto(b"\x55" + secrets.token_bytes(8) + frame_rel, TARGET)


def precise_sleep_until(target_time: float):
    """Гибридный таймер: coarse sleep при запасе > 2мс + точный spin-wait для < 0.01мс джиттера."""
    rem = target_time - time.perf_counter()
    if rem > 0.002:
        time.sleep(rem - 0.0015)
    while time.perf_counter() < target_time:
        pass


def run_circle(sock: socket.socket, radius: int = 120, duration: float = 2.0, hz: int = 500):
    """
    Плавный круг через опкод 0x18.
    Интегральный остаточный дрейф строго равен (0, 0).
    """
    steps = max(10, int(duration * hz))
    dt = 1.0 / hz

    print(f"[*] Круг: R={radius} отсчётов, T={duration:.1f}с, {hz} Гц (шаг {dt*1000:.2f}мс) через Опкод 0x18")

    prev_x, prev_y = float(radius), 0.0
    start_time = time.perf_counter()
    sent_count = 0

    for step in range(1, steps + 1):
        theta = 2.0 * math.pi * (step / steps)
        cur_x = radius * math.cos(theta)
        cur_y = radius * math.sin(theta)

        dx = int(round(cur_x - prev_x))
        dy = int(round(cur_y - prev_y))
        prev_x += dx
        prev_y += dy

        if dx != 0 or dy != 0:
            send_move_opcode(sock, dx, dy)
            sent_count += 1

        target_time = start_time + (step * dt)
        precise_sleep_until(target_time)

    actual_elapsed = time.perf_counter() - start_time
    actual_rate = steps / actual_elapsed
    drift_x = int(round(prev_x - radius))
    drift_y = int(round(prev_y))
    print(f"[+] Завершено за {actual_elapsed:.3f}с (фактический темп: {actual_rate:.1f} Гц, пакетов: {sent_count}). Дрейф: ({drift_x}, {drift_y})")


def run_infinite_circle(sock: socket.socket, radius: int = 120, period: float = 2.0, hz: int = 500):
    """Бесконечный непрерывный круг через опкод 0x18. Прерывается по Ctrl+C."""
    steps_per_loop = max(10, int(period * hz))
    dt = 1.0 / hz
    print(f"[*] Бесконечный круг (R={radius}, {hz} Гц, Опкод 0x18).")
    print("[*] Нажмите Ctrl+C для плавной остановки...")

    step = 0
    prev_x, prev_y = float(radius), 0.0
    t0 = time.perf_counter()

    try:
        while True:
            step += 1
            theta = 2.0 * math.pi * ((step % steps_per_loop) / steps_per_loop)
            cur_x = radius * math.cos(theta)
            cur_y = radius * math.sin(theta)

            dx = int(round(cur_x - prev_x))
            dy = int(round(cur_y - prev_y))
            prev_x += dx
            prev_y += dy

            if dx != 0 or dy != 0:
                send_move_opcode(sock, dx, dy)

            target_time = t0 + (step * dt)
            precise_sleep_until(target_time)

            if (target_time - time.perf_counter()) < -0.5:
                t0 = time.perf_counter() - (step * dt)

    except KeyboardInterrupt:
        print("\n[*] Остановка бесконечного круга...")
        rem_x = int(round(radius - prev_x))
        rem_y = int(round(0 - prev_y))
        if rem_x != 0 or rem_y != 0:
            send_move_opcode(sock, rem_x, rem_y)
        print("[+] Остановлено.")


def run_line(sock: socket.socket, total_dx: int = 200, total_dy: int = 0, duration: float = 1.0, hz: int = 500):
    """Плавная линия туда-обратно через опкод 0x18."""
    half_steps = max(5, int((duration / 2.0) * hz))
    half_dt = (duration / 2.0) / half_steps

    print(f"[*] Линия туда-обратно: d=({total_dx}, {total_dy}), T={duration}с, {hz} Гц")

    # Движение вперед
    accum_x, accum_y = 0, 0
    t0 = time.perf_counter()
    for s in range(1, half_steps + 1):
        target_x = int(round(total_dx * (s / half_steps)))
        target_y = int(round(total_dy * (s / half_steps)))
        dx = target_x - accum_x
        dy = target_y - accum_y
        accum_x += dx
        accum_y += dy
        if dx != 0 or dy != 0:
            send_move_opcode(sock, dx, dy)
        precise_sleep_until(t0 + s * half_dt)

    time.sleep(0.05)

    # Движение назад
    t0 = time.perf_counter()
    for s in range(1, half_steps + 1):
        target_x = int(round(total_dx * (1.0 - s / half_steps)))
        target_y = int(round(total_dy * (1.0 - s / half_steps)))
        dx = target_x - accum_x
        dy = target_y - accum_y
        accum_x += dx
        accum_y += dy
        if dx != 0 or dy != 0:
            send_move_opcode(sock, dx, dy)
        precise_sleep_until(t0 + s * half_dt)

    print(f"[+] Линия завершена. Итоговый остаток: ({accum_x}, {accum_y})")


def print_menu(cur_hz: int):
    print("\n" + "=" * 60)
    print("      MAKCU DIRECT CONTROLLER (БИНАРНЫЙ ОПКОД 0x18)")
    print(f" Цель: {BOARD_IP}:{BOARD_PORT} | Частота полинга = {cur_hz} Гц ({1000/cur_hz:.2f} мс)")
    print("=" * 60)
    print(" [1] Круг обычный (R=120 отсчётов, 2.0 сек)")
    print(" [2] Большой круг (R=250 отсчётов, 2.0 сек)")
    print(" [3] Быстрый круг (R=250 отсчётов, 1.2 сек — стресс-тест 1000Гц)")
    print(" [4] Бесконечный плавный круг (до нажатия Ctrl+C)")
    print(" [5] Горизонтальная линия туда-обратно (+-200 отсчётов)")
    print(" [6] Вертикальная линия туда-обратно (+-200 отсчётов)")
    print(" [7] Клик ЛКМ (Опкод 0x11)")
    print(" [8] Ручной сдвиг (ввод dx dy через опкод 0x18)")
    print(" [9] Проверить версию прошивки (Опкод 0x04)")
    print("-" * 60)
    print(" [F] Сменить частоту (100 / 125 / 500 / 1000 Гц)")
    print(" [0] Выход")
    print("=" * 60)


def main():
    sock = create_udp_socket()
    print(f"[*] Подключение к MAKCU ({BOARD_IP}:{BOARD_PORT})...")

    fw = get_firmware_version(sock)
    if fw:
        print(f"[+] Плата ответила на Опкод 0x04! Прошивка MAKCU: V{fw}")
    else:
        print("[!] Предупреждение: плата не ответила на 0x04 (возможно, занята или фильтрует ответы), но UDP сокет готов.")

    cur_hz = 500  # 500 Гц по умолчанию

    try:
        while True:
            print_menu(cur_hz)
            choice = input(f"Выберите действие [1-9 / F / 0]: ").strip().upper()

            if choice == "1":
                run_circle(sock, radius=120, duration=2.0, hz=cur_hz)
            elif choice == "2":
                run_circle(sock, radius=250, duration=2.0, hz=cur_hz)
            elif choice == "3":
                run_circle(sock, radius=250, duration=1.2, hz=cur_hz)
            elif choice == "4":
                run_infinite_circle(sock, radius=120, period=2.0, hz=cur_hz)
            elif choice == "5":
                run_line(sock, total_dx=200, total_dy=0, duration=1.0, hz=cur_hz)
            elif choice == "6":
                run_line(sock, total_dx=0, total_dy=200, duration=1.0, hz=cur_hz)
            elif choice == "7":
                print("[*] Клик ЛКМ через Опкод 0x11...")
                send_click_opcode(sock, "left")
                print("[+] Клик выполнен.")
            elif choice == "8":
                raw = input("Введите dx dy через пробел (например, 50 -30): ").strip()
                parts = raw.split()
                if len(parts) == 2:
                    try:
                        dx, dy = int(parts[0]), int(parts[1])
                        send_move_opcode(sock, dx, dy)
                        print(f"[+] Опкод 0x18 MOVE ({dx}, {dy}) отправлен.")
                    except ValueError:
                        print("[-] Ошибка: введите целые числа.")
                else:
                    print("[-] Ошибка формата.")
            elif choice == "9":
                ver = get_firmware_version(sock)
                if ver:
                    print(f"[+] Прошивка платы: V{ver}")
                else:
                    print("[-] Таймаут ответа на 0x04.")
            elif choice == "F":
                print("\nДоступные частоты:")
                print(" 1) 100 Гц (интервал 10.0 мс)")
                print(" 2) 125 Гц (интервал 8.0 мс — нативный такт FreeRTOS)")
                print(" 3) 500 Гц (интервал 2.0 мс — Sweet Spot)")
                print(" 4) 1000 Гц (интервал 1.0 мс — High-Speed)")
                f_ch = input("Выберите [1-4]: ").strip()
                if f_ch == "1":
                    cur_hz = 100
                elif f_ch == "2":
                    cur_hz = 125
                elif f_ch == "3":
                    cur_hz = 500
                elif f_ch == "4":
                    cur_hz = 1000
                print(f"[+] Частота установлена: {cur_hz} Гц")
            elif choice == "0":
                print("[*] Выход...")
                break
            else:
                print("[-] Неверный выбор.")
    finally:
        sock.close()
        if _winmm is not None:
            try:
                _winmm.timeEndPeriod(1)
            except Exception:
                pass
        print("[+] Сокет закрыт. Завершено.")


if __name__ == "__main__":
    main()
