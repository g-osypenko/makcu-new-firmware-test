#!/usr/bin/env python3
"""
ОСНОВЫ MAKCU: Минимальный скрипт прямого управления через бинарные опкоды MAK_API.
Без сторонних библиотек, без магии, без оберток — чистый стандартный Python (socket, struct, secrets).
"""

import socket
import struct
import secrets
import time

# Адрес платы MAKCU в локальной сети
BOARD_IP = "192.168.50.175"
BOARD_PORT = 8080
TARGET = (BOARD_IP, BOARD_PORT)

# Опкоды спецификации MAK_API
CMD_DEVICE   = 0x02  # Запрос типа устройства (Мышь / Клавиатура)
CMD_FIRMWARE = 0x04  # Запрос версии прошивки
CMD_MOVE     = 0x18  # Сдвиг мыши (dx: int16, dy: int16)
CMD_LEFT_BTN = 0x11  # Кнопка ЛКМ (state: uint8: 1=нажать, 0=отпустить)


def make_packet(cmd: int, payload: bytes = b"") -> tuple[bytes, bytes]:
    """
    Формирует сырой пакет для отправки по UDP RAW:
    0x55 + 8 байт nonce + 0xDE 0xAD + LEN (2 байта) + CMD (1 байт) + PAYLOAD
    """
    nonce = secrets.token_bytes(8)
    length = len(payload)
    frame = b"\xDE\xAD" + struct.pack("<H", length) + bytes([cmd]) + payload
    packet = b"\x55" + nonce + frame
    return packet, nonce


def get_firmware(sock: socket.socket) -> int | None:
    """Запрашивает версию прошивки через Опкод 0x04."""
    packet, expected_nonce = make_packet(CMD_FIRMWARE)
    sock.sendto(packet, TARGET)
    try:
        data, _ = sock.recvfrom(1024)
        # Проверяем синхробайт 0x55, сигнатуру DE AD и опкод 0x04
        if len(data) >= 18 and data[0] == 0x55 and data[9:11] == b"\xDE\xAD" and data[13] == CMD_FIRMWARE:
            fw_version = struct.unpack("<I", data[14:18])[0]
            return fw_version
    except socket.timeout:
        pass
    return None


def get_device_kinds(sock: socket.socket) -> int | None:
    """Запрашивает маску устройств через Опкод 0x02 (1 = Мышь, 2 = Клавиатура, 3 = Мышь+Клавиатура)."""
    packet, expected_nonce = make_packet(CMD_DEVICE)
    sock.sendto(packet, TARGET)
    try:
        data, _ = sock.recvfrom(1024)
        if len(data) >= 15 and data[0] == 0x55 and data[9:11] == b"\xDE\xAD" and data[13] == CMD_DEVICE:
            return data[14]
    except socket.timeout:
        pass
    return None


def move_mouse(sock: socket.socket, dx: int, dy: int):
    """Отправляет сдвиг мыши через бинарный Опкод 0x18 (SET MOVE)."""
    payload = struct.pack("<hh", int(dx), int(dy))
    packet, _ = make_packet(CMD_MOVE, payload)
    sock.sendto(packet, TARGET)


def main():
    print("=" * 60)
    print(" MAKCU BASICS: ПРЯМОЙ UDP ТЕСТ БЕЗ ОБЕРТОК")
    print("=" * 60)

    # Создаем стандартный UDP сокет
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.0)

    # 1. Запрос версии прошивки
    print(f"[*] Отправка запроса FIRMWARE_VERSION (Опкод 0x04) на {BOARD_IP}:{BOARD_PORT}...")
    fw = get_firmware(sock)
    if fw:
        print(f"[+] Ответ получен: Прошивка MAKCU = V{fw}")
    else:
        print("[-] Ошибка: плата не ответила на опкод 0x04.")
        return

    # 2. Запрос поддерживаемых устройств
    kinds = get_device_kinds(sock)
    kind_desc = []
    if kinds:
        if kinds & 0x01: kind_desc.append("Мышь")
        if kinds & 0x02: kind_desc.append("Клавиатура")
        print(f"[+] Поддерживаемые устройства (Опкод 0x02): {', '.join(kind_desc)} (Маска: 0x{kinds:02X})")

    # 3. Тестовое движение: шаг вправо и влево
    print("[*] Тест движения: +100 вправо через Опкод 0x18...")
    move_mouse(sock, 100, 0)
    time.sleep(0.5)

    print("[*] Тест движения: -100 влево через Опкод 0x18...")
    move_mouse(sock, -100, 0)
    time.sleep(0.5)

    print("[+] Все тесты успешно выполнены. Сокет закрыт.")
    sock.close()


if __name__ == "__main__":
    main()

