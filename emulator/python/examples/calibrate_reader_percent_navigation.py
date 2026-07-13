from typing import Literal

from crosspoint_emulator import HardwareDevice


def wait_for_paint(device: HardwareDevice, marker_host_time_ns: int) -> None:
    device.wait_for_log("from clearScreen to displayBuffer", after_host_time_ns=marker_host_time_ns, timeout_s=60)
    device.wait_for_log("Wait complete: refresh", after_host_time_ns=marker_host_time_ns, timeout_s=60)
    device.sleep(0.2)


def move_and_wait(
    device: HardwareDevice,
    control: Literal["left", "right", "up", "down"],
    marker: str,
) -> None:
    started = device.mark(marker)
    device.press(control, hold_ms=100, settle_ms=100)
    wait_for_paint(device, started.host_time_ns)


def run(device: HardwareDevice) -> None:
    """Exercise fine/coarse percent controls, cancel at the original value, then return Home."""
    path = "/stormlight.epub"
    device.press("back", hold_ms=100, settle_ms=500)

    opened = device.mark("reader-percent-navigation-open")
    device.open_epub(path)
    device.wait_for_log("Entering activity: EpubReader", after_host_time_ns=opened.host_time_ns, timeout_s=60)
    device.wait_for_log("Rendered page in", after_host_time_ns=opened.host_time_ns, timeout_s=120)
    device.sleep(2)

    menu = device.mark("reader-percent-menu-open")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.wait_for_log("Entering activity: EpubReaderMenu", after_host_time_ns=menu.host_time_ns, timeout_s=60)
    wait_for_paint(device, menu.host_time_ns)

    for index in range(4):
        move_and_wait(device, "down", f"reader-percent-menu-down-{index:02d}")

    percent = device.mark("reader-percent-open")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.wait_for_log(
        "Entering activity: EpubReaderPercentSelection",
        after_host_time_ns=percent.host_time_ns,
        timeout_s=60,
    )
    wait_for_paint(device, percent.host_time_ns)
    device.mark("reader-percent-ready")

    for index in range(3):
        move_and_wait(device, "right", f"reader-percent-right-{index:02d}")
        move_and_wait(device, "left", f"reader-percent-left-{index:02d}")
    for index in range(3):
        move_and_wait(device, "up", f"reader-percent-up-{index:02d}")
        move_and_wait(device, "down", f"reader-percent-down-{index:02d}")
    device.mark("reader-percent-controls-ready")

    reader = device.mark("reader-percent-exit")
    device.press("back", hold_ms=100, settle_ms=100)
    wait_for_paint(device, reader.host_time_ns)

    home = device.mark("reader-percent-navigation-exit")
    device.press("back", hold_ms=100, settle_ms=100)
    device.wait_for_log("Entering activity: Home", after_host_time_ns=home.host_time_ns, timeout_s=60)
    wait_for_paint(device, home.host_time_ns)
    device.mark("reader-percent-home-ready")
