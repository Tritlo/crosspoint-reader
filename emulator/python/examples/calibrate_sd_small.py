from crosspoint_emulator.hardware import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure complete 4 KiB and 64 KiB sequential SD I/O runs."""
    device.sleep(1)
    for byte_count in (4096, 65536):
        device.mark(f"sd-{byte_count}")
        device.benchmark_sd(byte_count, iterations=20)
