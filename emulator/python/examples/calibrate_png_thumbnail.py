from pathlib import Path

from crosspoint_emulator import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Cold-open the local PNG-cover fixture and hold its settled Home card."""
    path = "/png-cover-calibration.epub"
    fixture = Path(__file__).resolve().parents[3] / "books-for-testing" / "png-cover-calibration-small.epub"
    device.upload_file(fixture, path)
    device.select_files_on_home()
    device.sleep(3)
    device.press("confirm", hold_ms=100, settle_ms=2000)

    device.mark("png-cache-clear")
    device.clear_epub_cache(path)
    device.mark("png-cold-open")
    device.open_epub(path)
    device.mark("png-cold-ready")

    home_start = device.mark("png-home")
    device.press("back", hold_ms=100, settle_ms=2500)
    device.wait_for_log(
        "Generated thumb BMP from PNG cover image, success: yes",
        after_host_time_ns=home_start.host_time_ns,
        timeout_s=300,
    )
    device.sleep(2)
    device.mark("png-home-ready")
