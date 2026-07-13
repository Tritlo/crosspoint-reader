from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Open Settings, inspect one enum popup without changing its value, then return Home."""
    device.select_settings_on_home()
    device.sleep(3)

    device.mark("settings-open")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.sleep(3)
    device.mark("settings-ready")

    device.mark("settings-reader-category")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.sleep(2)
    device.mark("settings-reader-ready")

    for index in range(3):
        device.mark(f"settings-row-{index:02d}")
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(2)

    device.mark("settings-popup-open")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.sleep(3)
    device.mark("settings-popup-ready")

    for index in range(3):
        device.mark(f"settings-popup-down-{index:02d}")
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(2)
        device.mark(f"settings-popup-up-{index:02d}")
        device.press("up", hold_ms=100, settle_ms=100)
        device.sleep(2)
    device.mark("settings-popup-move-ready")

    device.press("back", hold_ms=100, settle_ms=100)
    device.sleep(2)
    device.press("back", hold_ms=100, settle_ms=100)
    device.sleep(2)

    device.mark("settings-exit")
    device.press("back", hold_ms=100, settle_ms=100)
    device.sleep(3)
    device.mark("settings-home-ready")
