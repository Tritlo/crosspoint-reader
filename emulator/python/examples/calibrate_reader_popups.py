from typing import Literal

from crosspoint_emulator import HardwareDevice


def wait_for_render(device: HardwareDevice, marker_host_time_ns: int) -> None:
    device.wait_for_log("CAL:RENDER:EpubReaderMenu:", after_host_time_ns=marker_host_time_ns, timeout_s=60)
    device.wait_for_log("Wait complete: refresh", after_host_time_ns=marker_host_time_ns, timeout_s=60)
    device.sleep(0.2)


def move_and_wait(
    device: HardwareDevice,
    control: Literal["up", "down"],
    marker: str,
) -> None:
    started = device.mark(marker)
    device.press(control, hold_ms=100, settle_ms=100)
    wait_for_render(device, started.host_time_ns)


def open_popup(device: HardwareDevice, marker: str) -> None:
    started = device.mark(marker)
    device.press("confirm", hold_ms=100, settle_ms=100)
    wait_for_render(device, started.host_time_ns)


def cancel_popup_to_reader(device: HardwareDevice, marker: str) -> None:
    started = device.mark(marker)
    device.press("back", hold_ms=100, settle_ms=100)
    device.wait_for_log(
        "CAL:RENDER:EpubReaderMenu:", after_host_time_ns=started.host_time_ns, timeout_s=60
    )
    device.wait_for_log(
        "Exiting activity: EpubReaderMenu", after_host_time_ns=started.host_time_ns, timeout_s=60
    )
    device.wait_for_log("Rendered page in", after_host_time_ns=started.host_time_ns, timeout_s=120)
    device.sleep(0.2)


def open_menu(device: HardwareDevice, marker: str) -> None:
    started = device.mark(marker)
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.wait_for_log("Entering activity: EpubReaderMenu", after_host_time_ns=started.host_time_ns, timeout_s=60)
    wait_for_render(device, started.host_time_ns)


def run(device: HardwareDevice) -> None:
    """Exercise and cancel both fixed Reader enum popups without changing their values."""
    device.press("back", hold_ms=100, settle_ms=500)

    opened = device.mark("reader-popups-navigation-open")
    device.open_epub("/stormlight.epub")
    device.wait_for_log("Entering activity: EpubReader", after_host_time_ns=opened.host_time_ns, timeout_s=60)
    device.wait_for_log("Rendered page in", after_host_time_ns=opened.host_time_ns, timeout_s=120)
    device.sleep(2)

    open_menu(device, "reader-popups-menu-open")

    for index in range(2):
        move_and_wait(device, "down", f"reader-popups-menu-down-{index:02d}")

    open_popup(device, "reader-orientation-popup-open")
    for index in range(3):
        move_and_wait(device, "down", f"reader-orientation-popup-down-{index:02d}")
        move_and_wait(device, "up", f"reader-orientation-popup-up-{index:02d}")
    cancel_popup_to_reader(device, "reader-orientation-popup-cancel")

    open_menu(device, "reader-popups-menu-reopen")
    for index in range(3):
        move_and_wait(device, "down", f"reader-popups-auto-turn-row-{index:02d}")
    open_popup(device, "reader-auto-turn-popup-open")
    for index in range(3):
        move_and_wait(device, "down", f"reader-auto-turn-popup-down-{index:02d}")
        move_and_wait(device, "up", f"reader-auto-turn-popup-up-{index:02d}")
    cancel_popup_to_reader(device, "reader-auto-turn-popup-cancel")

    home = device.mark("reader-popups-navigation-exit")
    device.press("back", hold_ms=100, settle_ms=100)
    device.wait_for_log("Entering activity: Home", after_host_time_ns=home.host_time_ns, timeout_s=60)
    device.wait_for_log("Wait complete: refresh", after_host_time_ns=home.host_time_ns, timeout_s=60)
    device.mark("reader-popups-home-ready")
