from __future__ import annotations

import hashlib
import json
import re
import statistics
import subprocess
from pathlib import Path
from typing import cast

from .hardware import HardwareError, read_source_cadence

_PANEL_SAMPLE_RE = re.compile(r"CAL:BENCH:PANEL:(fast|half|full):(\d+):(black|white):(\d+)")
_GRAY_SAMPLE_RE = re.compile(r"CAL:BENCH:GRAY:(fast|half):(\d+):(light|dark):(\d+):(\d+)")
_FRAME_VALUE_RE = re.compile(r"pts_time:([0-9.]+)\n.*?VALUE=([0-9.]+)", re.DOTALL)
_GFX_RE = re.compile(r"\[DBG\] \[GFX\] Time = \d+ ms from clearScreen to displayBuffer")
_WAIT_RE = re.compile(r"Wait complete: refresh \((\d+) ms\)")
_PAGE_RE = re.compile(r"Page render \(tiled\): .* total=(\d+)ms")
_RENDERED_IMAGE_RE = re.compile(r"\[DBG\] \[IMG\] Rendering image at .* \((\d+)x(\d+)\)")

Crop = tuple[int, int, int, int]


def parse_crop(value: str) -> Crop:
    try:
        x, y, width, height = (int(part) for part in value.split(","))
    except ValueError as error:
        raise HardwareError("crop must be x,y,width,height") from error
    if min(x, y) < 0 or min(width, height) <= 0:
        raise HardwareError("crop coordinates must be non-negative and dimensions positive")
    return x, y, width, height


def _percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def _stats(values: list[float]) -> dict[str, int | float]:
    median = statistics.median(values)
    return {
        "samples": len(values),
        "min": round(min(values), 3),
        "p50": round(median, 3),
        "p90": round(_percentile(values, 0.9), 3),
        "mad": round(statistics.median(abs(value - median) for value in values), 3),
        "max": round(max(values), 3),
    }


def _change_peaks(changes: list[tuple[float, float]], maximum_gap_ms: float = 100.0) -> list[tuple[float, float]]:
    """Collapse adjacent changed video frames into one optical transition peak."""
    groups: list[list[tuple[float, float]]] = []
    for sample in changes:
        if not groups or sample[0] - groups[-1][-1][0] > maximum_gap_ms:
            groups.append([sample])
        else:
            groups[-1].append(sample)
    return [max(group, key=lambda sample: sample[1]) for group in groups]


def _signal_series(video: Path, crop: Crop, key: str, ffmpeg: str) -> list[tuple[float, float]]:
    x, y, width, height = crop
    command = (
        ffmpeg,
        "-v",
        "error",
        "-i",
        str(video),
        "-vf",
        f"crop={width}:{height}:{x}:{y},signalstats,"
        f"metadata=print:key=lavfi.signalstats.{key}:file=-",
        "-f",
        "null",
        "-",
    )
    completed = subprocess.run(command, capture_output=True, check=False, text=True)
    if completed.returncode != 0:
        raise HardwareError(f"FFmpeg signal analysis failed: {completed.stderr.strip()}")
    output = (completed.stdout + completed.stderr).replace(f"lavfi.signalstats.{key}", "VALUE")
    values = [(float(timestamp), float(value)) for timestamp, value in _FRAME_VALUE_RE.findall(output)]
    if not values:
        raise HardwareError(f"FFmpeg produced no {key} samples")
    return values


def _load_json_object(path: Path) -> dict[str, object]:
    try:
        value: object = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HardwareError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise HardwareError(f"expected a JSON object: {path}")
    return cast(dict[str, object], value)


def _command_times(run: Path, camera_started_ns: int) -> dict[str, float]:
    result: dict[str, float] = {}
    try:
        lines = (run / "events.jsonl").read_text(encoding="utf-8").splitlines()
        for line in lines:
            event = _load_json_line(line)
            text = event.get("line")
            timestamp = event.get("hostTimeNs")
            if not isinstance(text, str) or not isinstance(timestamp, int):
                continue
            match = re.fullmatch(r"CMD:CAL:BENCH:PANEL:(fast|half|full):\d+", text)
            if match:
                result[match.group(1)] = (timestamp - camera_started_ns) / 1_000_000_000
    except OSError as error:
        raise HardwareError(f"cannot read calibration events from {run}: {error}") from error
    if set(result) != {"fast", "half", "full"}:
        raise HardwareError("panel run is missing fast/half/full command timestamps")
    return result


