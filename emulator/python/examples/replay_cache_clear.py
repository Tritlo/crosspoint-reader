from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator


def main() -> None:
    parser = ArgumentParser(description="Cold-open an EPUB, clear its cache, and open it cold again")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--book", required=True, help="absolute EPUB path inside the emulated SD")
    arguments = parser.parse_args()

    artifacts = arguments.artifacts.resolve()
    with Emulator.launch("x4", sd=arguments.sd.resolve(), artifacts=artifacts) as device:
        device.wait_for_activity("home")
        device.wait_for_panel_idle()
        device.press_action("confirm")
        device.wait_for_activity("file_browser")
        device.wait_for_panel_idle()
        device.press_action("confirm")
        device.wait_for_activity("reader.epub", timeout_ms=60_000, wall_timeout_ms=30_000)
        device.wait_for_panel_idle(timeout_ms=60_000, wall_timeout_ms=30_000)

        device.press_action("back")
        device.wait_for_activity("home", timeout_ms=60_000, wall_timeout_ms=30_000)
        device.wait_for_panel_idle(timeout_ms=60_000, wall_timeout_ms=30_000)
        device.press_action("down")
        device.press_action("confirm")
        device.wait_for_activity("file_browser")
        device.wait_for_panel_idle()

        result = device.clear_epub_cache(arguments.book)
        duration = result.get("durationUs")
        if not isinstance(duration, int):
            raise RuntimeError(f"cache-clear response omitted durationUs: {result!r}")
        print(f"cache clear: {duration} us")

        device.press_action("confirm")
        device.wait_for_activity("reader.epub", timeout_ms=60_000, wall_timeout_ms=30_000)
        device.wait_for_panel_idle(timeout_ms=60_000, wall_timeout_ms=30_000)
        device.screenshot("cold-after-cache-clear.png")

    print(artifacts)


if __name__ == "__main__":
    main()
