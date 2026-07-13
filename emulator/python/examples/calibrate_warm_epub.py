from crosspoint_emulator.hardware import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure a second warm open after all derived work has quiesced."""
    path = "/stormlight.epub"
    device.open_epub(path)
    device.sleep(15)
    device.press("back", hold_ms=100, settle_ms=2500)
    device.sleep(10)
    device.mark("warm-quiescent-open")
    device.open_epub(path)
    device.mark("warm-quiescent-ready")
    device.sleep(5)
