from typing import Literal

from crosspoint_emulator import HardwareDevice


def wait_for_paint(device: HardwareDevice, marker_host_time_ns: int) -> None:
    device.wait_for_log("from clearScreen to displayBuffer", after_host_time_ns=marker_host_time_ns, timeout_s=60)
    device.sleep(1)


def move_and_wait(device: HardwareDevice, control: Literal["up", "down"], marker: str) -> None:
    started = device.mark(marker)
    device.press(control, hold_ms=100, settle_ms=100)
    wait_for_paint(device, started.host_time_ns)


def run(device: HardwareDevice) -> None:
    """Exercise Reader menu and chapter-list paints, then return to Home unchanged."""
    path = "/stormlight.epub"
    device.press("back", hold_ms=100, settle_ms=500)

    opened = device.mark("reader-navigation-open")
    device.open_epub(path)
    device.wait_for_log("Entering activity: EpubReader", after_host_time_ns=opened.host_time_ns, timeout_s=60)
    device.wait_for_log("Rendered page in", after_host_time_ns=opened.host_time_ns, timeout_s=120)
    device.sleep(2)
    device.mark("reader-navigation-reader-ready")

    menu = device.mark("reader-menu-open")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.wait_for_log("Entering activity: EpubReaderMenu", after_host_time_ns=menu.host_time_ns, timeout_s=60)
    wait_for_paint(device, menu.host_time_ns)
    device.mark("reader-menu-ready")

    for index in range(3):
        move_and_wait(device, "down", f"reader-menu-down-{index:02d}")
        move_and_wait(device, "up", f"reader-menu-up-{index:02d}")
    device.mark("reader-menu-controls-ready")

    chapters = device.mark("reader-chapters-open")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.wait_for_log(
        "Entering activity: EpubReaderChapterSelection",
        after_host_time_ns=chapters.host_time_ns,
        timeout_s=60,
    )
    wait_for_paint(device, chapters.host_time_ns)
    device.mark("reader-chapters-ready")

    for index in range(3):
        move_and_wait(device, "down", f"reader-chapters-down-{index:02d}")
        move_and_wait(device, "up", f"reader-chapters-up-{index:02d}")
    device.mark("reader-chapters-controls-ready")

    reader = device.mark("reader-chapters-exit")
    device.press("back", hold_ms=100, settle_ms=100)
    wait_for_paint(device, reader.host_time_ns)
    device.mark("reader-navigation-reader-restored")

    home = device.mark("reader-navigation-exit")
    device.press("back", hold_ms=100, settle_ms=100)
    device.wait_for_log("Entering activity: Home", after_host_time_ns=home.host_time_ns, timeout_s=60)
    wait_for_paint(device, home.host_time_ns)
    device.mark("reader-navigation-home-ready")
