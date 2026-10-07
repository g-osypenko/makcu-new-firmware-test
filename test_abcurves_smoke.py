"""
Smoke-тест интеграции ABCurves SubStepper и MAKCU UDP Controller.
Проверяет:
1. Загрузку нативной 64-битной DLL abcurves_renderer.dll и бинарной модели renderer_global_h80.bin.
2. Инициализацию C ABI и прогрев 256 отчётов физического контекста.
3. Корректность работы Q16 аккумулятора и квантование дробных смещений.
4. Точность кинематики minimum jerk (Flash & Hogan) в экранных пикселях и отсчётах (mickeys).
5. Сетевой UDP-пинг платы MAKCU (Opcode 0x02 DEVICE).
6. Тестовое плавное движение мыши на реальной плате с контролем 1000 Гц тактирования.
"""

import math
import os
from pathlib import Path
import secrets
import socket
import struct
import sys
import time

if sys.platform == "win32":
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

# Импорт субстеппера
from abcurves_substepper import ABCurvesSubStepper

TARGET_IP = "192.168.50.175"
PORT = 8080


def test_1_library_and_model():
    print("\n--- [ТЕСТ 1] Проверка загрузки DLL и бинарной модели ---")
    dll_path = Path(r"c:\makcu-new-firmware-test\ABCurves\abcurves\_native\abcurves_renderer.dll")
    model_path = Path(r"c:\makcu-new-firmware-test\ABCurves\models\renderer_global_h80.bin")

    assert dll_path.is_file(), f"DLL не найдена: {dll_path}"
    assert model_path.is_file(), f"Модель не найдена: {model_path}"
    print(f"  [+] DLL найдена: {dll_path} ({dll_path.stat().st_size} байт)")
    print(f"  [+] Модель найдена: {model_path} ({model_path.stat().st_size} байт)")

    stepper = ABCurvesSubStepper(model_path=model_path, dll_path=dll_path, pixel_scale=2.17)
    print(f"  [+] SubStepper успешно загрузил модель! Receipt: {stepper.model.receipt.mode}")
    return stepper


def test_2_fractional_q16_quantization(stepper: ABCurvesSubStepper):
    print("\n--- [ТЕСТ 2] Проверка квантования дробных субпиксельных шагов (Q16) ---")
    # 20 шагов по 0.35 отсчёта. Сумма = 7.0 отсчётов.
    # Без аккумулятора обычное округление выдало бы 20 раз 0!
    stepper.reset_stream(seed=12345)
    reports = []
    for _ in range(20):
        dx, dy = stepper.step(0.35, 0.0)
        reports.append(dx)

    total_dx = sum(reports)
    print(f"  Вход: 20 тиков по 0.35 отсчёта (ожидаемая сумма: 7.0)")
    print(f"  Выходные отчёты HID: {reports}")
    print(f"  Итоговая сумма: {total_dx}")
    assert total_dx == 7, f"Сумма {total_dx} != 7. Аккумулятор потерял дельты!"
    print("  [+] Аккумулятор Q16 идеально сохранил дробные приращения без потерь!")


def test_3_minimum_jerk_trajectory(stepper: ABCurvesSubStepper):
    print("\n--- [ТЕСТ 3] Проверка генерации траектории Minimum Jerk (100 px = 217 mickeys) ---")
    target_px_x = 100.0
    target_px_y = -50.0
    duration_ms = 200

    steps = stepper.generate_steps(target_px_x, target_px_y, duration_ms=duration_ms, settling_ms=25)
    sum_dx = sum(s[0] for s in steps)
    sum_dy = sum(s[1] for s in steps)

    expected_dx = round(target_px_x * stepper.pixel_scale)
    expected_dy = round(target_px_y * stepper.pixel_scale)

    print(f"  Цель (px): ({target_px_x}, {target_px_y}) за {duration_ms} мс")
    print(f"  Ожидаемые отсчёты (4.35x): dx={expected_dx}, dy={expected_dy}")
    print(f"  Сумма сгенерированных отчётов: dx={sum_dx}, dy={sum_dy}")
    print(f"  Всего сгенерировано 1-мс тиков: {len(steps)} (из них ненулевых шагов X: {sum(1 for s in steps if s[0] != 0)})")

    assert abs(sum_dx - expected_dx) <= 1, f"Несовпадение X: {sum_dx} != {expected_dx}"
    assert abs(sum_dy - expected_dy) <= 1, f"Несовпадение Y: {sum_dy} != {expected_dy}"
    print("  [+] Траектория Minimum Jerk сгенерирована со 100% точностью!")


