from crosspoint_emulator.hardware import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Alternate thirty page turns with the current EPUB already open."""
    device.sleep(1)
    for index in range(30):
        control = "down" if index % 2 == 0 else "up"
        device.mark(f"page-{index:02d}-{control}")
        device.press(control, hold_ms=100, settle_ms=100)
        device.sleep(1.8)
