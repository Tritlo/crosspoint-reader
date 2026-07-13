from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator, export_video


def settle(device: Emulator) -> None:
    device.wait_for_panel_idle(timeout_ms=60_000, wall_timeout_ms=30_000)
    device.advance(200)


def main() -> None:
    parser = ArgumentParser(description="Replay X4 Reader menu and chapter-list navigation")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--fps", type=int, default=30)
    arguments = parser.parse_args()

    artifacts = arguments.artifacts.resolve()
    with Emulator.launch("x4", sd=arguments.sd.resolve(), artifacts=artifacts) as device:
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
        device.screenshot("reader.png")

        device.press_action("confirm")
        device.wait_for_activity("reader.epub.menu")
        settle(device)
        device.screenshot("reader-menu.png")
        for _ in range(3):
            device.press_action("down")
            settle(device)
            device.press_action("up")
            settle(device)
        device.screenshot("reader-menu-controls.png")

        device.press_action("confirm")
        device.wait_for_activity("reader.epub.chapters")
        settle(device)
        device.screenshot("reader-chapters.png")
        for _ in range(3):
            device.press_action("down")
            settle(device)
            device.press_action("up")
            settle(device)
        device.screenshot("reader-chapters-controls.png")

        device.press_action("back")
        device.wait_for_activity("reader.epub")
        settle(device)
        device.press_action("back")
        device.wait_for_activity("home")
        settle(device)

    output = artifacts / "reader-navigation.mp4"
    export_video(artifacts, output, fps=arguments.fps)
    print(output)


if __name__ == "__main__":
    main()
