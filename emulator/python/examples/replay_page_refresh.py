from argparse import ArgumentParser
from pathlib import Path

from crosspoint_emulator import Emulator, export_video


def main() -> None:
    parser = ArgumentParser(description="Replay one calibrated X4 page turn")
    parser.add_argument("--sd", required=True, type=Path)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--name", default="page-turn")
    parser.add_argument("--fps", type=int, default=30)
    arguments = parser.parse_args()

    artifacts = arguments.artifacts.resolve()
    with Emulator.launch("x4", sd=arguments.sd.resolve(), artifacts=artifacts) as device:
        device.wait_for_activity("home")
        device.wait_for_panel_idle()
        device.press_action("confirm")
        device.wait_for_activity("file_browser")
        device.wait_for_panel_idle()
        device.press_action("confirm")
        device.wait_for_activity("reader.epub", timeout_ms=30_000)
        device.wait_for_panel_idle(timeout_ms=30_000)

        state = device.state()
        generation = state.get("renderGeneration")
        if not isinstance(generation, int):
            raise RuntimeError("emulator state omitted renderGeneration")
        device.press_action("page_forward")
        device.wait_for_render(after=generation, timeout_ms=30_000)
        device.wait_for_panel_idle(timeout_ms=30_000)
        device.screenshot(f"{arguments.name}.png")

    output = artifacts / f"{arguments.name}.mp4"
    export_video(artifacts, output, fps=arguments.fps)
    print(output)


if __name__ == "__main__":
    main()