def _load_json_line(line: str) -> dict[str, object]:
    try:
        value: object = json.loads(line)
    except json.JSONDecodeError as error:
        raise HardwareError(f"invalid calibration event: {error}") from error
    if not isinstance(value, dict):
        raise HardwareError("calibration event is not an object")
    return cast(dict[str, object], value)


def _panel_samples(run: Path) -> dict[str, list[tuple[int, str, int]]]:
    try:
        serial = (run / "serial.log").read_text(encoding="utf-8")
    except OSError as error:
        raise HardwareError(f"cannot read panel serial log from {run}: {error}") from error
    samples: dict[str, list[tuple[int, str, int]]] = {"fast": [], "half": [], "full": []}
    for mode, index, target, duration in _PANEL_SAMPLE_RE.findall(serial):
        samples[mode].append((int(index), target, int(duration)))
    for mode, values in samples.items():
        values.sort()
        if not values or [index for index, _, _ in values] != list(range(len(values))):
            raise HardwareError(f"panel run has incomplete {mode} samples")
    return samples


def _grayscale_batch_times(run: Path, video_start_ns: int) -> dict[str, float]:
    result: dict[str, float] = {}
    try:
        for line in (run / "events.jsonl").read_text(encoding="utf-8").splitlines():
            event = _load_json_line(line)
            text = event.get("line")
            timestamp = event.get("hostTimeNs")
            if not isinstance(text, str) or not isinstance(timestamp, int):
                continue
            match = re.fullmatch(r"CAL:BENCH-START:GRAY:(fast|half):\d+", text)
            if match:
                result[match.group(1)] = (timestamp - video_start_ns) / 1_000_000_000
    except OSError as error:
        raise HardwareError(f"cannot read grayscale events from {run}: {error}") from error
    if set(result) != {"fast", "half"}:
        raise HardwareError("grayscale run is missing fast/half batch timestamps")
    return result


def _grayscale_samples(run: Path) -> dict[str, list[tuple[int, str, int, int]]]:
    try:
        serial = (run / "serial.log").read_text(encoding="utf-8")
    except OSError as error:
        raise HardwareError(f"cannot read grayscale serial log from {run}: {error}") from error
    samples: dict[str, list[tuple[int, str, int, int]]] = {"fast": [], "half": []}
    for primary, index, target, start_us, duration_us in _GRAY_SAMPLE_RE.findall(serial):
        samples[primary].append((int(index), target, int(start_us), int(duration_us)))
    for primary, values in samples.items():
        values.sort()
        if not values or [index for index, _, _, _ in values] != list(range(len(values))):
            raise HardwareError(f"grayscale run has incomplete {primary} samples")
    return samples


def _reader_primary_operations(
    run: Path, video_start_ns: int, primary: str, minimum_image_pixels: int = 0
) -> list[tuple[int, float, int, int, int, int]]:
    if primary not in ("fast", "half"):
        raise ValueError("reader primary must be fast or half")
    operations: list[tuple[int, float, int, int, int, int]] = []
    display_start: float | None = None
    waits: list[int] = []
    image_pixels = 0
    try:
        for line in (run / "events.jsonl").read_text(encoding="utf-8").splitlines():
            event = _load_json_line(line)
            if event.get("direction") != "device":
                continue
            text = event.get("line")
            timestamp = event.get("hostTimeNs")
            if not isinstance(text, str) or not isinstance(timestamp, int):
                continue
            if _GFX_RE.search(text):
                display_start = (timestamp - video_start_ns) / 1_000_000_000
            if image := _RENDERED_IMAGE_RE.search(text):
                image_pixels = max(image_pixels, int(image.group(1)) * int(image.group(2)))
            if wait := _WAIT_RE.search(text):
                waits.append(int(wait.group(1)))
            page = _PAGE_RE.search(text)
            if page is None:
                continue
            is_half = len(waits) >= 2 and waits[-2] >= 1000
            if (
                display_start is not None
                and len(waits) >= 2
                and is_half == (primary == "half")
                and image_pixels >= minimum_image_pixels
            ):
                operations.append(
                    (len(operations), display_start, waits[-2], waits[-1], int(page.group(1)), image_pixels)
                )
            waits.clear()
            image_pixels = 0
    except OSError as error:
        raise HardwareError(f"cannot read reader events from {run}: {error}") from error
    if not operations:
        raise HardwareError(f"reader run contains no {primary}-primary page operations")
    return operations


