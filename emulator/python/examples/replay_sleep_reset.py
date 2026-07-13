from argparse import ArgumentParser
from pathlib import Path
from typing import cast

from crosspoint_emulator import Emulator, export_video


def is_blocked_app_task(value: object) -> bool:
    if not isinstance(value, dict):
        return False
    task = cast(dict[str, object], value)
    return task.get("name") == "app-main" and task.get("state") == "blocked"


def main() -> None:
    parser = ArgumentParser(description="Replay X4 deep sleep and retained-panel reset")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--dwell-ms", type=int, default=2000)
    arguments = parser.parse_args()

    artifacts = arguments.artifacts.resolve()
    reset_artifacts = artifacts / "resets" / "1"
    with Emulator.launch("x4", sd=arguments.sd.resolve(), artifacts=artifacts) as device:
        device.wait_for_activity("home")
        device.wait_for_panel_idle()
        device.advance(arguments.dwell_ms)
        device.screenshot("home.png")

        device.action_down("power")
        device.advance(600)
        device.wait_for_activity("sleep")
        device.action_up("power")
        device.wait_for_panel_idle()
        device.advance(arguments.dwell_ms)
        state = device.state()
        tasks = state.get("tasks")
        if not isinstance(tasks, list) or not any(is_blocked_app_task(task) for task in cast(list[object], tasks)):
            raise RuntimeError(f"application task did not quiesce in deep sleep: {tasks!r}")
        sleep_panel = device.capture_panel("sleep-panel.png")
        device.screenshot("sleep.png")

        device.reset()
        retained_panel = artifacts / "captures" / "reset-panel.png"
        if sleep_panel.read_bytes() != retained_panel.read_bytes():
            raise RuntimeError("reset snapshot does not match the visible sleep panel")

        device.wait_for_activity("home")
        device.wait_for_panel_idle()
        device.advance(arguments.dwell_ms)
        device.screenshot("home-after-reset.png")

    sleep_video = export_video(artifacts, artifacts / "sleep-entry.mp4", fps=arguments.fps)
    wake_video = export_video(reset_artifacts, artifacts / "reset-to-home.mp4", fps=arguments.fps)
    print(sleep_video)
    print(wake_video)


if __name__ == "__main__":
    main()
