from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator, export_video


def main() -> None:
    parser = ArgumentParser(description="Replay repeated X4 root-library opens")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--dwell-ms", type=int, default=3000)
    arguments = parser.parse_args()

    artifacts = arguments.artifacts.resolve()
    with Emulator.launch("x4", sd=arguments.sd.resolve(), artifacts=artifacts) as device:
        device.wait_for_activity("home")
        device.wait_for_panel_idle()
        for index in range(arguments.iterations):
            device.press_action("confirm")
            device.wait_for_activity("file_browser")
            device.wait_for_panel_idle()
            if index == arguments.iterations - 1:
                device.screenshot("library.png")
            device.advance(arguments.dwell_ms)
            device.press_action("back")
            device.wait_for_activity("home")
            device.wait_for_panel_idle()
            device.advance(arguments.dwell_ms)

    output = artifacts / "library-navigation.mp4"
    export_video(artifacts, output, fps=arguments.fps)
    print(output)


if __name__ == "__main__":
    main()
