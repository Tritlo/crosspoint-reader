from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure repeated settled selector paints in the root File Browser."""
    device.select_files_on_home()
    device.sleep(1.5)
    device.press("confirm", hold_ms=100, settle_ms=100)
    device.sleep(1)

    device.mark("file-browser-controls-start")
    for _ in range(6):
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(0.8)
        device.press("up", hold_ms=100, settle_ms=100)
        device.sleep(0.8)
    device.mark("file-browser-controls-ready")
