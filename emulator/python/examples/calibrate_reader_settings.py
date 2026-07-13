from typing import Literal

from crosspoint_emulator import HardwareDevice


def press_many(device: HardwareDevice, control: Literal["up", "down"], count: int) -> None:
    for _ in range(count):
        device.press(control, hold_ms=100, settle_ms=500)


def enter_reader_settings(device: HardwareDevice) -> None:
    device.select_settings_on_home()
    device.mark("settings-home-ready")
    device.press("confirm", hold_ms=100, settle_ms=1500)
    device.mark("settings-ready")
    device.press("confirm", hold_ms=100, settle_ms=1500)


def run(device: HardwareDevice) -> None:
    """Measure XL-font re-layout, then restore the Medium baseline through Settings."""
    path = "/stormlight.epub"
    enter_reader_settings(device)
    press_many(device, "down", 3)
    device.press("confirm", hold_ms=100, settle_ms=800)
    press_many(device, "down", 2)
    device.press("confirm", hold_ms=100, settle_ms=1200)
    device.press("back", hold_ms=100, settle_ms=500)
    device.press("back", hold_ms=100, settle_ms=2000)

    device.mark("xl-open")
    device.open_epub(path)
    device.mark("xl-ready")
    for index in range(8):
        device.mark(f"xl-page-{index:02d}")
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(2)
    device.mark("xl-pages-ready")

    enter_reader_settings(device)
    press_many(device, "down", 3)
    device.press("confirm", hold_ms=100, settle_ms=800)
    press_many(device, "up", 2)
    device.press("confirm", hold_ms=100, settle_ms=1200)
    device.press("back", hold_ms=100, settle_ms=500)
    device.press("back", hold_ms=100, settle_ms=2000)
    device.mark("medium-restored")
