from crosspoint_emulator import HardwareDevice


PATH = "/stormlight.epub"
EXPANDED_PAGES = 12


def enter_file_browser(device: HardwareDevice) -> None:
    device.select_files_on_home()
    device.sleep(3)
    device.press("confirm", hold_ms=100, settle_ms=2000)


def open_and_wait(device: HardwareDevice, marker: str) -> None:
    started = device.mark(marker)
    device.open_epub(PATH)
    device.wait_for_log(
        "[DBG] [ERS] Progress saved:",
        after_host_time_ns=started.host_time_ns,
        timeout_s=300,
    )


def return_home_and_wait_for_thumbnail(device: HardwareDevice, marker: str) -> None:
    started = device.mark(marker)
    device.press("back", hold_ms=100, settle_ms=2500)
    device.wait_for_log(
        "Generated thumb BMP from JPG cover image, success: yes",
        after_host_time_ns=started.host_time_ns,
        timeout_s=300,
    )
    device.sleep(2)


def run(device: HardwareDevice) -> None:
    """Clear current, minimal-open, and expanded Stormlight cache populations."""
    enter_file_browser(device)
    device.mark("cache-clear-current")
    device.clear_epub_cache(PATH)

    open_and_wait(device, "cache-minimal-open")
    return_home_and_wait_for_thumbnail(device, "cache-minimal-home")
    enter_file_browser(device)
    device.mark("cache-clear-minimal")
    device.clear_epub_cache(PATH)

    open_and_wait(device, "cache-expanded-open")
    for index in range(EXPANDED_PAGES):
        started = device.mark(f"cache-expanded-page-{index:02d}")
        device.press("down", hold_ms=100, settle_ms=100)
        device.wait_for_log(
            "[DBG] [ERS] Progress saved:",
            after_host_time_ns=started.host_time_ns,
            timeout_s=120,
        )
    return_home_and_wait_for_thumbnail(device, "cache-expanded-home")
    enter_file_browser(device)
    device.mark("cache-clear-expanded")
    device.clear_epub_cache(PATH)
