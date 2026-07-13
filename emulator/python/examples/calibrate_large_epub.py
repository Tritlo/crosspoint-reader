from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Cold-index and reopen the image-heavy 46 MB Stormlight fixture."""
    path = "/stormlight.epub"
    device.press("back", hold_ms=100, settle_ms=1500)
    device.mark("cold-cache-clear")
    device.clear_epub_cache(path)
    device.mark("cold-open")
    device.open_epub(path)
    device.mark("cold-ready")
    device.sleep(1)
    for index in range(12):
        device.mark(f"cold-page-{index:02d}")
        device.press("down", hold_ms=100, settle_ms=100)
        device.sleep(2)
    device.mark("cold-pages-ready")
    device.press("back", hold_ms=100, settle_ms=2500)
    device.mark("home-ready")
    device.mark("warm-open")
    device.open_epub(path)
    device.mark("warm-ready")
    device.sleep(2)
