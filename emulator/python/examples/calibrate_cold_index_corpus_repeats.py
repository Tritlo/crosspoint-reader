from crosspoint_emulator import HardwareDevice


BOOKS = (
    ("wot", "/wot.epub"),
    ("sun-eater", "/sun-eater.epub"),
    ("mistborn", "/mistborn.epub"),
    ("lotr", "/lotr.epub"),
)
REPEATS = 2


def run(device: HardwareDevice) -> None:
    """Repeat cold indexing for the four non-Stormlight corpus books."""
    device.press("back", hold_ms=100, settle_ms=2500)
    for name, path in BOOKS:
        for index in range(REPEATS):
            sample = f"{name}-index-{index:02d}"
            device.select_files_on_home()
            device.sleep(1)
            device.press("confirm", hold_ms=100, settle_ms=2000)
            device.mark(f"{sample}-cache-clear")
            device.clear_epub_cache(path)
            started = device.mark(f"{sample}-cold-open")
            device.open_epub(path)
            device.mark(f"{sample}-cold-ready")
            device.wait_for_log(
                "[DBG] [ERS] Progress saved:",
                after_host_time_ns=started.host_time_ns,
                timeout_s=180,
            )
            device.sleep(1)
            device.press("back", hold_ms=100, settle_ms=3000)
