from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator, export_video


def settle(device: Emulator) -> None:
    device.wait_for_panel_idle(timeout_ms=60_000, wall_timeout_ms=30_000)
    device.advance(200)


def main() -> None:
    parser = ArgumentParser(description="Replay X4 Reader percentage navigation")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--timing-profile", type=Path)
    arguments = parser.parse_args()

    artifacts = arguments.artifacts.resolve()
    timing_profile = arguments.timing_profile.resolve() if arguments.timing_profile is not None else None
    with Emulator.launch(
        "x4", sd=arguments.sd.resolve(), artifacts=artifacts, timing_profile=timing_profile
    ) as device:
        device.wait_for_activity("home")
        settle(device)

        device.press_action("down")
        settle(device)
        device.press_action("confirm")
        device.wait_for_activity("file_browser")
        settle(device)
        device.press_action("confirm")
        device.wait_for_activity("reader.epub", timeout_ms=60_000, wall_timeout_ms=30_000)
        settle(device)

        device.press_action("confirm")
        device.wait_for_activity("reader.epub.menu")
        settle(device)
        for _ in range(4):
            device.press_action("down")
            settle(device)

        device.press_action("confirm")
        device.wait_for_activity("reader.epub.percent")
        settle(device)
        device.screenshot("reader-percent-0.png")

        device.press_action("right")
        settle(device)
        device.screenshot("reader-percent-1.png")
        device.press_action("left")
        settle(device)
        device.press_action("up")
        settle(device)
        device.screenshot("reader-percent-10.png")
        device.press_action("down")
        settle(device)

        device.press_action("back")
        device.wait_for_activity("reader.epub")
        settle(device)
        device.press_action("back")
        device.wait_for_activity("home")
        settle(device)

    output = artifacts / "reader-percent-navigation.mp4"
    export_video(artifacts, output, fps=arguments.fps)
    print(output)


if __name__ == "__main__":
    main()