def _camera_alignment(manifest: dict[str, object], run: Path) -> tuple[int, int, str, dict[str, object]]:
    camera_value = manifest.get("camera")
    if not isinstance(camera_value, dict):
        raise HardwareError("calibration manifest has no camera object")
    camera = cast(dict[str, object], camera_value)
    camera_started_ns = camera.get("startedHostTimeNs")
    first_frame_decoded_ns = camera.get("firstFrameDecodedHostTimeNs")
    first_frame_ns = camera.get("firstFrameObservedHostTimeNs")
    fps = camera.get("fps")
    if not isinstance(camera_started_ns, int) or not isinstance(fps, int) or fps <= 0:
        raise HardwareError("camera manifest is missing start timestamp or FPS")
    video_start_ns = (
        first_frame_decoded_ns
        if isinstance(first_frame_decoded_ns, int)
        else (first_frame_ns if isinstance(first_frame_ns, int) else camera_started_ns)
    )
    alignment = (
        "ffmpeg-first-frame-decoded-host-ns-v1"
        if isinstance(first_frame_decoded_ns, int)
        else (
            "ffmpeg-first-frame-observed-host-ns-v1"
            if isinstance(first_frame_ns, int)
            else "camera-process-start-host-ns-v1"
        )
    )
    cadence = {
        key: camera[key]
        for key in ("decodedSourceFrames", "observedSourceFps", "sourceFrameIntervalMs")
        if key in camera
    }
    if not isinstance(cadence.get("observedSourceFps"), int | float):
        cadence = read_source_cadence(run / "capture.ffmpeg.log")
    return video_start_ns, fps, alignment, cadence


def _frame_quantization_ms(cadence: dict[str, object], encoded_fps: int) -> float:
    intervals = cadence.get("sourceFrameIntervalMs")
    if isinstance(intervals, dict):
        p50 = cast(dict[str, object], intervals).get("p50")
        if isinstance(p50, int | float) and p50 > 0:
            return round(float(p50), 3)
    return round(1000 / encoded_fps, 3)


def _quantization_limitation(evidence: dict[str, object]) -> str:
    observed_fps = evidence.get("observedSourceFps")
    quantization_ms = evidence.get("frameQuantizationMs")
    if isinstance(observed_fps, int | float) and isinstance(quantization_ms, int | float):
        return (
            f"Auto-exposure, perspective, lighting, and {quantization_ms:g} ms camera-source quantization "
            f"({observed_fps:g} FPS observed) affect optical phase estimates."
        )
    return "Auto-exposure, perspective, lighting, and encoded-frame quantization affect optical phase estimates."


def _boot_serial_alignment(run: Path) -> tuple[int, int, int, int, int, float]:
    timed_events: list[tuple[int, int]] = []
    boot_entry_ms: int | None = None
    operation_start_ms: int | None = None
    operation_end_ms: int | None = None
    try:
        for line in (run / "events.jsonl").read_text(encoding="utf-8").splitlines():
            event = _load_json_line(line)
            if event.get("direction") != "device":
                continue
            text = event.get("line")
            host_time_ns = event.get("hostTimeNs")
            if not isinstance(text, str) or not isinstance(host_time_ns, int):
                continue
            timestamp = re.match(r"\[(\d+)]", text)
            if timestamp is None:
                continue
            device_time_ms = int(timestamp.group(1))
            timed_events.append((device_time_ms, host_time_ns))
            if boot_entry_ms is None and "[ACT] Entering activity: Boot" in text:
                boot_entry_ms = device_time_ms
            elif boot_entry_ms is not None and operation_start_ms is None and _GFX_RE.search(text):
                operation_start_ms = device_time_ms
            elif operation_start_ms is not None and operation_end_ms is None and _WAIT_RE.search(text):
                operation_end_ms = device_time_ms
    except OSError as error:
        raise HardwareError(f"cannot read boot events from {run}: {error}") from error
    if boot_entry_ms is None or operation_start_ms is None or operation_end_ms is None:
        raise HardwareError("boot run is missing Boot/displayBuffer/full-refresh boundaries")
    clock_origins = [
        host_time_ns - device_time_ms * 1_000_000
        for device_time_ms, host_time_ns in timed_events
        if device_time_ms <= boot_entry_ms
    ]
    if len(clock_origins) < 3:
        raise HardwareError("boot run has too few pre-refresh serial timestamps for clock alignment")
    clock_origin_ns = round(statistics.median(clock_origins))
    clock_origin_spread_ms = (max(clock_origins) - min(clock_origins)) / 1_000_000
    return boot_entry_ms, operation_start_ms, operation_end_ms, clock_origin_ns, len(clock_origins), clock_origin_spread_ms


