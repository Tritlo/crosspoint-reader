from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure 512 KiB sequential SD I/O without exceeding host timeout."""
    device.sleep(1)
    device.mark("sd-524288")
    device.benchmark_sd(524288, iterations=5)
