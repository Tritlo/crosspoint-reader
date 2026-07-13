from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator, export_video


def settle(device: Emulator, dwell_ms: int) -> None:
    device.wait_for_panel_idle()
    device.advance(dwell_ms)


def main() -> None:
    parser = ArgumentParser(description="Replay X4 recent-book removal")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--dwell-ms", type=int, default=2000)
    arguments = parser.parse_args()

    artifacts = arguments.artifacts.resolve()
    with Emulator.launch("x4", sd=arguments.sd.resolve(), artifacts=artifacts) as device:
        device.wait_for_activity("home")
        settle(device, arguments.dwell_ms)

        for _ in range(2):
            device.press_action("down")
            settle(device, arguments.dwell_ms)
        device.press_action("confirm")
        settle(device, arguments.dwell_ms)
        device.screenshot("recent-books.png")

        device.button_down("confirm")
        device.advance(1_200)
        device.button_up("confirm")
        device.advance(20)
        settle(device, arguments.dwell_ms)
        device.screenshot("confirmation.png")

        device.press_action("right")
        settle(device, arguments.dwell_ms)
        device.screenshot("recent-books-empty.png")

        device.press_action("back")
        device.wait_for_activity("home")
        settle(device, arguments.dwell_ms)
        device.screenshot("home-empty.png")

    output = artifacts / "recent-removal.mp4"
    export_video(artifacts, output, fps=arguments.fps)
    print(output)


if __name__ == "__main__":
    main()
