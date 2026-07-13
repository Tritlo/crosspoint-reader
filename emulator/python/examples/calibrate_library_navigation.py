from crosspoint_emulator.hardware import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure repeated root File Browser opens over the multi-book calibration SD."""
    device.select_files_on_home()
    device.sleep(3)
    for index in range(5):
        device.mark(f"library-open-{index:02d}")
        device.press("confirm", hold_ms=100, settle_ms=100)
        device.sleep(4)
        device.mark(f"library-ready-{index:02d}")
        device.press("back", hold_ms=100, settle_ms=100)
        device.sleep(4)
    device.mark("library-navigation-ready")
