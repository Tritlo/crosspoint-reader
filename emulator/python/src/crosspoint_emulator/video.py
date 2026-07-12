from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
from bisect import bisect_right
from pathlib import Path
from typing import cast

from .client import EmulatorError


def _read_json_object(path: Path) -> dict[str, object]:
    decoded: object = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(decoded, dict):
        raise EmulatorError(f"expected a JSON object in {path}")
    return cast(dict[str, object], decoded)


def _read_events(path: Path) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        decoded: object = json.loads(line)
        if not isinstance(decoded, dict):
            raise EmulatorError(f"event {line_number} in {path} is not an object")
        events.append(cast(dict[str, object], decoded))
    return events


def _rotation_filter(degrees: int) -> str | None:
    if degrees == 0:
        return None
    if degrees == 90:
        return "transpose=clock"
    if degrees == 180:
        return "hflip,vflip"
    if degrees == 270:
        return "transpose=cclock"
    raise EmulatorError(f"unsupported review rotation: {degrees}")


def export_video(
    run_directory: str | Path,
    output: str | Path | None = None,
    *,
    fps: int = 10,
    ffmpeg: str = "ffmpeg",
    ffprobe: str = "ffprobe",
) -> Path:
    """Replay canonical panel frames into a naturally oriented fixed-FPS MP4."""
    if fps <= 0:
        raise ValueError("fps must be positive")
    run = Path(run_directory).resolve()
    events_path = run / "events.jsonl"
    manifest = _read_json_object(run / "manifest.json")
    events = _read_events(events_path)
    frames: list[tuple[int, int, Path, str]] = []
    for event in events:
        if event.get("type") != "panel.frame":
            continue
        simulated_time = event.get("simulatedTimeUs")
        sequence = event.get("sequence")
        relative_path = event.get("path")
        identity = event.get("identity")
        if not isinstance(simulated_time, int) or not isinstance(sequence, int) or not isinstance(relative_path, str):
            raise EmulatorError(f"malformed panel.frame event: {event!r}")
        if not isinstance(identity, str):
            raise EmulatorError(f"panel.frame event omitted identity: {event!r}")
        frame_path = run / relative_path
        if not frame_path.is_file():
            raise EmulatorError(f"panel frame is missing: {frame_path}")
        frames.append((simulated_time, sequence, frame_path, identity))
    if not frames:
        raise EmulatorError(f"run has no panel.frame events: {run}")
    frames.sort(key=lambda frame: (frame[0], frame[1]))

    first_time_us = frames[0][0]
    final_event_time_us = max(
        (value for event in events if isinstance((value := event.get("simulatedTimeUs")), int)), default=first_time_us
    )
    duration_us = max(1_000_000 // fps, final_event_time_us - first_time_us)
    frame_count = max(1, (duration_us * fps + 999_999) // 1_000_000)
    frame_times = [frame[0] for frame in frames]
    sampled: list[tuple[Path, str]] = []
    for index in range(frame_count):
        sample_time = first_time_us + index * 1_000_000 // fps
        source_index = max(0, bisect_right(frame_times, sample_time) - 1)
        sampled.append((frames[source_index][2], frames[source_index][3]))

    ffmpeg_path = shutil.which(ffmpeg)
    ffprobe_path = shutil.which(ffprobe)
    if ffmpeg_path is None or ffprobe_path is None:
        raise EmulatorError("video export requires ffmpeg and ffprobe on PATH")

    presentation = run / "presentation"
    presentation.mkdir(parents=True, exist_ok=True)
    destination = Path(output) if output is not None else presentation / "recording.mp4"
    if not destination.is_absolute():
        destination = (Path.cwd() / destination).resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    samples_directory = presentation / f".{destination.stem}-samples"
    if samples_directory.exists():
        shutil.rmtree(samples_directory)
    samples_directory.mkdir()
    try:
        for index, (source, _identity) in enumerate(sampled):
            os.link(source, samples_directory / f"frame-{index:06d}.png")

        rotation_value = manifest.get("reviewRotationDegrees", 0)
        if not isinstance(rotation_value, int):
            raise EmulatorError("manifest reviewRotationDegrees must be an integer")
        video_filter = _rotation_filter(rotation_value)
        arguments = [
            ffmpeg_path,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-framerate",
            str(fps),
            "-start_number",
            "0",
            "-i",
            str(samples_directory / "frame-%06d.png"),
        ]
        if video_filter is not None:
            arguments.extend(("-vf", video_filter))
        arguments.extend(("-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(destination)))
        completed = subprocess.run(arguments, capture_output=True, check=False)
        if completed.returncode != 0:
            message = completed.stderr.decode("utf-8", errors="replace").strip()
            raise EmulatorError(f"ffmpeg failed: {message}")
    finally:
        shutil.rmtree(samples_directory, ignore_errors=True)

    probe_arguments = [
        ffprobe_path,
        "-v",
        "error",
        "-count_frames",
        "-select_streams",
        "v:0",
        "-show_entries",
        "stream=nb_read_frames,duration,r_frame_rate",
        "-of",
        "json",
        str(destination),
    ]
    probe = subprocess.run(probe_arguments, capture_output=True, check=False)
    if probe.returncode != 0:
        raise EmulatorError(f"ffprobe failed: {probe.stderr.decode('utf-8', errors='replace').strip()}")
    probe_json: object = json.loads(probe.stdout)
    if not isinstance(probe_json, dict):
        raise EmulatorError("ffprobe returned malformed JSON")
    streams = cast(dict[str, object], probe_json).get("streams")
    if not isinstance(streams, list) or not streams or not isinstance(streams[0], dict):
        raise EmulatorError("ffprobe returned no video stream")
    stream = cast(dict[str, object], streams[0])
    encoded_frames = stream.get("nb_read_frames")
    if not isinstance(encoded_frames, str) or int(encoded_frames) != frame_count:
        raise EmulatorError(f"encoded frame count {encoded_frames!r} does not match expected {frame_count}")

    version = subprocess.run((ffmpeg_path, "-version"), capture_output=True, check=True).stdout.decode("utf-8")
    metadata: dict[str, object] = {
        "sourceEvents": str(events_path),
        "output": str(destination),
        "fps": fps,
        "frameCount": frame_count,
        "durationSeconds": frame_count / fps,
        "firstSimulatedTimeUs": first_time_us,
        "lastSimulatedTimeUs": final_event_time_us,
        "reviewRotationDegrees": rotation_value,
        "sampledFrameIdentities": [identity for _path, identity in sampled],
        "ffmpegVersion": version.splitlines()[0],
        "ffmpegArguments": ["<FFMPEG>" if value == ffmpeg_path else value for value in arguments],
        "ffprobe": stream,
    }
    metadata_path = destination.with_suffix(".json")
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description="Export a CrossPoint emulator trace to MP4")
    parser.add_argument("run", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--fps", type=int, default=10)
    arguments = parser.parse_args()
    print(export_video(arguments.run, arguments.output, fps=arguments.fps))


if __name__ == "__main__":
    main()
