from pathlib import Path

from crosspoint_emulator.hardware import HardwareDevice


_FIXTURES = (
    ("png-321x482", "png-cover-calibration-321x482.epub"),
    ("png-642x963", "png-cover-calibration-642x963.epub"),
    ("png-1284x1927", "png-cover-calibration-1284x1927.epub"),
)


def run(device: HardwareDevice) -> None:
    """Measure three controlled RGB PNG cover sizes at the Home thumbnail boundary."""
    fixture_root = Path(__file__).resolve().parents[3] / "books-for-testing"
    for marker, fixture_name in _FIXTURES:
        path = f"/{fixture_name}"
        device.mark(f"{marker}-upload")
        device.upload_file(fixture_root / fixture_name, path)
        device.mark(f"{marker}-upload-ready")

        device.select_files_on_home()
        device.sleep(1)
        device.press("confirm", hold_ms=100, settle_ms=1500)

        device.mark(f"{marker}-cache-clear")
        device.clear_epub_cache(path)
        device.mark(f"{marker}-png-cold-open")
        device.open_epub(path)
        device.mark(f"{marker}-png-cold-ready")

        home_start = device.mark(f"{marker}-png-home")
        device.press("back", hold_ms=100, settle_ms=2500)
        device.wait_for_log(
            "Generated thumb BMP from PNG cover image, success: yes",
            after_host_time_ns=home_start.host_time_ns,
            timeout_s=300,
        )
        device.sleep(1)
        device.mark(f"{marker}-png-home-ready")
