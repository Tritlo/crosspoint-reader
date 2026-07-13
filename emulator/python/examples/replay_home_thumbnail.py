from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator, export_video


def main() -> None:
    parser = ArgumentParser(description="Replay cold EPUB thumbnail generation on Home")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--timing-profile", type=Path)
    parser.add_argument("--hold-ms", type=int, default=0)
    parser.add_argument("--no-video", action="store_true")
    arguments = parser.parse_args()
    if arguments.hold_ms < 0:
        parser.error("hold-ms must be non-negative")

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

        device.press_action("back")
        device.wait_for_activity("home", timeout_ms=60_000, wall_timeout_ms=30_000)
        device.wait_for_panel_idle(timeout_ms=60_000, wall_timeout_ms=30_000)
        device.advance(arguments.hold_ms)
        device.screenshot("home-with-thumbnail.png")

    if arguments.no_video:
        print(artifacts)
    else:
        output = artifacts / "home-thumbnail.mp4"
        export_video(artifacts, output, fps=arguments.fps)
        print(output)


if __name__ == "__main__":
    main()
