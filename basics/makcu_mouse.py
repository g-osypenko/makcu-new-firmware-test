"""
MAKCU Mouse Controller — High-Performance UDP Driver powered by mak-suite.

Replaces the legacy serial implementation (mouse_example.py) with official 
mak-suite UDP RAW transport while preserving 100% API compatibility.
"""

from __future__ import annotations

import math
import random
import sys
import threading
import time
from pathlib import Path

# Add mak-suite to sys.path
_ROOT = Path(__file__).resolve().parents[1]
_MAK_SUITE_PATH = _ROOT / "mak-suite" / "python"
if str(_MAK_SUITE_PATH) not in sys.path:
    sys.path.insert(0, str(_MAK_SUITE_PATH))

from makxd import ConnectionConfig, MakxdController, UdpWireMode
from makxd.enums import MouseButton
from makxd.stream import StreamKind

# Default Board Configuration
BOARD_IP = "192.168.50.175"
BOARD_PORT = 8080

# Button mapping: 0=Left, 1=Right, 2=Middle, 3=Side1 (S4), 4=Side2 (S5)
INDEX_TO_BUTTON = {
    0: MouseButton.LEFT,
    1: MouseButton.RIGHT,
    2: MouseButton.MIDDLE,
    3: MouseButton.MOUSE4,
    4: MouseButton.MOUSE5,
}

_global_mouse_instance: Mouse | None = None
_mask_applied_idx: int | None = None


# ---------------------------------------------------------------------------
# Smooth WindMouse Trajectory Generator (Adaptive Braking & Exact Closure)
# ---------------------------------------------------------------------------
def generate_smooth_windmouse_path(
    target_dx: float,
    target_dy: float,
    max_step: float = 8.0,
    gravity: float = 6.0,
    wind: float = 0.8,
) -> list[tuple[int, int, float]]:
    """
    Generates human-like WindMouse path with adaptive terminal damping
    to guarantee zero overshoot and exact integer closure.
    """
    current_x, current_y = 0.0, 0.0
    dest_x, dest_y = float(target_dx), float(target_dy)
    vel_x, vel_y = 0.0, 0.0
    wind_x, wind_y = 0.0, 0.0

    path: list[tuple[int, int, float]] = []
    carry_x, carry_y = 0.0, 0.0
    target_area = 2.0

    # Max 150 iterations to prevent runaway
    for _ in range(150):
        dist = math.hypot(dest_x - current_x, dest_y - current_y)
        if dist < target_area:
            break

        # Wind fluctuation
        wind_x = wind_x / math.sqrt(3) + (random.random() - 0.5) * wind * 2
        wind_y = wind_y / math.sqrt(3) + (random.random() - 0.5) * wind * 2

        # Adaptive gravity & step limit: decelerate smoothly as distance drops
        curr_max_step = min(max_step, max(1.5, dist * 0.25))
        grav_scale = gravity if dist > 15.0 else (gravity * dist / 15.0)

        grav_x = grav_scale * (dest_x - current_x) / dist
        grav_y = grav_scale * (dest_y - current_y) / dist

        # Apply friction
        vel_x = (vel_x + wind_x + grav_x) * 0.92
        vel_y = (vel_y + wind_y + grav_y) * 0.92

        # Clamp speed
        spd = math.hypot(vel_x, vel_y)
        if spd > curr_max_step:
            scale = curr_max_step / spd
            vel_x *= scale
            vel_y *= scale

        next_x = current_x + vel_x
        next_y = current_y + vel_y

        carry_x += (next_x - current_x)
        carry_y += (next_y - current_y)

        out_dx = int(round(carry_x))
        out_dy = int(round(carry_y))
        carry_x -= out_dx
        carry_y -= out_dy

        if out_dx != 0 or out_dy != 0:
            delay = random.uniform(0.002, 0.005)
            path.append((out_dx, out_dy, delay))

        current_x, current_y = next_x, next_y

    # Terminal correction: apply exact remainder to guarantee sum(dx) == target_dx
    rem_x = int(round(target_dx)) - sum(p[0] for p in path)
    rem_y = int(round(target_dy)) - sum(p[1] for p in path)
    if rem_x != 0 or rem_y != 0:
        path.append((rem_x, rem_y, 0.003))

    return path