def analyze_boot_waveform(
    run: Path,
    crop: Crop,
    *,
    threshold: float = 10.0,
    boundary_ms: float = 200.0,
    ffmpeg: str = "ffmpeg",
) -> dict[str, object]:
    manifest = _load_json_object(run / "manifest.json")
    if manifest.get("status") != "complete" or manifest.get("deviceProfile") != "x4":
        raise HardwareError("boot waveform analysis requires a complete X4 run")
    video_start_ns, fps, alignment, cadence = _camera_alignment(manifest, run)
    boot_entry_ms, operation_start_ms, operation_end_ms, clock_origin_ns, clock_samples, clock_spread_ms = (
        _boot_serial_alignment(run)
    )
    operation_start_seconds = (clock_origin_ns + operation_start_ms * 1_000_000 - video_start_ns) / 1_000_000_000
    operation_duration_ms = operation_end_ms - operation_start_ms
    differences = _signal_series(run / "capture.mp4", crop, "YDIF", ffmpeg)
    changes = [
        ((timestamp - operation_start_seconds) * 1000, value)
        for timestamp, value in differences
        if operation_start_seconds + boundary_ms / 1000 <= timestamp
        < operation_start_seconds + (operation_duration_ms - boundary_ms) / 1000
        and value >= threshold
    ]
    peaks = _change_peaks(changes)
    if not peaks:
        raise HardwareError("boot full refresh has no optical transitions above the configured threshold")
    pulse_intervals = [
        current[0] - previous[0]
        for previous, current in zip(peaks, peaks[1:])
        if 150 <= current[0] - previous[0] <= 400
    ]
    if not pulse_intervals:
        raise HardwareError("boot full refresh has no attributable optical pulse intervals")
    result: dict[str, object] = {
        "schemaVersion": 1,
        "kind": "x4-boot-full-refresh",
        "sourceRun": run.name,
        "videoSha256": hashlib.sha256((run / "capture.mp4").read_bytes()).hexdigest(),
        "captureFps": fps,
        "frameQuantizationMs": _frame_quantization_ms(cadence, fps),
        **cadence,
        "crop": {"x": crop[0], "y": crop[1], "width": crop[2], "height": crop[3]},
        "changeThreshold": threshold,
        "boundaryExclusionMs": boundary_ms,
        "alignment": alignment,
        "serialClockOriginSamples": clock_samples,
        "serialClockOriginSpreadMs": round(clock_spread_ms, 3),
        "bootEntryDeviceMs": boot_entry_ms,
        "operationStartDeviceMs": operation_start_ms,
        "operationEndDeviceMs": operation_end_ms,
        "operationDurationMs": operation_duration_ms,
        "operationStartVideoPtsSeconds": round(operation_start_seconds, 6),
        "changePeaks": [
            {"timeMs": round(time_ms, 3), "meanAbsoluteDifference": round(value, 3)}
            for time_ms, value in peaks
        ],
        "pulseIntervalMs": _stats(pulse_intervals),
    }
    result["limitations"] = [
        "Serial/video alignment uses the median pre-refresh device/host clock origin because displayBuffer blocks later log delivery.",
        "The natural Boot target is a conformance check, not another fitted full-refresh waveform.",
        _quantization_limitation(result),
    ]
    return result


