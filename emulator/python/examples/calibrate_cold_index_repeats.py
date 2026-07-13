from crosspoint_emulator.hardware import HardwareDevice


REPEATS = 3


def run(device: HardwareDevice) -> None:
    """Repeat a cold Stormlight index without overlapping the next sample."""
    path = "/stormlight.epub"
    device.press("back", hold_ms=100, settle_ms=2500)
    for index in range(REPEATS):
        name = f"stormlight-index-{index:02d}"
        device.select_files_on_home()
        device.sleep(1)
        device.press("confirm", hold_ms=100, settle_ms=2000)
        device.mark(f"{name}-cache-clear")
        device.clear_epub_cache(path)
        started = device.mark(f"{name}-cold-open")
        device.open_epub(path)
        device.mark(f"{name}-cold-ready")
        device.wait_for_log(
            "[DBG] [ERS] Progress saved:",
            after_host_time_ns=started.host_time_ns,
            timeout_s=180,
        )
        device.sleep(1)
        device.press("back", hold_ms=100, settle_ms=3000)
