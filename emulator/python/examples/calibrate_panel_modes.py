from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure repeated black/white transitions in every supported mode."""
    device.sleep(1)
    for mode in ("fast", "half", "full"):
        device.mark(f"panel-{mode}")
        device.benchmark_panel(mode, iterations=10)
        device.sleep(1)