def analyze_panel_waveform(
    run: Path,
    crop: Crop,
    *,
    threshold: float = 10.0,
    boundary_ms: float = 200.0,
    ffmpeg: str = "ffmpeg",
) -> dict[str, object]:
    manifest = _load_json_object(run / "manifest.json")
    if manifest.get("status") != "complete":
        raise HardwareError("optical analysis requires a complete calibration run")
    video_start_ns, fps, alignment, cadence = _camera_alignment(manifest, run)

    video = run / "capture.mp4"
    y_average = _signal_series(video, crop, "YAVG", ffmpeg)
    y_difference = _signal_series(video, crop, "YDIF", ffmpeg)
    command_times = _command_times(run, video_start_ns)
    panel_samples = _panel_samples(run)
    modes: dict[str, object] = {}

    for mode in ("fast", "half", "full"):
        operation_start = command_times[mode]
        operations: list[dict[str, object]] = []
        interior_starts: list[float] = []
        interior_ends: list[float] = []
        interior_spans: list[float] = []
        dominant_changes: list[float] = []
        pulse_intervals: list[float] = []
        target_changes: dict[str, dict[str, list[float]]] = {
            target: {"start": [], "end": [], "span": [], "dominant": []} for target in ("black", "white")
        }
        for index, target, duration_us in panel_samples[mode]:
            duration_seconds = duration_us / 1_000_000
            operation_end = operation_start + duration_seconds
            averages = [value for timestamp, value in y_average if operation_start <= timestamp < operation_end]
            changes = [
                ((timestamp - operation_start) * 1000, value)
                for timestamp, value in y_difference
                if operation_start <= timestamp < operation_end and value >= threshold
            ]
            if not averages:
                raise HardwareError(f"video has no frames for {mode} operation {index}")
            strongest = sorted(changes, key=lambda sample: sample[1], reverse=True)[:8]
            strongest.sort()
            interior = [
                sample
                for sample in changes
                if boundary_ms <= sample[0] <= duration_seconds * 1000 - boundary_ms
            ]
            peaks = _change_peaks(interior)
            if mode == "full":
                pulse_intervals.extend(
                    current[0] - previous[0]
                    for previous, current in zip(peaks, peaks[1:])
                    if 150.0 <= current[0] - previous[0] <= 400.0
                )
            operation: dict[str, object] = {
                "index": index,
                "target": target,
                "durationUs": duration_us,
                "initialY": round(statistics.mean(averages[: min(3, len(averages))]), 3),
                "finalY": round(statistics.mean(averages[-min(3, len(averages)) :]), 3),
                "strongestChanges": [
                    {"timeMs": round(time_ms, 3), "meanAbsoluteDifference": round(value, 3)}
                    for time_ms, value in strongest
                ],
                "changePeaks": [
                    {"timeMs": round(time_ms, 3), "meanAbsoluteDifference": round(value, 3)}
                    for time_ms, value in peaks
                ],
            }
            if interior:
                start_ms = interior[0][0]
                end_ms = interior[-1][0]
                dominant_ms = max(interior, key=lambda sample: sample[1])[0]
                operation["interiorChangeStartMs"] = round(start_ms, 3)
                operation["interiorChangeEndMs"] = round(end_ms, 3)
                operation["dominantChangeMs"] = round(dominant_ms, 3)
                interior_starts.append(start_ms)
                interior_ends.append(end_ms)
                interior_spans.append(end_ms - start_ms)
                dominant_changes.append(dominant_ms)
                target_changes[target]["start"].append(start_ms)
                target_changes[target]["end"].append(end_ms)
                target_changes[target]["span"].append(end_ms - start_ms)
                target_changes[target]["dominant"].append(dominant_ms)
            operations.append(operation)
            operation_start = operation_end

        summary: dict[str, object] = {
            "operations": operations,
            "operationUs": _stats([float(duration) for _, _, duration in panel_samples[mode]]),
        }
        if interior_starts:
            summary["interiorChangeStartMs"] = _stats(interior_starts)
            summary["interiorChangeEndMs"] = _stats(interior_ends)
            summary["interiorChangeSpanMs"] = _stats(interior_spans)
            summary["dominantChangeMs"] = _stats(dominant_changes)
            summary["byTarget"] = {
                target: {
                    "interiorChangeStartMs": _stats(values["start"]),
                    "interiorChangeEndMs": _stats(values["end"]),
                    "interiorChangeSpanMs": _stats(values["span"]),
                    "dominantChangeMs": _stats(values["dominant"]),
                }
                for target, values in target_changes.items()
                if values["start"]
            }
        if pulse_intervals:
            summary["pulseIntervalMs"] = _stats(pulse_intervals)
        modes[mode] = summary

    result: dict[str, object] = {
        "schemaVersion": 1,
        "sourceRun": run.name,
        "videoSha256": hashlib.sha256(video.read_bytes()).hexdigest(),
        "captureFps": fps,
        "frameQuantizationMs": _frame_quantization_ms(cadence, fps),
        **cadence,
        "crop": {"x": crop[0], "y": crop[1], "width": crop[2], "height": crop[3]},
        "changeThreshold": threshold,
        "boundaryExclusionMs": boundary_ms,
        "alignment": alignment,
        "modes": modes,
        "limitations": [
            (
                "The first-frame host timestamp is observed after camera decoding and may retain driver buffering."
                if alignment == "ffmpeg-first-frame-decoded-host-ns-v1"
                else (
                    "The first-frame host timestamp is observed after encoding and FFmpeg progress reporting."
                    if alignment == "ffmpeg-first-frame-observed-host-ns-v1"
                    else "Camera process start and encoded video PTS are not a hardware synchronization edge."
                )
            ),
            "The crop excludes the bezel but is not perspective-rectified.",
        ],
    }
    limitations = cast(list[str], result["limitations"])
    limitations.insert(-1, _quantization_limitation(result))
    return result


