from crosspoint_emulator.hardware import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure root-directory enumeration independently of File Browser rendering."""
    device.mark("directory-benchmark")
    device.benchmark_directory(iterations=10)
