from crosspoint_emulator import HardwareDevice


BOOKS = (
    ("wot", "/wot.epub"),
    ("sun-eater", "/sun-eater.epub"),
    ("mistborn", "/mistborn.epub"),
    ("lotr", "/lotr.epub"),
)
WARM_REPEATS = 3


def run(device: HardwareDevice) -> None:
    """Cold-open representative workloads, then sample quiescent warm reopens."""
    for name, path in BOOKS:
        device.select_files_on_home()
        device.sleep(3)
        device.press("confirm", hold_ms=100, settle_ms=2000)

        device.mark(f"{name}-cache-clear")
        device.clear_epub_cache(path)
        device.mark(f"{name}-cold-open")
        device.open_epub(path)
        device.mark(f"{name}-cold-ready")

        device.sleep(1)
        device.mark(f"{name}-page-00")
        device.press("down", hold_ms=100, settle_ms=2500)
        device.mark(f"{name}-page-ready")

        home_start = device.mark(f"{name}-home")
        device.press("back", hold_ms=100, settle_ms=2500)
        device.wait_for_log(
            "Generated thumb BMP from JPG cover image, success: yes",
            after_host_time_ns=home_start.host_time_ns,
            timeout_s=300,
        )
        device.sleep(2)
        device.mark(f"{name}-home-ready")

        for index in range(WARM_REPEATS):
            warm_start = device.mark(f"{name}-warm-open-{index:02d}")
            device.open_epub(path)
            device.mark(f"{name}-warm-ready-{index:02d}")
            device.wait_for_log(
                "[DBG] [ERS] Progress saved:",
                after_host_time_ns=warm_start.host_time_ns,
                timeout_s=120,
            )
            device.sleep(1)
            if index + 1 < WARM_REPEATS:
                device.press("back", hold_ms=100, settle_ms=2500)
                device.sleep(5)