def analyze_grayscale_waveform(
    run: Path,
    crop: Crop,
    *,
    threshold: float = 3.0,
    window_ms: float = 500.0,
    ffmpeg: str = "ffmpeg",
) -> dict[str, object]:
    manifest = _load_json_object(run / "manifest.json")
    if manifest.get("status") != "complete":
        raise HardwareError("grayscale analysis requires a complete calibration run")
    video_start_ns, fps, alignment, cadence = _camera_alignment(manifest, run)
    video = run / "capture.mp4"
    y_average = _signal_series(video, crop, "YAVG", ffmpeg)
    y_difference = _signal_series(video, crop, "YDIF", ffmpeg)
    batch_times = _grayscale_batch_times(run, video_start_ns)
    samples = _grayscale_samples(run)
    primaries: dict[str, object] = {}

    for primary in ("fast", "half"):
        operations: list[dict[str, object]] = []
        target_metrics: dict[str, dict[str, list[float]]] = {
            target: {"start": [], "end": [], "dominant": [], "finalY": []} for target in ("light", "dark")
        }
        for index, target, start_offset_us, duration_us in samples[primary]:
            start = batch_times[primary] + start_offset_us / 1_000_000
            window_end = start + window_ms / 1000
            pre = [value for timestamp, value in y_average if start - 0.1 <= timestamp < start]
            post = [value for timestamp, value in y_average if window_end - 0.1 <= timestamp < window_end]
            changes = [
                ((timestamp - start) * 1000, value)
                for timestamp, value in y_difference
                if start <= timestamp < window_end and value >= threshold
            ]
            if not pre or not post:
                raise HardwareError(f"video has no grayscale boundary frames for {primary} operation {index}")
            peaks = _change_peaks(changes)
            operation: dict[str, object] = {
                "index": index,
                "target": target,
                "startOffsetUs": start_offset_us,
                "durationUs": duration_us,
                "initialY": round(statistics.mean(pre), 3),
                "finalY": round(statistics.mean(post), 3),
                "changePeaks": [
                    {"timeMs": round(time_ms, 3), "meanAbsoluteDifference": round(value, 3)}
                    for time_ms, value in peaks
                ],
            }
            if changes:
                start_ms = changes[0][0]
                end_ms = changes[-1][0]
                dominant_ms = max(changes, key=lambda sample: sample[1])[0]
                operation["visibleChangeStartMs"] = round(start_ms, 3)
                operation["visibleChangeEndMs"] = round(end_ms, 3)
                operation["dominantChangeMs"] = round(dominant_ms, 3)
                metrics = target_metrics[target]
                metrics["start"].append(start_ms)
                metrics["end"].append(end_ms)
                metrics["dominant"].append(dominant_ms)
                metrics["finalY"].append(statistics.mean(post))
            operations.append(operation)
        by_target = {
            target: {
                "visibleChangeStartMs": _stats(values["start"]),
                "visibleChangeEndMs": _stats(values["end"]),
                "dominantChangeMs": _stats(values["dominant"]),
                "finalY": _stats(values["finalY"]),
            }
            for target, values in target_metrics.items()
            if values["start"]
        }
        primaries[primary] = {
            "operations": operations,
            "operationUs": _stats([float(duration) for _, _, _, duration in samples[primary]]),
            "visibleChangeDetected": bool(by_target),
            "byTarget": by_target,
        }

    result: dict[str, object] = {
        "schemaVersion": 1,
        "kind": "x4-differential-grayscale",
        "sourceRun": run.name,
        "videoSha256": hashlib.sha256(video.read_bytes()).hexdigest(),
        "captureFps": fps,
        "frameQuantizationMs": _frame_quantization_ms(cadence, fps),
        **cadence,
        "crop": {"x": crop[0], "y": crop[1], "width": crop[2], "height": crop[3]},
        "changeThreshold": threshold,
        "analysisWindowMs": window_ms,
        "alignment": alignment,
        "primaries": primaries,
    }
    result["limitations"] = [
        "Batch-start alignment uses serial receipt plus device-measured per-operation offsets.",
        "The full-screen light/dark targets isolate waveform timing but are not representative page content.",
        "No detected transition means every operation stayed below the configured change threshold; lowering the "
        "threshold would model camera noise rather than a measured panel phase.",
        _quantization_limitation(result),
    ]
    return result


