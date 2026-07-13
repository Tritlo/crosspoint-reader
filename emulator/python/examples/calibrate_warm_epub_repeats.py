import os
import re
from pathlib import PurePosixPath

from crosspoint_emulator.hardware import HardwareDevice


WARM_REPEATS = 3


def run(device: HardwareDevice) -> None:
    """Measure repeated warm opens for one cached EPUB in an isolated run."""
    path = os.environ.get("CROSSPOINT_WARM_EPUB", "/stormlight.epub")
    name = PurePosixPath(path).stem
    if not path.startswith("/") or not path.endswith(".epub") or re.fullmatch(r"[a-z0-9-]+", name) is None:
        raise ValueError("CROSSPOINT_WARM_EPUB must be an absolute lowercase EPUB path")

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
