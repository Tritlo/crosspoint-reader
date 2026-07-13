from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Capture natural fast-primary text page waveforms from cached Stormlight at the 15-page baseline."""
    device.press("back", hold_ms=100, settle_ms=1500)
    device.mark("fast-open")
    device.open_epub("/stormlight.epub")
    device.mark("fast-ready")
    device.sleep(1)
    for index in range(12):
        device.mark(f"fast-page-{index:02d}")
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(4)
    device.mark("fast-pages-ready")
