import json
import shutil
import struct
import zlib
from pathlib import Path
from typing import cast

import pytest

from crosspoint_emulator import DeviceProfile, Emulator, export_video


REPOSITORY = Path(__file__).resolve().parents[3]


def decode_grayscale_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    offset = 8
    width = 0
    height = 0
    compressed = bytearray()
    while offset < len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4 : offset + 8]
        chunk = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(">IIBBBBB", chunk)
            assert (bit_depth, color_type, compression, filtering, interlace) == (8, 0, 0, 0, 0)
        elif chunk_type == b"IDAT":
            compressed.extend(chunk)
        elif chunk_type == b"IEND":
            break
    assert width > 0 and height > 0
    scanlines = zlib.decompress(compressed)
    assert len(scanlines) == (width + 1) * height
    pixels = bytearray()
    for y in range(height):
        start = y * (width + 1)
        assert scanlines[start] == 0
        pixels.extend(scanlines[start + 1 : start + 1 + width])
    return width, height, bytes(pixels)


def run_page_turn(profile: DeviceProfile, fixture: Path, artifacts: Path) -> tuple[tuple[int, int, bytes], str]:
    with Emulator.launch(profile, sd=fixture, artifacts=artifacts) as device:
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
        assert isinstance(generation, int)
        device.press_action("page_forward")
        device.wait_for_render(after=generation, timeout_ms=30_000)
        device.wait_for_panel_idle(timeout_ms=30_000)
        assert device.screenshot("page-2.png").is_file()
        assert device.capture_framebuffer("page-2-framebuffer.png").is_file()
        panel = decode_grayscale_png(device.capture_panel("page-2-panel.png"))

    events = (artifacts / "events.jsonl").read_text(encoding="utf-8")
    normalized = events.replace(str(artifacts.resolve()), "<ARTIFACTS>")
    assert '"activityId":"reader.epub"' in normalized
    return panel, normalized


@pytest.mark.parametrize("profile", ("x3", "x4"))
def test_epub_page_turn(profile: DeviceProfile, tmp_path: Path) -> None:
    fixture = tmp_path / "fixture"
    fixture.mkdir()
    shutil.copy2(REPOSITORY / "test" / "epubs" / "test_text_decorations.epub", fixture / "test.epub")

    first_panel, first_events = run_page_turn(profile, fixture, tmp_path / "run-a")
    second_panel, second_events = run_page_turn(profile, fixture, tmp_path / "run-b")
    assert first_panel == second_panel
    assert first_events == second_events
    assert first_panel == decode_grayscale_png(Path(__file__).parent / "goldens" / f"{profile}-page-2-panel.png")

    first_video = export_video(tmp_path / "run-a", tmp_path / f"{profile}-first.mp4", fps=8)
    second_video = export_video(tmp_path / "run-a", tmp_path / f"{profile}-second.mp4", fps=8)
    assert first_video.is_file()
    assert second_video.is_file()
    first_metadata = cast(dict[str, object], json.loads(first_video.with_suffix(".json").read_text(encoding="utf-8")))
    second_metadata = cast(dict[str, object], json.loads(second_video.with_suffix(".json").read_text(encoding="utf-8")))
    assert first_metadata["frameCount"] == second_metadata["frameCount"]
    assert first_metadata["durationSeconds"] == second_metadata["durationSeconds"]
    assert first_metadata["sampledFrameIdentities"] == second_metadata["sampledFrameIdentities"]