# ---------------------------------------------------------------------------
# Mouse Singleton (mak-suite UDP Transport)
# ---------------------------------------------------------------------------
class Mouse:
    _instance: Mouse | None = None

    def __new__(cls, *args, **kwargs) -> Mouse:
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __init__(self, host: str = BOARD_IP, port: int = BOARD_PORT) -> None:
        if getattr(self, "_inited", False):
            return

        self.host = host
        self.port = port
        self.button_states: dict[int, bool] = {i: False for i in range(5)}
        self._button_lock = threading.Lock()
        self.is_connected = False

        print(f"[INFO] Connecting to MAKCU via mak-suite UDP ({self.host}:{self.port})...")
        try:
            config = ConnectionConfig.udp(
                host=self.host,
                port=self.port,
                mode=UdpWireMode.RAW,
            )
            self.controller = MakxdController(connection=config)
            self.controller.connect()
            self.is_connected = True

            # Subscribe to real-time physical button input stream
            try:
                self.controller.set_input_callback(self._on_input_event)
                self.controller.input_stream(StreamKind.MOUSE, enabled=True)
            except Exception as stream_err:
                print(f"[WARN] Input streaming setup: {stream_err}")

            print(f"[INFO] Successfully connected to MAKCU via UDP!")
        except Exception as e:
            print(f"[ERROR] Failed to connect to MAKCU via UDP: {e}")
            self.controller = None
            self.is_connected = False

        self._inited = True
        global _global_mouse_instance
        _global_mouse_instance = self

    def _on_input_event(self, change) -> None:
        """Callback from mak-suite background thread when physical button changes."""
        if getattr(change, "kind", None) == StreamKind.MOUSE:
            control = getattr(change, "control", None)
            value = getattr(change, "value", None)
            if control in self.button_states and value is not None:
                with self._button_lock:
                    self.button_states[control] = bool(value)

    def is_button_pressed(self, idx: int) -> bool:
        """Check if physical mouse button (0..4) is pressed."""
        with self._button_lock:
            return self.button_states.get(idx, False)

    def move(self, x: float, y: float) -> None:
        """Instant relative mouse move via Opcode 0x18."""
        if not self.is_connected or self.controller is None:
            return
        dx, dy = int(round(x)), int(round(y))
        if dx != 0 or dy != 0:
            self.controller.mouse.move(dx, dy)

    def move_smooth(
        self,
        dx: float,
        dy: float,
        max_step: float = 8.0,
        gravity: float = 6.0,
        wind: float = 0.8,
    ) -> None:
        """
        Smooth WindMouse relative movement with exact closure and no overshoot.
        """
        if not self.is_connected or self.controller is None:
            return
        path = generate_smooth_windmouse_path(
            dx, dy, max_step=max_step, gravity=gravity, wind=wind
        )
        for step_x, step_y, delay in path:
            self.controller.mouse.move(step_x, step_y)
            target_t = time.perf_counter() + delay
            while time.perf_counter() < target_t:
                pass

    def click(self, button: MouseButton = MouseButton.LEFT) -> None:
        """Click mouse button."""
        if not self.is_connected or self.controller is None:
            return
        self.controller.mouse.click(button)

    def lock_button(self, idx: int) -> None:
        """Hardware lock/mask a button by index (0..4)."""
        if not self.is_connected or self.controller is None:
            return
        btn = INDEX_TO_BUTTON.get(idx)
        if btn is not None:
            self.controller.mouse.button_mask(btn, True)

    def unlock_button(self, idx: int) -> None:
        """Unlock/unmask a button by index (0..4)."""
        if not self.is_connected or self.controller is None:
            return
        btn = INDEX_TO_BUTTON.get(idx)
        if btn is not None:
            self.controller.mouse.button_mask(btn, False)

    def unlock_all(self) -> None:
        """Release all hardware button locks."""
        if not self.is_connected or self.controller is None:
            return
        for btn in INDEX_TO_BUTTON.values():
            try:
                self.controller.mouse.button_mask(btn, False)
            except Exception:
                pass

    @classmethod
    def mask_manager_tick(cls, selected_idx: int | None, aimbot_running: bool) -> None:
        """Manage button masks according to aimbot state (1:1 compatible with mouse_example.py)."""
        global _mask_applied_idx
        mouse = cls()
        if not mouse.is_connected:
            _mask_applied_idx = None
            return

        if not isinstance(selected_idx, int) or not (0 <= selected_idx <= 4):
            selected_idx = None

        if not aimbot_running:
            if _mask_applied_idx is not None:
                mouse.unlock_button(_mask_applied_idx)
                _mask_applied_idx = None
            return

        if selected_idx is None:
            if _mask_applied_idx is not None:
                mouse.unlock_button(_mask_applied_idx)
                _mask_applied_idx = None
            return

        if _mask_applied_idx != selected_idx:
            if _mask_applied_idx is not None:
                mouse.unlock_button(_mask_applied_idx)
            mouse.lock_button(selected_idx)
            _mask_applied_idx = selected_idx

    @classmethod
    def cleanup(cls) -> None:
        """Clean up controller and disconnect."""
        global _mask_applied_idx
        mouse = cls()
        if mouse.is_connected and mouse.controller is not None:
            try:
                mouse.unlock_all()
            except Exception:
                pass
            try:
                mouse.controller.disconnect()
            except Exception:
                pass
        mouse.is_connected = False
        _mask_applied_idx = None
        cls._instance = None
        print("[INFO] MAKCU UDP Controller cleaned up.")


