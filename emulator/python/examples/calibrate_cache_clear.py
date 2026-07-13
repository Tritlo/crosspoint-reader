from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Measure one large-EPUB cache removal away from Home background work."""
    path = "/stormlight.epub"
    device.select_files_on_home()
    device.sleep(3)
    device.press("confirm", hold_ms=100, settle_ms=2000)
    device.mark("cache-clear-start")
    device.clear_epub_cache(path)
    device.mark("cache-clear-ready")