def analyze_reader_half_waveform(
    run: Path,
    crop: Crop,
    *,
    threshold: float = 5.0,
    window_ms: float = 2200.0,
    ffmpeg: str = "ffmpeg",
) -> dict[str, object]:
    manifest = _load_json_object(run / "manifest.json")
    if manifest.get("status") != "complete":
        raise HardwareError("reader half-waveform analysis requires a complete calibration run")
    video_start_ns, fps, alignment, cadence = _camera_alignment(manifest, run)
    video = run / "capture.mp4"
    differences = _signal_series(video, crop, "YDIF", ffmpeg)
    operations = _reader_primary_operations(run, video_start_ns, "half")
    analyzed: list[dict[str, object]] = []
    inversion_starts: list[float] = []
    target_settles: list[float] = []
    for index, start, primary_busy_ms, grayscale_busy_ms, total_ms, _ in operations:
        changes = [
            ((timestamp - start) * 1000, value)
            for timestamp, value in differences
            if start <= timestamp < start + window_ms / 1000 and value >= threshold
        ]
        peaks = _change_peaks(changes)
        if len(peaks) < 2:
            raise HardwareError(f"reader half operation {index} has fewer than two optical transition peaks")
        inversion_ms = peaks[0][0]
        settle_ms = peaks[1][0]
        inversion_starts.append(inversion_ms)
        target_settles.append(settle_ms)
        analyzed.append(
            {
                "index": index,
                "primaryBusyMs": primary_busy_ms,
                "grayscaleBusyMs": grayscale_busy_ms,
                "totalMs": total_ms,
                "invertedTargetStartMs": round(inversion_ms, 3),
                "targetSettleMs": round(settle_ms, 3),
                "changePeaks": [
                    {"timeMs": round(time_ms, 3), "meanAbsoluteDifference": round(value, 3)}
                    for time_ms, value in peaks
                ],
            }
        )
    result: dict[str, object] = {
        "schemaVersion": 1,
        "kind": "x4-reader-half-inversion",
        "sourceRun": run.name,
        "videoSha256": hashlib.sha256(video.read_bytes()).hexdigest(),
        "captureFps": fps,
        "frameQuantizationMs": _frame_quantization_ms(cadence, fps),
        **cadence,
        "crop": {"x": crop[0], "y": crop[1], "width": crop[2], "height": crop[3]},
        "changeThreshold": threshold,
        "analysisWindowMs": window_ms,
        "alignment": alignment,
        "operations": analyzed,
        "invertedTargetStartMs": _stats(inversion_starts),
        "targetSettleMs": _stats(target_settles),
    }
    result["limitations"] = [
        "The first-frame host timestamp is observed after camera decoding and may retain driver buffering.",
        "The two dominant page-content changes are interpreted as inverted-target drive and target settling.",
        _quantization_limitation(result),
    ]
    return result


