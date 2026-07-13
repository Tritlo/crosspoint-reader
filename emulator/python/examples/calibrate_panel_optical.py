from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Capture a short synchronized optical sample of each refresh mode."""
    device.sleep(1)
    for mode in ("fast", "half", "full"):
        device.mark(f"panel-optical-{mode}")
        device.benchmark_panel(mode, iterations=3)
        device.sleep(1)
