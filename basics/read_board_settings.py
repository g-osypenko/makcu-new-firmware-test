#!/usr/bin/env python3
"""
ОСНОВЫ MAKCU: Чтение и вывод всех сохраненных аппаратных настроек из памяти платы (NOR Flash).
Показывает размер буфера, фильтр слияния mouse_spread, кривые, deadzones и эмулируемые дескрипторы.
"""

from makxd.connection_config import ConnectionConfig, ConnectionMethod, UdpWireMode
import makxd
from makxd.settings import DeviceConfiguration, SettingsSection

BOARD_IP = "192.168.50.175"
BOARD_PORT = 8080


def main():
    print("=" * 65)
    print(" MAKCU BASICS: ПОЛНЫЙ СЛЕПОК НАСТРОЕК ПЛАТЫ (NOR FLASH)")
    print("=" * 65)

    conn = ConnectionConfig.udp(BOARD_IP, port=BOARD_PORT, mode=UdpWireMode.RAW)
    controller = makxd.create_controller(connection=conn)

    try:
        cfg = DeviceConfiguration(controller.transport)
        info = cfg.info()
        snap = cfg.read()
        s = snap.settings

        print(f"[*] Ревизия настроек в NOR Flash: {info.revision}")
        print(f"[*] Поддерживаемые секции платы: {info.sections} (4 = MOUSE)")
        print(f"[*] Статус сохранения (save_state): {info.save_state} (0 = синхронизировано)")
        print("-" * 65)
        print(" АППАРАТНЫЕ ПАРАМЕТРЫ МЫШИ (NOR FLASH):")
        print(f"  • Аппаратный фильтр mouse_spread: {s.mouse_spread_percent}% (0% = сплайн отключен, прямая выдача)")
        print("-" * 65)
        print(" ПАРАМЕТРЫ КОНТРОЛЛЕРА/ГЕЙМПАДА (НЕ ПРИМЕНЯЮТСЯ К МЫШИ):")
        print("  [!] ВНИМАНИЕ: Секция CONTROLLER не поддерживается прошивкой мыши (status 5).")
        print(f"  • buffer_ms (заглушка SDK):       {s.controller.buffer_ms} мс")
        print(f"  • interpolation (заглушка SDK):   {s.controller.interpolation}")
        print(f"  • timing_variance (заглушка SDK): {s.controller.timing_variance_percent}%")
        print("-" * 65)
        print(" ПОВЕДЕНИЕ (BEHAVIORS / ПРОФИЛЬ 0):")
        beh = s.controller.behaviors[0]
        if beh:
            print(f"  • Имя профиля:                   {beh.name}")
            print(f"  • Сила кривой:                   {beh.strength_percent}")
            print(f"  • Инерция сенсора:               {beh.inertia_percent}%")
            print(f"  • Микро-сглаживание:             {beh.micro_percent}%")
            print(f"  • Мертвые зоны:                  Центр={beh.curves[0].center_deadzone_percent}%, Анти-зона={beh.curves[0].anti_deadzone_percent}%")
        print("-" * 65)
        print(" ТРАНСЛЯЦИЯ (ГЕЙМПАД/WASD):")
        for i, tr in enumerate(s.translation):
            channel_names = ["Левый стик (WASD)", "Правый стик (Мышь)", "Левый триггер", "Правый триггер"]
            name = channel_names[i] if i < len(channel_names) else f"Канал {i}"
            print(f"  • {name:20s}: Включен={tr.enabled}, Масштаб={tr.scale}, Таймаут={tr.timeout_ms}мс")

    finally:
        controller.disconnect()
        print("=" * 65)


if __name__ == "__main__":
    main()