def _analyze_reader_direct_waveform(
    run: Path,
    crop: Crop,
    *,
    threshold: float,
    window_ms: float,
    minimum_image_pixels: int,
    kind: str,
    label: str,
    ffmpeg: str,
) -> dict[str, object]:
    manifest = _load_json_object(run / "manifest.json")
    if manifest.get("status") != "complete":
        raise HardwareError(f"reader {label} waveform analysis requires a complete calibration run")
    video_start_ns, fps, alignment, cadence = _camera_alignment(manifest, run)
    video = run / "capture.mp4"
    differences = _signal_series(video, crop, "YDIF", ffmpeg)
    operations = _reader_primary_operations(run, video_start_ns, "fast", minimum_image_pixels)
    analyzed: list[dict[str, object]] = []
    starts: list[float] = []
    ends: list[float] = []
    dominant: list[float] = []
    for index, start, primary_busy_ms, grayscale_busy_ms, total_ms, image_pixels in operations:
        changes = [
            ((timestamp - start) * 1000, value)
            for timestamp, value in differences
            if start <= timestamp < start + window_ms / 1000 and value >= threshold
        ]
        peaks = _change_peaks(changes)
        if not changes or len(peaks) != 1:
            raise HardwareError(f"reader {label} operation {index} does not have one optical transition")
        start_ms = changes[0][0]
        end_ms = changes[-1][0]
        dominant_ms = peaks[0][0]
        starts.append(start_ms)
        ends.append(end_ms)
        dominant.append(dominant_ms)
        operation: dict[str, object] = {
            "index": index,
            "primaryBusyMs": primary_busy_ms,
            "grayscaleBusyMs": grayscale_busy_ms,
            "totalMs": total_ms,
            "transitionStartMs": round(start_ms, 3),
            "transitionEndMs": round(end_ms, 3),
            "dominantChangeMs": round(dominant_ms, 3),
            "changePeak": {
                "timeMs": round(dominant_ms, 3),
                "meanAbsoluteDifference": round(peaks[0][1], 3),
            },
        }
        if minimum_image_pixels > 0:
            operation["sourceImagePixels"] = image_pixels
        analyzed.append(operation)
    return {
        "schemaVersion": 1,
        "kind": kind,
        "sourceRun": run.name,
        "videoSha256": hashlib.sha256(video.read_bytes()).hexdigest(),
        "captureFps": fps,
        "frameQuantizationMs": _frame_quantization_ms(cadence, fps),
        **cadence,
        "crop": {"x": crop[0], "y": crop[1], "width": crop[2], "height": crop[3]},
        "changeThreshold": threshold,
        "analysisWindowMs": window_ms,
        "alignment": alignment,
        "operations": analyzed,
        "transitionStartMs": _stats(starts),
        "transitionEndMs": _stats(ends),
        "dominantChangeMs": _stats(dominant),
    }


def analyze_reader_fast_waveform(
    run: Path,
    crop: Crop,
    *,
    threshold: float = 5.0,
    window_ms: float = 800.0,
    ffmpeg: str = "ffmpeg",
) -> dict[str, object]:
    analysis = _analyze_reader_direct_waveform(
        run,
        crop,
        threshold=threshold,
        window_ms=window_ms,
        minimum_image_pixels=0,
        kind="x4-reader-fast-transition",
        label="fast",
        ffmpeg=ffmpeg,
    )
    analysis["limitations"] = [
        "The first-frame host timestamp is observed after camera decoding and may retain driver buffering.",
        "The page-content change is represented as one direct old-to-new transition without an inverted state.",
        _quantization_limitation(analysis),
    ]
    return analysis


def analyze_reader_image_fast_waveform(
    run: Path,
    crop: Crop,
    *,
    threshold: float = 5.0,
    window_ms: float = 800.0,
    minimum_image_area_fraction: float = 0.5,
    target_dark_pixel_percent_min: int = 50,
    ffmpeg: str = "ffmpeg",
) -> dict[str, object]:
    manifest = _load_json_object(run / "manifest.json")
    if manifest.get("status") != "complete" or manifest.get("deviceProfile") != "x4":
        raise HardwareError("reader image fast-waveform analysis requires a complete X4 calibration run")
    if not 0 < minimum_image_area_fraction <= 1:
        raise HardwareError("minimum image area fraction must be in (0, 1]")
    if not 1 <= target_dark_pixel_percent_min <= 100:
        raise HardwareError("target dark-pixel percentage must be in [1, 100]")
    minimum_image_pixels = round(800 * 480 * minimum_image_area_fraction)
    analysis = _analyze_reader_direct_waveform(
        run,
        crop,
        threshold=threshold,
        window_ms=window_ms,
        minimum_image_pixels=minimum_image_pixels,
        kind="x4-reader-image-fast-transition",
        label="image fast",
        ffmpeg=ffmpeg,
    )
    analysis.update(
        {
            "sourceImageAreaPixelsMin": minimum_image_pixels,
            "sourceImageAreaFractionMin": minimum_image_area_fraction,
            "targetDarkPixelPercentMin": target_dark_pixel_percent_min,
            "limitations": [
                "The first-frame host timestamp is observed after camera decoding and may retain driver buffering.",
                "Source samples render an image over at least half of the X4 panel; runtime uses the recorded conservative majority-dark target selector.",
                "The image-content change is represented as one lighter direct-drive phase followed by the settled target.",
                _quantization_limitation(analysis),
            ],
        }
    )
    return analysis


def write_waveform(analysis: dict[str, object], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(analysis, indent=2, sort_keys=True) + "\n", encoding="utf-8")
