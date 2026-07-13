from crosspoint_emulator import HardwareDevice


def press_down(device: HardwareDevice, count: int) -> None:
    for _ in range(count):
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(2)


def measure_popup(device: HardwareDevice, name: str) -> None:
    device.mark(f"settings-popup-variant-{name}-open")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.sleep(3)

    for index in range(3):
        device.mark(f"settings-popup-variant-{name}-down-{index:02d}")
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(2)
        device.mark(f"settings-popup-variant-{name}-up-{index:02d}")
        device.press("up", hold_ms=100, settle_ms=100)
        device.sleep(2)

    device.mark(f"settings-popup-variant-{name}-samples-ready")
    device.press("back", hold_ms=100, settle_ms=100)
    device.sleep(2)
    device.mark(f"settings-popup-variant-{name}-cancelled")


def run(device: HardwareDevice) -> None:
    """Measure differently sized Settings popups, cancelling every popup without saving."""
    device.select_settings_on_home()
    device.sleep(3)

    device.mark("settings-popup-variants-open")
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.sleep(3)
    device.mark("settings-popup-variants-ready")

    # Display: Sleep Screen (7 options), then Refresh Frequency (5 options).
    press_down(device, 1)
    measure_popup(device, "sleep-screen")
    press_down(device, 5)
    measure_popup(device, "refresh-frequency")

    # Return to the category tab and advance from Display to Reader.
    device.press("back", hold_ms=100, settle_ms=100)
    device.sleep(2)
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.sleep(2)

    # Reader: Line Spacing (3 options), then Paragraph Alignment (5 options).
    press_down(device, 4)
    measure_popup(device, "line-spacing")
    press_down(device, 2)
    measure_popup(device, "paragraph-alignment")

    device.press("back", hold_ms=100, settle_ms=100)
    device.sleep(2)
    device.press("back", hold_ms=100, settle_ms=100)
    device.sleep(3)
    device.mark("settings-popup-variants-home-ready")
