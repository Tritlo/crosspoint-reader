from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator, export_video


def main() -> None:
    parser = ArgumentParser(description="Replay the first image-heavy EPUB pages with calibrated timing")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--pages", type=int, default=5)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--timing-profile", type=Path)
    parser.add_argument("--no-video", action="store_true")
    arguments = parser.parse_args()
    if arguments.pages < 1:
        parser.error("pages must be positive")

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

        for _ in range(1, arguments.pages):
            device.press_action("down")
            device.wait_for_panel_idle(timeout_ms=60_000, wall_timeout_ms=30_000)
        device.screenshot("image-pages-final.png")

    if arguments.no_video:
        print(artifacts)
    else:
        output = artifacts / "image-pages.mp4"
        export_video(artifacts, output, fps=arguments.fps)
        print(output)


if __name__ == "__main__":
    main()
