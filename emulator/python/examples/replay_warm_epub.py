from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator


def main() -> None:
    parser = ArgumentParser(description="Open one already-cached EPUB through the production File Browser path")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--timing-profile", type=Path)
    arguments = parser.parse_args()

    artifacts = arguments.artifacts.resolve()
    timing_profile = arguments.timing_profile.resolve() if arguments.timing_profile is not None else None
    with Emulator.launch(
        "x4", sd=arguments.sd.resolve(), artifacts=artifacts, timing_profile=timing_profile
    ) as device:
        device.wait_for_activity("home")
        device.wait_for_panel_idle()
        device.press_action("confirm")
        device.wait_for_activity("file_browser")
        device.wait_for_panel_idle()
        device.press_action("confirm")
        device.wait_for_activity("reader.epub", timeout_ms=60_000, wall_timeout_ms=30_000)
        device.wait_for_panel_idle(timeout_ms=60_000, wall_timeout_ms=30_000)
        device.screenshot("warm-page.png")

    print(artifacts)


if __name__ == "__main__":
    main()
