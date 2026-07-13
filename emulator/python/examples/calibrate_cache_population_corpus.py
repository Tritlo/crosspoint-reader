from crosspoint_emulator.hardware import HardwareDevice


BOOKS = (
    ("wot", "/wot.epub"),
    ("sun-eater", "/sun-eater.epub"),
    ("mistborn", "/mistborn.epub"),
    ("lotr", "/lotr.epub"),
)


def run(device: HardwareDevice) -> None:
    """Measure existing representative cache populations without rebuilding them."""
    device.select_files_on_home()
    device.sleep(3)
    device.press("confirm", hold_ms=100, settle_ms=2000)
    for name, path in BOOKS:
        device.mark(f"cache-clear-{name}")
        device.clear_epub_cache(path)
