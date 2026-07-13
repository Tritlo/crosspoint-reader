from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure repeated Home selector controls with each panel update settled."""
    device.select_files_on_home()
    device.sleep(1.5)
    device.mark("home-controls-start")
    for _ in range(6):
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(0.8)

        device.press("up", hold_ms=100, settle_ms=100)
        device.sleep(0.8)
    device.mark("home-controls-ready")
