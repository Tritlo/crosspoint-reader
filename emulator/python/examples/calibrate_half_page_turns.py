from typing import Literal

from crosspoint_emulator import HardwareDevice


def press_many(device: HardwareDevice, control: Literal["up", "down"], count: int) -> None:
    for _ in range(count):
        device.press(control, hold_ms=100, settle_ms=1000)


def set_refresh_frequency(device: HardwareDevice, *, from_index: int, to_index: int) -> None:
    device.select_settings_on_home()
    device.sleep(3)
    device.press("confirm", hold_ms=100, settle_ms=2000)
    press_many(device, "down", 6)
    device.press("confirm", hold_ms=100, settle_ms=1500)
    direction: Literal["up", "down"] = "up" if to_index < from_index else "down"
    press_many(device, direction, abs(to_index - from_index))
    device.press("confirm", hold_ms=100, settle_ms=2000)
    device.press("back", hold_ms=100, settle_ms=1000)
    device.press("back", hold_ms=100, settle_ms=2500)


def run(device: HardwareDevice) -> None:
    """Measure cached half-primary page renders, then restore the 15-page cadence."""
    path = "/stormlight.epub"
    set_refresh_frequency(device, from_index=3, to_index=0)
    device.mark("half-open")
    device.open_epub(path)
    device.mark("half-ready")
    for index in range(10):
        device.mark(f"half-page-{index:02d}")
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(3.5)
    device.mark("half-pages-ready")
    set_refresh_frequency(device, from_index=0, to_index=3)
    device.mark("refresh-15-restored")
