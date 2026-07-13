from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from typing import cast

Corners = tuple[int, int, int, int, int, int, int, int]


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def probe_video(ffprobe: str, path: Path) -> dict[str, int | float | str]:
    completed = subprocess.run(
        (
            ffprobe,
            "-v",
            "error",
            "-count_frames",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height,r_frame_rate,nb_read_frames:format=duration",
            "-of",
            "json",
            str(path),
        ),
        capture_output=True,
        check=False,
        text=True,
    )
    if completed.returncode != 0:
        raise SystemExit(completed.stderr.strip() or f"ffprobe failed with code {completed.returncode}")
    try:
        value: object = json.loads(completed.stdout)
        root = cast(dict[str, object], value)
        streams = cast(list[object], root["streams"])
        stream = cast(dict[str, object], streams[0])
        format_value = cast(dict[str, object], root["format"])
        return {
            "width": int(cast(str | int, stream["width"])),
            "height": int(cast(str | int, stream["height"])),
            "rFrameRate": str(stream["r_frame_rate"]),
            "frameCount": int(cast(str | int, stream["nb_read_frames"])),
            "durationSeconds": float(cast(str | float, format_value["duration"])),
        }
    except (IndexError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot parse ffprobe output for {path}: {error}") from error


def parse_corners(value: str) -> Corners:
    try:
        parts = [int(part) for part in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("corners must be eight comma-separated integers") from error
    if len(parts) != 8 or any(part < 0 for part in parts):
        raise argparse.ArgumentTypeError("corners must be eight non-negative integers")
    return (parts[0], parts[1], parts[2], parts[3], parts[4], parts[5], parts[6], parts[7])


def main() -> None:
    parser = argparse.ArgumentParser(description="Rectify and align physical/emulator review video")
    parser.add_argument("--physical", required=True, type=Path)
    parser.add_argument("--physical-start", required=True, type=float)
    parser.add_argument("--emulator", required=True, type=Path)
    parser.add_argument("--emulator-start", required=True, type=float)
    parser.add_argument("--duration", required=True, type=float)
    parser.add_argument("--corners", required=True, type=parse_corners, metavar="X0,Y0,X1,Y1,X2,Y2,X3,Y3")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--width", type=int, default=480)
    parser.add_argument("--height", type=int, default=800)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    arguments = parser.parse_args()

    physical = cast(Path, arguments.physical).resolve()
    emulator = cast(Path, arguments.emulator).resolve()
    output = cast(Path, arguments.output).resolve()
    physical_start = cast(float, arguments.physical_start)
    emulator_start = cast(float, arguments.emulator_start)
    duration = cast(float, arguments.duration)
    width = cast(int, arguments.width)
    height = cast(int, arguments.height)
    fps = cast(int, arguments.fps)
    corners = cast(Corners, arguments.corners)
    if not physical.is_file() or not emulator.is_file():
        parser.error("physical and emulator inputs must be files")
    if physical_start < 0 or emulator_start < 0 or duration <= 0:
        parser.error("start times must be non-negative and duration must be positive")
    if width <= 0 or height <= 0 or fps <= 0:
        parser.error("width, height, and fps must be positive")

    physical_start_frame = round(physical_start * fps)
    emulator_start_frame = round(emulator_start * fps)
    frame_count = round(duration * fps)
    if frame_count <= 0:
        parser.error("duration must include at least one output frame")

    x0, y0, x1, y1, x2, y2, x3, y3 = corners
    label_height = 50
    physical_filter = (
        f"fps={fps},trim=start_frame={physical_start_frame}:"
        f"end_frame={physical_start_frame + frame_count},setpts=N/({fps}*TB),"
        f"perspective=x0={x0}:y0={y0}:x1={x1}:y1={y1}:x2={x2}:y2={y2}:x3={x3}:y3={y3}:"
        f"sense=source:interpolation=cubic,scale={width}:{height}:flags=lanczos,"
        f"pad={width}:{height + label_height}:0:{label_height}:white,"
        "drawtext=text='Physical camera':x=(w-text_w)/2:y=15:fontsize=20:fontcolor=black[p]"
    )
    emulator_filter = (
        f"fps={fps},trim=start_frame={emulator_start_frame}:"
        f"end_frame={emulator_start_frame + frame_count},setpts=N/({fps}*TB),"
        f"scale={width}:{height}:flags=neighbor,"
        f"pad={width}:{height + label_height}:0:{label_height}:white,"
        "drawtext=text='Emulator':x=(w-text_w)/2:y=15:fontsize=20:fontcolor=black[e]"
    )
    filter_complex = f"[0:v]{physical_filter};[1:v]{emulator_filter};[p][e]hstack=inputs=2:shortest=1[out]"
    output.parent.mkdir(parents=True, exist_ok=True)
    ffmpeg = cast(str, arguments.ffmpeg)
    ffprobe = cast(str, arguments.ffprobe)
    ffmpeg_arguments = (
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        str(physical),
        "-i",
        str(emulator),
        "-filter_complex",
        filter_complex,
        "-map",
        "[out]",
        "-an",
        "-c:v",
        "libx264",
        "-preset",
        "veryfast",
        "-crf",
        "18",
        "-pix_fmt",
        "yuv420p",
        "-movflags",
        "+faststart",
        str(output),
    )
    completed = subprocess.run(
        ffmpeg_arguments,
        capture_output=True,
        check=False,
        text=True,
    )
    if completed.returncode != 0:
        raise SystemExit(completed.stderr.strip() or f"ffmpeg failed with code {completed.returncode}")

    probe = probe_video(ffprobe, output)
    expected_width = width * 2
    expected_height = height + label_height
    if (
        probe["width"] != expected_width
        or probe["height"] != expected_height
        or probe["rFrameRate"] != f"{fps}/1"
        or probe["frameCount"] != frame_count
    ):
        raise SystemExit(
            f"comparison verification failed: expected {expected_width}x{expected_height}, {fps} FPS, "
            f"{frame_count} frames; got {probe}"
        )

    version = subprocess.run((ffmpeg, "-version"), capture_output=True, check=False, text=True)
    ffmpeg_version = version.stdout.splitlines()[0] if version.returncode == 0 and version.stdout else "unknown"
    manifest = {
        "schemaVersion": 1,
        "kind": "physical-emulator-video-comparison",
        "physical": {
            "input": str(physical),
            "sha256": sha256_file(physical),
            "requestedStartSeconds": physical_start,
            "startFrame": physical_start_frame,
            "corners": list(corners),
        },
        "emulator": {
            "input": str(emulator),
            "sha256": sha256_file(emulator),
            "requestedStartSeconds": emulator_start,
            "startFrame": emulator_start_frame,
        },
        "output": str(output),
        "outputSha256": sha256_file(output),
        "requestedDurationSeconds": duration,
        "fps": fps,
        "frameCount": frame_count,
        "width": expected_width,
        "height": expected_height,
        "reviewRotationDegrees": 0,
        "ffmpegVersion": ffmpeg_version,
        "ffmpegArguments": list(ffmpeg_arguments),
        "ffprobe": probe,
    }
    manifest_path = output.with_suffix(".json")
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
