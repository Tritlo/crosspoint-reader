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
    """Force every-page cleanup, capture the image FAST override, then restore 15 pages."""
    path = "/stormlight.epub"
    device.press("back", hold_ms=100, settle_ms=2500)
    set_refresh_frequency(device, from_index=3, to_index=0)

    device.mark("half-image-cache-clear")
    device.clear_epub_cache(path)
    opened = device.open_epub(path)
    device.wait_for_log("Entering activity: EpubReader", after_host_time_ns=opened.host_time_ns)
    device.wait_for_log("Rendered page in", after_host_time_ns=opened.host_time_ns)
    device.mark("half-image-ready")

    for index in range(8):
        marker = device.mark(f"half-image-page-{index:02d}")
        device.press("down", hold_ms=100, settle_ms=100)
        device.wait_for_log("Rendered page in", after_host_time_ns=marker.host_time_ns)
        device.sleep(1)

    device.mark("half-image-pages-ready")
    leaving = device.mark("half-image-leave-reader")
    device.press("back", hold_ms=100, settle_ms=100)
    device.wait_for_log("Entering activity: Home", after_host_time_ns=leaving.host_time_ns, timeout_s=60)
    device.sleep(2)
    set_refresh_frequency(device, from_index=0, to_index=3)
    device.mark("refresh-15-restored")