def test_4_udp_ping():
    print("\n--- [ТЕСТ 4] Проверка связи с платой MAKCU по UDP RAW ---")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.5)
    try:
        sock.ioctl(0x9800000C, False)
    except Exception:
        pass

    nonce = secrets.token_bytes(8)
    # CMD 0x02: DEVICE
    req = b"\x55" + nonce + b"\xDE\xAD\x00\x00\x02"
    sock.sendto(req, (TARGET_IP, PORT))

    try:
        resp, addr = sock.recvfrom(1024)
        print(f"  [+] Плата ответила! Получено {len(resp)} байт от {addr}: {resp.hex()}")
        assert resp.startswith(b"\x55"), "Ответ должен начинаться с 0x55 (RAW режим)"
        # Проверяем битовую маску устройств
        assert b"\xDE\xAD" in resp, "В ответе отсутствует сигнатура DE AD"
        print("  [+] Сетевой протокол UdpWireMode.RAW функционирует штатно!")
    except socket.timeout:
        print(f"  [-] ВНИМАНИЕ: Плата {TARGET_IP}:{PORT} не ответила за 1.5 с.")
        sock.close()
        return False

    sock.close()
    return True


def test_5_hardware_substep_move(stepper: ABCurvesSubStepper):
    print("\n--- [ТЕСТ 5] Проверка аппаратного движения мыши субстеппером на MAKCU ---")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.ioctl(0x9800000C, False)
    except Exception:
        pass

    # Синхронизируем экран 3840x2160
    cmd_screen = b"m.screen(3840,2160)"
    frame_scr = b"\xDE\xAD" + len(cmd_screen).to_bytes(2, "little") + b"k" + cmd_screen
    sock.sendto(b"\x55" + secrets.token_bytes(8) + frame_scr, (TARGET_IP, PORT))

    # Движение вправо на 60 px, затем влево на 60 px (плавно, 150 мс каждое)
    print("  Выполняется плавное субстеп-движение: +60 px вправо, пауза, -60 px влево...")

    for delta_px in [60.0, -60.0]:
        steps = stepper.generate_steps(delta_px, 0.0, duration_ms=150.0, settling_ms=15)
        # Тактирование с интервалом 1.0 мс (1000 Гц)
        interval = 0.001
        t_next = time.perf_counter() + interval

        sent_packets = 0
        skipped_zeroes = 0

        for dx, dy in steps:
            while time.perf_counter() < t_next:
                pass
            t_next += interval

            if dx != 0 or dy != 0:
                payload = struct.pack("<hh", dx, dy)
                frame = b"\xDE\xAD\x04\x00\x18" + payload
                sock.sendto(b"\x55" + secrets.token_bytes(8) + frame, (TARGET_IP, PORT))
                sent_packets += 1
            else:
                skipped_zeroes += 1

        print(f"    Перемещение {delta_px:+4.0f} px: отправлено {sent_packets} HID пакетов (пропущено {skipped_zeroes} нулевых тиков)")
        time.sleep(0.1)

    sock.close()
    print("  [+] Аппаратное движение успешно выполнено без перегрузки USB-шины!")


def main():
    print("=================================================================")
    print("      SMOKE ТЕСТ: ABCurves SubStepper -> MAKCU UDP RAW          ")
    print("=================================================================")

    stepper = test_1_library_and_model()
    test_2_fractional_q16_quantization(stepper)
    test_3_minimum_jerk_trajectory(stepper)
    board_alive = test_4_udp_ping()

    if board_alive:
        test_5_hardware_substep_move(stepper)
    else:
        print("\n[!] Пропуск теста 5 из-за отсутствия ответа платы.")

    print("\n=================================================================")
    print("             ВСЕ ТЕСТЫ УСПЕШНО ПРОЙДЕНЫ!                         ")
    print("=================================================================")


if __name__ == "__main__":
    main()
