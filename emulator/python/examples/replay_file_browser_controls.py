from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator, export_video


def settle(device: Emulator) -> None:
    device.wait_for_panel_idle()
    device.advance(200)


def main() -> None:
    parser = ArgumentParser(description="Replay repeated X4 File Browser selector controls")
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

        for _ in range(6):
            device.press_action("down")
            settle(device)
            device.press_action("up")
            settle(device)
        device.screenshot("file-browser-controls.png")

    output = artifacts / "file-browser-controls.mp4"
    export_video(artifacts, output, fps=arguments.fps)
    print(output)


if __name__ == "__main__":
    main()