# ---------------------------------------------------------------------------
# Global Helper Functions (Direct 1:1 replacement for mouse_example.py)
# ---------------------------------------------------------------------------
def is_button_pressed(idx: int) -> bool:
    return Mouse().is_button_pressed(idx)

def lock_button_idx(idx: int) -> None:
    Mouse().lock_button(idx)

def unlock_button_idx(idx: int) -> None:
    Mouse().unlock_button(idx)

def unlock_all_locks() -> None:
    Mouse().unlock_all()

def mask_manager_tick(selected_idx: int | None, aimbot_running: bool) -> None:
    Mouse.mask_manager_tick(selected_idx, aimbot_running)

def test_move() -> None:
    Mouse().move(100, 100)


# ---------------------------------------------------------------------------
# Interactive Verification Menu
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    mouse = Mouse()
    if not mouse.is_connected:
        print("[FATAL] Could not connect to MAKCU. Exiting.")
        sys.exit(1)

    try:
        while True:
            print("\n" + "=" * 60)
            print(" MAKCU UDP DRIVER (mak-suite + WindMouse)")
            print("=" * 60)
            print(" [1] Прямой сдвиг +100 px (Опкод 0x18 мгновенно)")
            print(" [2] Плавный WindMouse сдвиг (+300, 0) с точным финишем")
            print(" [3] Плавный WindMouse сдвиг (-300, 0) обратно")
            print(" [4] Тестовый клик ЛКМ")
            print(" [5] Мониторинг кнопок мыши в реальном времени (3 сек)")
            print(" [0] Выход")
            choice = input("Выбор: ").strip()

            if choice == "1":
                print("[*] Отправка +100 px...")
                mouse.move(100, 0)
            elif choice == "2":
                print("[*] Выполнение плавного WindMouse (+300 px)...")
                mouse.move_smooth(300, 0)
                print("[+] Готово!")
            elif choice == "3":
                print("[*] Выполнение плавного WindMouse (-300 px)...")
                mouse.move_smooth(-300, 0)
                print("[+] Готово!")
            elif choice == "4":
                print("[*] Клик...")
                mouse.click()
            elif choice == "5":
                print("[*] Нажимай кнопки мыши (ЛКМ, ПКМ, СКМ, боковые)...")
                t_end = time.time() + 3.0
                while time.time() < t_end:
                    states = [mouse.is_button_pressed(i) for i in range(5)]
                    sys.stdout.write(f"\rКнопки [L={states[0]}, R={states[1]}, M={states[2]}, S1={states[3]}, S2={states[4]}]")
                    sys.stdout.flush()
                    time.sleep(0.05)
                print("\n[*] Завершено.")
            elif choice == "0":
                break
    finally:
        Mouse.cleanup()

