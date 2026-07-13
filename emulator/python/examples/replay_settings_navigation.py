from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator, export_video


def settle(device: Emulator, dwell_ms: int) -> None:
    device.wait_for_panel_idle()
    device.advance(dwell_ms)


def main() -> None:
    parser = ArgumentParser(description="Replay X4 Settings navigation and an enum popup")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--dwell-ms", type=int, default=2000)
    parser.add_argument("--physical-cadence", action="store_true")
    parser.add_argument("--timing-profile", type=Path)
    arguments = parser.parse_args()

    artifacts = arguments.artifacts.resolve()
    timing_profile = arguments.timing_profile.resolve() if arguments.timing_profile is not None else None
    with Emulator.launch(
        "x4", sd=arguments.sd.resolve(), artifacts=artifacts, timing_profile=timing_profile
    ) as device:
        device.wait_for_activity("home")
        settle(device, arguments.dwell_ms)

        for _ in range(3):
            device.press_action("down")
            settle(device, arguments.dwell_ms)

        device.press_action("confirm")
        device.wait_for_activity("settings")
        settle(device, arguments.dwell_ms)
        device.screenshot("settings.png")

        device.press_action("confirm")
        if arguments.physical_cadence:
            device.advance(1500)
        else:
            settle(device, arguments.dwell_ms)
        device.screenshot("settings-reader.png")

        for _ in range(3):
            device.press_action("down")
            if arguments.physical_cadence:
                device.advance(500)
            else:
                settle(device, arguments.dwell_ms)
        if not arguments.physical_cadence:
            device.screenshot("settings-row.png")

        device.press_action("confirm")
        if arguments.physical_cadence:
            device.advance(800)
        else:
            settle(device, arguments.dwell_ms)
        device.screenshot("settings-popup.png")

        for index in range(3):
            device.press_action("down")
            if arguments.physical_cadence:
                device.advance(500)
            else:
                settle(device, arguments.dwell_ms)
            if index == 0:
                device.screenshot("settings-popup-large.png")
            device.press_action("up")
            if arguments.physical_cadence:
                device.advance(500)
            else:
                settle(device, arguments.dwell_ms)
        if arguments.physical_cadence:
            settle(device, arguments.dwell_ms)
        device.screenshot("settings-popup-restored.png")

        device.press_action("back")
        settle(device, arguments.dwell_ms)
        device.press_action("back")
        settle(device, arguments.dwell_ms)
        device.press_action("back")
        device.wait_for_activity("home")
        settle(device, arguments.dwell_ms)

    output = artifacts / "settings-navigation.mp4"
    export_video(artifacts, output, fps=arguments.fps)
    print(output)


if __name__ == "__main__":
    main()
