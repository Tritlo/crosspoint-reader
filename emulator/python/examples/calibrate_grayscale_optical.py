from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Capture repeated custom-LUT grayscale transitions after fast and half primaries."""
    device.sleep(1)
    for primary in ("fast", "half"):
        device.mark(f"grayscale-optical-{primary}")
        device.benchmark_grayscale(primary, iterations=10)
        device.sleep(1)
