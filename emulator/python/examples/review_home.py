from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import DeviceProfile, Emulator


def main() -> None:
    parser = ArgumentParser(description="Capture a deterministic CrossPoint Home review")
    parser.add_argument("--device", choices=("x3", "x4"), default="x4")
    parser.add_argument("--sd", type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--video", action="store_true")
    arguments = parser.parse_args()

    device_profile: DeviceProfile = arguments.device
    with Emulator.launch(device_profile, sd=arguments.sd, artifacts=arguments.artifacts) as device:
        device.wait_for_activity("home")
        device.wait_for_panel_idle()
        print(device.screenshot("home.png"))

    if arguments.video:
        print(device.export_video(arguments.artifacts / "run.mp4"))


if __name__ == "__main__":
    main()
