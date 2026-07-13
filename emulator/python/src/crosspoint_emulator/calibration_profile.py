from __future__ import annotations

import hashlib
import json
import re
import statistics
from pathlib import Path
from typing import cast

from .hardware import HardwareError

_PAGE_RE = re.compile(
    r"Page render \(tiled\): prewarm=(\d+)ms bw_render=(\d+)ms display=(\d+)ms "
    r"gray_lsb=(\d+)ms gray_msb=(\d+)ms gray_display=(\d+)ms cleanup=(\d+)ms total=(\d+)ms"
)
_WAIT_RE = re.compile(r"Wait complete: refresh \((\d+) ms\)")
_PANEL_RE = re.compile(r"CAL:BENCH:PANEL:(fast|half|full):\d+:(?:black|white):(\d+)")
_SD_RE = re.compile(r"CAL:BENCH:SD:(WRITE|READ):\d+:(\d+):(\d+):(\d+):(\d+):1")
_SD_END_RE = re.compile(r"CAL:BENCH-END:SD:(\d+):(\d+):")
_DIRECTORY_RE = re.compile(r"CAL:BENCH:DIR:\d+:(\d+):(\d+):(\d+):(\d+):(\d+):(\d+):1")
_DIRECTORY_END_RE = re.compile(r"CAL:BENCH-END:DIR:(\d+):")
_CACHE_CLEAR_RE = re.compile(r"CAL:CACHE:CLEARED:[^:\r\n]+:(\d+)(?::\d+:\d+:\d+)?:\d+(?=\r?\n|$)")
_CACHE_CLEAR_POPULATION_RE = re.compile(
    r"CAL:CACHE:CLEARED:[^:\r\n]+:(\d+):(\d+):(\d+):(\d+):\d+(?=\r?\n|$)"
)
_MARK_RE = re.compile(r"CAL:MARK:([a-zA-Z0-9_.-]+):(\d+)")
_CAL_ACTIVITY_RENDER_RE = re.compile(r"CAL:RENDER:([^:\r\n]+):(\d+)")
_TIMED_LOG_RE = re.compile(r"\[(\d+)]")
_EPUB_LOAD_RE = re.compile(r"\[DBG\] \[EBP\] Loading ePub: (/[^\r\n]+)")
_THUMB_START_RE = re.compile(r"Generating thumb BMP from (JPG|PNG) cover image")
_THUMB_END_RE = re.compile(r"Generated thumb BMP from (?:JPG|PNG) cover image, success: yes")
_DECOMPRESSED_RE = re.compile(r"Decompressed (\d+) bytes into (\d+) bytes")
_JPEG_DIMENSIONS_RE = re.compile(r"JPEG dimensions: (\d+)x(\d+)")
_PNG_DIMENSIONS_RE = re.compile(r"Image: (\d+)x(\d+), depth=(\d+), color=(\d+)")
_THUMB_TARGET_RE = re.compile(r"Converting (?:JPEG|PNG) to (?:1|2)-bit BMP \(target: (\d+)x(\d+)\)")
_IMAGE_RENDER_RE = re.compile(r"Rendering image at -?\d+,-?\d+: (.+) \((\d+)x(\d+)\)")
_IMAGE_FOUND_RE = re.compile(r"\[DBG\] \[EHP\] Found image:")
_EHP_IMAGE_DIMENSIONS_RE = re.compile(r"\[DBG\] \[EHP\] Image dimensions: (\d+)x(\d+)")
_IMAGE_DECODE_RE = re.compile(r"Decoding and caching: (.+)")
_JPEG_SCALE_RE = re.compile(r"JPEG (\d+)x(\d+) -> (\d+)x(\d+) \(scale")
_CACHE_LOAD_RE = re.compile(r"Loading from cache: (.+) \((\d+)x(\d+)\)")
_CACHE_WRITTEN_RE = re.compile(r"Cache written: (.+) \((\d+)x(\d+), (\d+) bytes\)")
_GFX_RENDER_MS_RE = re.compile(r"\[DBG\] \[GFX\] Time = (\d+) ms from clearScreen to displayBuffer")
_BUTTON_DOWN_RE = re.compile(r"CAL:BUTTON:(down|up):DOWN:(\d+)")
_CONFIRM_DOWN_RE = re.compile(r"CAL:BUTTON:confirm:DOWN:(\d+)")
_CONFIRM_UP_RE = re.compile(r"CAL:BUTTON:confirm:UP:\d+")
_CONFIRMATION_ENTER_RE = re.compile(r"\[(\d+)] \[DBG\] \[ACT\] Entering activity: Confirmation")
_PAGE_FIELDS = ("prewarm", "bwRender", "display", "grayLsb", "grayMsb", "grayDisplay", "cleanup", "total")
_LARGE_IMAGE_PIXELS = {"x3": 960 * 540 // 2, "x4": 800 * 480 // 2}
_SMALL_IMAGE_MAX_BYTES = 16 * 1024
_MEDIUM_IMAGE_MAX_BYTES = 128 * 1024


def _percentile(values: list[int], fraction: float) -> float:
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def _stats(values: list[int]) -> dict[str, int | float]:
    if not values:
        raise HardwareError("cannot summarize an empty calibration sample")
    median = statistics.median(values)
    return {
        "samples": len(values),
        "min": min(values),
        "p50": round(median, 3),
        "p90": round(_percentile(values, 0.9), 3),
        "mad": round(statistics.median(abs(value - median) for value in values), 3),
        "max": max(values),
    }


def _cache_clear_metrics(serial: str) -> dict[str, object]:
    durations = [int(duration_us) for duration_us in _CACHE_CLEAR_RE.findall(serial)]
    if not durations:
        return {}
    samples = [
        {
            "durationUs": int(duration_us),
            "fileCount": int(file_count),
            "directoryCount": int(directory_count),
            "fileBytes": int(file_bytes),
        }
        for duration_us, file_count, directory_count, file_bytes in _CACHE_CLEAR_POPULATION_RE.findall(serial)
    ]
    return {
        "cacheClearUs": _stats(durations),
        **({"cacheClearSamples": samples} if samples else {}),
    }


def _load_run(path: Path) -> tuple[dict[str, object], str]:
    try:
        manifest_value: object = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
        serial = (path / "serial.log").read_text(encoding="utf-8")
    except (OSError, json.JSONDecodeError) as error:
        raise HardwareError(f"cannot read calibration run {path}: {error}") from error
    if not isinstance(manifest_value, dict):
        raise HardwareError(f"calibration manifest is not an object: {path}")
    return cast(dict[str, object], manifest_value), serial


def _object_dict(value: object) -> dict[str, object] | None:
    return cast(dict[str, object], value) if isinstance(value, dict) else None


def _manifest_string(manifest: dict[str, object], key: str, path: Path) -> str:
    value = manifest.get(key)
    if not isinstance(value, str):
        raise HardwareError(f"calibration manifest has no string {key}: {path}")
    return value


def _cadence_evidence(source: dict[str, object]) -> dict[str, object]:
    return {
        key: source[key]
        for key in (
            "captureFps",
            "decodedSourceFrames",
            "observedSourceFps",
            "sourceFrameIntervalMs",
            "frameQuantizationMs",
        )
        if key in source
    }


def _source(path: Path, manifest: dict[str, object]) -> dict[str, object]:
    camera = _object_dict(manifest.get("camera"))
    fps = camera.get("fps") if camera is not None else None
    return {
        "run": path.name,
        "status": manifest.get("status"),
        "startedUtc": manifest.get("startedUtc"),
        "scenarioSha256": manifest.get("scenarioSha256"),
        "captureFps": fps,
        **(
            {
                key: camera[key]
                for key in ("decodedSourceFrames", "observedSourceFps", "sourceFrameIntervalMs")
                if key in camera
            }
            if camera is not None
            else {}
        ),
        "firmwareVersion": manifest.get("firmwareVersion"),
    }


def _optical_waveform(path: Path) -> tuple[dict[str, object], Path, dict[str, object]]:
    try:
        value: object = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HardwareError(f"cannot read optical waveform {path}: {error}") from error
    if not isinstance(value, dict):
        raise HardwareError(f"optical waveform is not an object: {path}")
    analysis = cast(dict[str, object], value)
    run = path.parent
    manifest, _ = _load_run(run)
    if analysis.get("schemaVersion") != 1 or analysis.get("sourceRun") != run.name:
        raise HardwareError("optical waveform source does not match its calibration run")
    if analysis.get("alignment") != "ffmpeg-first-frame-decoded-host-ns-v1":
        raise HardwareError("optical waveform must use first-decoded-frame alignment")
    video = run / "capture.mp4"
    try:
        video_hash = hashlib.sha256(video.read_bytes()).hexdigest()
    except OSError as error:
        raise HardwareError(f"cannot read optical capture {video}: {error}") from error
    if analysis.get("videoSha256") != video_hash:
        raise HardwareError("optical waveform video hash does not match its capture")
    modes = _object_dict(analysis.get("modes"))
    if modes is None:
        raise HardwareError("optical waveform has no modes")
    compact_modes: dict[str, object] = {}
    for mode in ("fast", "half", "full"):
        mode_value = _object_dict(modes.get(mode))
        by_target = _object_dict(mode_value.get("byTarget")) if mode_value is not None else None
        if mode_value is None or by_target is None:
            raise HardwareError(f"optical waveform has no {mode} target summaries")
        targets: dict[str, object] = {}
        for target in ("black", "white"):
            target_value = _object_dict(by_target.get(target))
            if target_value is None:
                raise HardwareError(f"optical waveform has no {mode}/{target} summary")
            targets[target] = target_value
        compact: dict[str, object] = {"byTarget": targets}
        if mode == "full":
            pulse = _object_dict(mode_value.get("pulseIntervalMs"))
            if pulse is None:
                raise HardwareError("optical waveform has no full-refresh pulse interval")
            compact["pulseIntervalMs"] = pulse
        compact_modes[mode] = compact
    return {
        "sourceRun": analysis["sourceRun"],
        "videoSha256": video_hash,
        **_cadence_evidence(analysis),
        "crop": analysis.get("crop"),
        "changeThreshold": analysis.get("changeThreshold"),
        "alignment": analysis["alignment"],
        "modes": compact_modes,
        "limitations": analysis.get("limitations"),
    }, run, manifest


def _grayscale_waveform(path: Path) -> tuple[dict[str, object], Path, dict[str, object]]:
    try:
        value: object = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HardwareError(f"cannot read grayscale waveform {path}: {error}") from error
    if not isinstance(value, dict):
        raise HardwareError(f"grayscale waveform is not an object: {path}")
    analysis = cast(dict[str, object], value)
    run = path.parent
    manifest, _ = _load_run(run)
    if (
        analysis.get("schemaVersion") != 1
        or analysis.get("kind") != "x4-differential-grayscale"
        or analysis.get("sourceRun") != run.name
    ):
        raise HardwareError("grayscale waveform source does not match its calibration run")
    if analysis.get("alignment") != "ffmpeg-first-frame-decoded-host-ns-v1":
        raise HardwareError("grayscale waveform must use first-decoded-frame alignment")
    video = run / "capture.mp4"
    try:
        video_hash = hashlib.sha256(video.read_bytes()).hexdigest()
    except OSError as error:
        raise HardwareError(f"cannot read grayscale capture {video}: {error}") from error
    if analysis.get("videoSha256") != video_hash:
        raise HardwareError("grayscale waveform video hash does not match its capture")
    primaries = _object_dict(analysis.get("primaries"))
    if primaries is None:
        raise HardwareError("grayscale waveform has no primary summaries")
    compact_primaries: dict[str, object] = {}
    for primary in ("fast", "half"):
        primary_value = _object_dict(primaries.get(primary))
        by_target = _object_dict(primary_value.get("byTarget")) if primary_value is not None else None
        operation = _object_dict(primary_value.get("operationUs")) if primary_value is not None else None
        if primary_value is None or by_target is None or operation is None:
            raise HardwareError(f"grayscale waveform has no {primary} summary")
        detected = primary_value.get("visibleChangeDetected")
        if not isinstance(detected, bool):
            detected = bool(by_target)
        if not detected:
            if by_target:
                raise HardwareError(f"grayscale waveform marks {primary} undetected but retains target summaries")
            compact_primaries[primary] = {
                "operationUs": operation,
                "visibleChangeDetected": False,
                "byTarget": {},
            }
            continue
        targets: dict[str, object] = {}
        for target in ("light", "dark"):
            target_value = _object_dict(by_target.get(target))
            if target_value is None:
                raise HardwareError(f"grayscale waveform has no {primary}/{target} summary")
            targets[target] = target_value
        compact_primaries[primary] = {
            "operationUs": operation,
            "visibleChangeDetected": True,
            "byTarget": targets,
        }
    return {
        "sourceRun": analysis["sourceRun"],
        "videoSha256": video_hash,
        **_cadence_evidence(analysis),
        "crop": analysis.get("crop"),
        "changeThreshold": analysis.get("changeThreshold"),
        "analysisWindowMs": analysis.get("analysisWindowMs"),
        "alignment": analysis["alignment"],
        "primaries": compact_primaries,
        "limitations": analysis.get("limitations"),
    }, run, manifest


def _reader_half_waveform(path: Path) -> tuple[dict[str, object], Path, dict[str, object]]:
    try:
        value: object = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HardwareError(f"cannot read reader half waveform {path}: {error}") from error
    if not isinstance(value, dict):
        raise HardwareError(f"reader half waveform is not an object: {path}")
    analysis = cast(dict[str, object], value)
    run = path.parent
    manifest, _ = _load_run(run)
    if (
        analysis.get("schemaVersion") != 1
        or analysis.get("kind") != "x4-reader-half-inversion"
        or analysis.get("sourceRun") != run.name
    ):
        raise HardwareError("reader half waveform source does not match its calibration run")
    if analysis.get("alignment") != "ffmpeg-first-frame-decoded-host-ns-v1":
        raise HardwareError("reader half waveform must use first-decoded-frame alignment")
    video = run / "capture.mp4"
    try:
        video_hash = hashlib.sha256(video.read_bytes()).hexdigest()
    except OSError as error:
        raise HardwareError(f"cannot read reader half capture {video}: {error}") from error
    if analysis.get("videoSha256") != video_hash:
        raise HardwareError("reader half waveform video hash does not match its capture")
    inversion = _object_dict(analysis.get("invertedTargetStartMs"))
    settle = _object_dict(analysis.get("targetSettleMs"))
    if inversion is None or settle is None:
        raise HardwareError("reader half waveform has no inversion/settling summaries")
    return {
        "sourceRun": analysis["sourceRun"],
        "videoSha256": video_hash,
        **_cadence_evidence(analysis),
        "crop": analysis.get("crop"),
        "changeThreshold": analysis.get("changeThreshold"),
        "analysisWindowMs": analysis.get("analysisWindowMs"),
        "alignment": analysis["alignment"],
        "invertedTargetStartMs": inversion,
        "targetSettleMs": settle,
        "limitations": analysis.get("limitations"),
    }, run, manifest


def _reader_direct_waveform(
    path: Path, expected_kind: str, label: str
) -> tuple[dict[str, object], Path, dict[str, object]]:
    try:
        value: object = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HardwareError(f"cannot read reader {label} waveform {path}: {error}") from error
    if not isinstance(value, dict):
        raise HardwareError(f"reader {label} waveform is not an object: {path}")
    analysis = cast(dict[str, object], value)
    run = path.parent
    manifest, _ = _load_run(run)
    if (
        analysis.get("schemaVersion") != 1
        or analysis.get("kind") != expected_kind
        or analysis.get("sourceRun") != run.name
    ):
        raise HardwareError(f"reader {label} waveform source does not match its calibration run")
    if analysis.get("alignment") != "ffmpeg-first-frame-decoded-host-ns-v1":
        raise HardwareError(f"reader {label} waveform must use first-decoded-frame alignment")
    video = run / "capture.mp4"
    try:
        video_hash = hashlib.sha256(video.read_bytes()).hexdigest()
    except OSError as error:
        raise HardwareError(f"cannot read reader {label} capture {video}: {error}") from error
    if analysis.get("videoSha256") != video_hash:
        raise HardwareError(f"reader {label} waveform video hash does not match its capture")
    start = _object_dict(analysis.get("transitionStartMs"))
    end = _object_dict(analysis.get("transitionEndMs"))
    if start is None or end is None:
        raise HardwareError(f"reader {label} waveform has no transition summaries")
    compact: dict[str, object] = {
        "sourceRun": analysis["sourceRun"],
        "videoSha256": video_hash,
        **_cadence_evidence(analysis),
        "crop": analysis.get("crop"),
        "changeThreshold": analysis.get("changeThreshold"),
        "analysisWindowMs": analysis.get("analysisWindowMs"),
        "alignment": analysis["alignment"],
        "transitionStartMs": start,
        "transitionEndMs": end,
        "dominantChangeMs": analysis.get("dominantChangeMs"),
        "limitations": analysis.get("limitations"),
    }
    if expected_kind == "x4-reader-image-fast-transition":
        image_pixels = analysis.get("sourceImageAreaPixelsMin")
        image_fraction = analysis.get("sourceImageAreaFractionMin")
        dark_percent = analysis.get("targetDarkPixelPercentMin")
        if (
            not isinstance(image_pixels, int)
            or image_pixels <= 0
            or not isinstance(image_fraction, int | float)
            or not 0 < image_fraction <= 1
            or not isinstance(dark_percent, int)
            or not 1 <= dark_percent <= 100
        ):
            raise HardwareError("reader image-fast waveform has invalid content selectors")
        compact.update(
            {
                "sourceImageAreaPixelsMin": image_pixels,
                "sourceImageAreaFractionMin": image_fraction,
                "targetDarkPixelPercentMin": dark_percent,
            }
        )
    return compact, run, manifest


def _reader_fast_waveform(path: Path) -> tuple[dict[str, object], Path, dict[str, object]]:
    return _reader_direct_waveform(path, "x4-reader-fast-transition", "fast")


def _reader_image_fast_waveform(path: Path) -> tuple[dict[str, object], Path, dict[str, object]]:
    return _reader_direct_waveform(path, "x4-reader-image-fast-transition", "image-fast")


def _page_metrics(serial: str) -> dict[str, object]:
    samples: list[dict[str, int]] = []
    waits: list[int] = []
    for line in serial.splitlines():
        wait = _WAIT_RE.search(line)
        if wait:
            waits.append(int(wait.group(1)))
        page = _PAGE_RE.search(line)
        if not page:
            continue
        sample: dict[str, int] = {}
        for field, value in zip(_PAGE_FIELDS, map(int, page.groups()), strict=True):
            sample[field] = value
        if len(waits) >= 2:
            sample["primaryBusy"] = waits[-2]
            sample["grayscaleBusy"] = waits[-1]
        waits.clear()
        samples.append(sample)
    if not samples:
        raise HardwareError("page calibration run contains no tiled page timings")

    def summarize(selected: list[dict[str, int]]) -> dict[str, object]:
        fields = tuple(selected[0])
        return {field: _stats([sample[field] for sample in selected]) for field in fields}

    fast = [sample for sample in samples if sample.get("primaryBusy", 0) < 1000]
    half = [sample for sample in samples if sample.get("primaryBusy", 0) >= 1000]
    result: dict[str, object] = {"all": summarize(samples)}
    if fast:
        result["fastPrimary"] = summarize(fast)
    if half:
        result["halfPrimary"] = summarize(half)
    return result


def _panel_metrics(serial: str) -> dict[str, object]:
    result: dict[str, object] = {}
    for mode in ("fast", "half", "full"):
        durations = [int(duration) for found_mode, duration in _PANEL_RE.findall(serial) if found_mode == mode]
        if not durations:
            raise HardwareError(f"panel calibration run contains no {mode} samples")
        marker = f"CAL:MARK:panel-{mode}:"
        if marker not in serial:
            raise HardwareError(f"panel calibration run contains no {mode} marker")
        segment = serial.split(marker, 1)[1].split("CAL:BENCH-END:PANEL", 1)[0]
        waits = [int(value) for value in _WAIT_RE.findall(segment)][: len(durations)]
        mode_result: dict[str, object] = {"operationUs": _stats(durations)}
        if waits:
            mode_result["controllerBusyMs"] = _stats(waits)
        result[mode] = mode_result
    return result


def _storage_metrics(runs: list[tuple[Path, str]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for path, serial in runs:
        completed = {int(byte_count): int(iterations) for byte_count, iterations in _SD_END_RE.findall(serial)}
        grouped: dict[tuple[int, str], dict[str, list[int]]] = {}
        for operation, byte_count_text, open_text, io_text, close_text in _SD_RE.findall(serial):
            byte_count = int(byte_count_text)
            if byte_count not in completed:
                continue
            key = (byte_count, operation.lower())
            sample = grouped.setdefault(key, {"openUs": [], "ioUs": [], "closeUs": [], "totalUs": []})
            values = (int(open_text), int(io_text), int(close_text))
            for field, value in zip(("openUs", "ioUs", "closeUs"), values, strict=True):
                sample[field].append(value)
            sample["totalUs"].append(sum(values))
        for (byte_count, operation), sample in grouped.items():
            expected = completed[byte_count]
            if len(sample["totalUs"]) != expected:
                raise HardwareError(f"incomplete {byte_count}-byte {operation} benchmark in {path}")
            size_result = result.setdefault(str(byte_count), {})
            if not isinstance(size_result, dict) or operation in size_result:
                raise HardwareError(f"duplicate {byte_count}-byte {operation} benchmark")
            io_median = statistics.median(sample["ioUs"])
            size_result[operation] = {
                **{field: _stats(values) for field, values in sample.items()},
                "ioMiBPerSecondP50": round(byte_count / io_median * 1_000_000 / (1024 * 1024), 6),
            }
    if not result:
        raise HardwareError("SD calibration runs contain no completed benchmarks")
    return dict(sorted(result.items(), key=lambda item: int(item[0])))


def _directory_metrics(path: Path, serial: str) -> dict[str, object]:
    completed = _DIRECTORY_END_RE.search(serial)
    samples = [tuple(map(int, values)) for values in _DIRECTORY_RE.findall(serial)]
    if completed is None or len(samples) != int(completed.group(1)):
        raise HardwareError(f"directory calibration run is incomplete: {path}")
    entry_counts = [sample[0] for sample in samples]
    if len(set(entry_counts)) != 1:
        raise HardwareError(f"directory calibration entry count changed during the run: {path}")
    return {
        "sourceRun": path.name,
        "entries": entry_counts[0],
        "rootOpenUs": _stats([sample[1] for sample in samples]),
        "enumerationUs": _stats([sample[2] for sample in samples]),
        "perNextUs": _stats([round(sample[2] / (sample[0] + 1)) for sample in samples]),
        "entryCloseUs": _stats([sample[3] for sample in samples]),
        "rootCloseUs": _stats([sample[4] for sample in samples]),
        "totalUs": _stats([sample[5] for sample in samples]),
    }


def _timed_pairs(serial: str, start_text: str, end_text: str) -> list[int]:
    started: int | None = None
    durations: list[int] = []
    for line in serial.splitlines():
        timestamp = _TIMED_LOG_RE.match(line)
        if timestamp is None:
            continue
        time_ms = int(timestamp.group(1))
        if start_text in line:
            started = time_ms
        elif end_text in line and started is not None:
            durations.append(time_ms - started)
            started = None
    return durations


def _first_timed_after(serial: str, text: str, after_ms: int) -> int | None:
    for line in serial.splitlines():
        timestamp = _TIMED_LOG_RE.match(line)
        if timestamp is not None and int(timestamp.group(1)) >= after_ms and text in line:
            return int(timestamp.group(1))
    return None


def _warm_repeat_metrics(serial: str, markers: dict[str, int], name: str) -> dict[str, object]:
    open_to_ready: list[int] = []
    metadata_load: list[int] = []
    metadata_to_page: list[int] = []
    prefix = f"{name}-warm-open-"
    for marker in sorted(marker for marker in markers if marker.startswith(prefix)):
        suffix = marker[len(prefix) :]
        ready = f"{name}-warm-ready-{suffix}"
        if not suffix.isdigit() or ready not in markers:
            continue
        start_text = f"CAL:MARK:{marker}:"
        end_text = f"CAL:MARK:{ready}:"
        sample_serial = serial.split(start_text, 1)[1].split(end_text, 1)[0]
        open_to_ready.append(markers[ready] - markers[marker])
        metadata_load.extend(_timed_pairs(sample_serial, "[DBG] [EBP] Loading ePub:", "[DBG] [EBP] Loaded ePub:"))
        metadata_start = _first_timed_after(sample_serial, "[DBG] [EBP] Loading ePub:", 0)
        page_start = _first_timed_after(sample_serial, "[DBG] [ERS] Loading file:", 0)
        if metadata_start is not None and page_start is not None:
            metadata_to_page.append(page_start - metadata_start)
    result: dict[str, object] = {}
    if open_to_ready:
        result["openToReadyMs"] = _stats(open_to_ready)
    if metadata_load:
        result["cachedMetadataLoadMs"] = _stats(metadata_load)
    if metadata_to_page:
        result["metadataStartToPageLoadMs"] = _stats(metadata_to_page)
    return result


def _thumbnail_metrics(serial: str) -> dict[str, object]:
    samples: list[dict[str, int | str]] = []
    current: dict[str, int | str] | None = None
    for line in serial.splitlines():
        timestamp = _TIMED_LOG_RE.match(line)
        start = _THUMB_START_RE.search(line)
        if timestamp is not None and start is not None:
            current = {"startedMs": int(timestamp.group(1)), "format": start.group(1).lower()}
            continue
        if current is None:
            continue
        if decompressed := _DECOMPRESSED_RE.search(line):
            current["archiveCompressedBytes"] = int(decompressed.group(1))
            current["imageBytes"] = int(decompressed.group(2))
        if dimensions := _JPEG_DIMENSIONS_RE.search(line):
            current["sourceWidth"] = int(dimensions.group(1))
            current["sourceHeight"] = int(dimensions.group(2))
        if dimensions := _PNG_DIMENSIONS_RE.search(line):
            current["sourceWidth"] = int(dimensions.group(1))
            current["sourceHeight"] = int(dimensions.group(2))
            current["bitDepth"] = int(dimensions.group(3))
            current["colorType"] = int(dimensions.group(4))
        if target := _THUMB_TARGET_RE.search(line):
            current["targetWidth"] = int(target.group(1))
            current["targetHeight"] = int(target.group(2))
            if timestamp is not None:
                current["conversionStartedMs"] = int(timestamp.group(1))
        if timestamp is not None and _THUMB_END_RE.search(line):
            current["generationMs"] = int(timestamp.group(1)) - cast(int, current["startedMs"])
            if "conversionStartedMs" in current:
                current["preparationMs"] = cast(int, current["conversionStartedMs"]) - cast(int, current["startedMs"])
                current["conversionMs"] = int(timestamp.group(1)) - cast(int, current["conversionStartedMs"])
            samples.append(current)
            current = None
    if not samples:
        return {}
    formats = sorted({cast(str, sample["format"]) for sample in samples})
    fields = (
        "archiveCompressedBytes",
        "imageBytes",
        "sourceWidth",
        "sourceHeight",
        "targetWidth",
        "targetHeight",
        "bitDepth",
        "colorType",
    )
    return {
        "thumbnailGenerationMs": _stats([cast(int, sample["generationMs"]) for sample in samples]),
        **(
            {"thumbnailConversionMs": _stats([cast(int, sample["conversionMs"]) for sample in samples])}
            if all("conversionMs" in sample for sample in samples)
            else {}
        ),
        **(
            {"thumbnailPreparationMs": _stats([cast(int, sample["preparationMs"]) for sample in samples])}
            if all("preparationMs" in sample for sample in samples)
            else {}
        ),
        "thumbnailInput": {
            "format": formats[0] if len(formats) == 1 else formats,
            **{
                field: _stats([cast(int, sample[field]) for sample in samples if field in sample])
                for field in fields
                if any(field in sample for sample in samples)
            },
        },
    }


def _jpeg_thumbnail_model(workloads: dict[str, object]) -> dict[str, object]:
    samples: list[tuple[int, int]] = []

    def collect(value: object) -> None:
        mapping = _object_dict(value)
        if mapping is None:
            return
        generation = _object_dict(mapping.get("thumbnailGenerationMs"))
        thumbnail_input = _object_dict(mapping.get("thumbnailInput"))
        if generation is not None and thumbnail_input is not None and thumbnail_input.get("format") == "jpg":
            image_bytes = _object_dict(thumbnail_input.get("imageBytes"))
            duration = generation.get("p50")
            byte_count = image_bytes.get("p50") if image_bytes is not None else None
            if isinstance(duration, (int, float)) and isinstance(byte_count, (int, float)):
                samples.append((round(byte_count), round(duration)))
        for child in mapping.values():
            collect(child)

    collect(workloads)
    samples = sorted(set(samples))
    if len(samples) < 2:
        return {}
    mean_bytes = sum(byte_count for byte_count, _ in samples) / len(samples)
    mean_ms = sum(duration for _, duration in samples) / len(samples)
    denominator = sum((byte_count - mean_bytes) ** 2 for byte_count, _ in samples)
    if denominator == 0:
        return {}
    milliseconds_per_byte = (
        sum((byte_count - mean_bytes) * (duration - mean_ms) for byte_count, duration in samples) / denominator
    )
    intercept_ms = max(0.0, mean_ms - milliseconds_per_byte * mean_bytes)
    fitted = [intercept_ms + milliseconds_per_byte * byte_count for byte_count, _ in samples]
    errors = [abs(predicted - duration) / duration * 100 for predicted, (_, duration) in zip(fitted, samples)]
    return {
        "kind": "linear-decoded-image-bytes-v1",
        "interceptUs": round(intercept_ms * 1000),
        "nanosecondsPerImageByte": round(milliseconds_per_byte * 1_000_000),
        "maxAbsoluteRelativeErrorPercent": round(max(errors), 3),
        "samples": [
            {"imageBytes": byte_count, "generationMs": duration}
            for byte_count, duration in samples
        ],
    }


def _png_thumbnail_model(workloads: dict[str, object]) -> dict[str, object]:
    samples: set[tuple[int, int, int, int, int, int, int, int, int]] = set()

    def collect(value: object) -> None:
        mapping = _object_dict(value)
        if mapping is None:
            return
        preparation = _object_dict(mapping.get("thumbnailPreparationMs"))
        conversion = _object_dict(mapping.get("thumbnailConversionMs"))
        thumbnail_input = _object_dict(mapping.get("thumbnailInput"))
        if (
            preparation is not None
            and conversion is not None
            and thumbnail_input is not None
            and thumbnail_input.get("format") == "png"
        ):
            values: list[int] = []
            for field in (
                "sourceWidth",
                "sourceHeight",
                "targetWidth",
                "targetHeight",
                "bitDepth",
                "colorType",
                "imageBytes",
            ):
                statistics_value = _object_dict(thumbnail_input.get(field))
                p50 = statistics_value.get("p50") if statistics_value is not None else None
                if not isinstance(p50, (int, float)):
                    break
                values.append(round(p50))
            else:
                preparation_duration = preparation.get("p50")
                conversion_duration = conversion.get("p50")
                if (
                    isinstance(preparation_duration, (int, float))
                    and isinstance(conversion_duration, (int, float))
                    and len(values) == 7
                ):
                    samples.add(
                        (
                            values[0],
                            values[1],
                            values[2],
                            values[3],
                            values[4],
                            values[5],
                            values[6],
                            round(preparation_duration),
                            round(conversion_duration),
                        )
                    )
        for child in mapping.values():
            collect(child)

    collect(workloads)
    ordered = sorted(samples, key=lambda sample: sample[0] * sample[1])
    if len(ordered) < 3:
        return {}
    targets = {(sample[2], sample[3]) for sample in ordered}
    formats = {(sample[4], sample[5]) for sample in ordered}
    if len(targets) != 1 or formats != {(8, 2)}:
        return {}
    pixels = [width * height for width, height, _, _, _, _, _, _, _ in ordered]
    image_bytes = [byte_count for _, _, _, _, _, _, byte_count, _, _ in ordered]
    preparation_durations = [duration for _, _, _, _, _, _, _, duration, _ in ordered]
    conversion_durations = [duration for _, _, _, _, _, _, _, _, duration in ordered]

    def fit(inputs: list[int], durations: list[int]) -> tuple[float, float, float]:
        mean_input = sum(inputs) / len(inputs)
        mean_duration = sum(durations) / len(durations)
        denominator = sum((value - mean_input) ** 2 for value in inputs)
        if denominator == 0:
            return 0.0, 0.0, 100.0
        slope = sum(
            (value - mean_input) * (duration - mean_duration) for value, duration in zip(inputs, durations)
        ) / denominator
        intercept = max(0.0, mean_duration - slope * mean_input)
        fitted = [intercept + slope * value for value in inputs]
        errors = [
            abs(predicted - duration) / duration * 100 for predicted, duration in zip(fitted, durations)
        ]
        return intercept, slope, max(errors)

    preparation_intercept_ms, preparation_ms_per_byte, preparation_error = fit(
        image_bytes, preparation_durations
    )
    conversion_intercept_ms, conversion_ms_per_pixel, conversion_error = fit(pixels, conversion_durations)
    if preparation_ms_per_byte <= 0 or conversion_ms_per_pixel <= 0:
        return {}
    target_width, target_height = next(iter(targets))
    return {
        "kind": "png-two-phase-v1",
        "preparationInterceptUs": round(preparation_intercept_ms * 1000),
        "preparationNanosecondsPerImageByte": round(preparation_ms_per_byte * 1_000_000),
        "conversionInterceptUs": round(conversion_intercept_ms * 1000),
        "conversionNanosecondsPerSourcePixel": round(conversion_ms_per_pixel * 1_000_000),
        "minImageBytes": min(image_bytes),
        "maxImageBytes": max(image_bytes),
        "minSourcePixels": min(pixels),
        "maxSourcePixels": max(pixels),
        "targetWidth": target_width,
        "targetHeight": target_height,
        "bitDepth": 8,
        "colorType": 2,
        "preparationMaxAbsoluteRelativeErrorPercent": round(preparation_error, 3),
        "conversionMaxAbsoluteRelativeErrorPercent": round(conversion_error, 3),
        "samples": [
            {
                "sourceWidth": width,
                "sourceHeight": height,
                "imageBytes": byte_count,
                "preparationMs": preparation_duration,
                "conversionMs": conversion_duration,
            }
            for width, height, _, _, _, _, byte_count, preparation_duration, conversion_duration in ordered
        ],
    }


def _cache_clear_model(workloads: dict[str, object]) -> dict[str, object]:
    samples: list[dict[str, int]] = []

    def collect(value: object) -> None:
        mapping = _object_dict(value)
        if mapping is None:
            if isinstance(value, list):
                for child in cast(list[object], value):
                    collect(child)
            return
        raw_samples = mapping.get("cacheClearSamples")
        if isinstance(raw_samples, list):
            for raw_sample in cast(list[object], raw_samples):
                sample = _object_dict(raw_sample)
                if sample is None:
                    continue
                duration = sample.get("durationUs")
                file_count = sample.get("fileCount")
                directory_count = sample.get("directoryCount")
                file_bytes = sample.get("fileBytes")
                if (
                    isinstance(duration, int)
                    and isinstance(file_count, int)
                    and isinstance(directory_count, int)
                    and isinstance(file_bytes, int)
                    and duration >= 0
                    and file_count >= 0
                    and directory_count >= 0
                    and file_bytes >= 0
                ):
                    samples.append(
                        {
                            "durationUs": duration,
                            "fileCount": file_count,
                            "directoryCount": directory_count,
                            "fileBytes": file_bytes,
                        }
                    )
        for child in mapping.values():
            collect(child)

    collect(workloads)
    no_cache = [sample["durationUs"] for sample in samples if sample["fileCount"] == 0]
    populated = [sample for sample in samples if sample["fileCount"] > 0]
    if not no_cache or len(populated) < 2 or len({sample["fileCount"] for sample in populated}) < 2:
        return {}

    mean_files = sum(sample["fileCount"] for sample in populated) / len(populated)
    mean_duration = sum(sample["durationUs"] for sample in populated) / len(populated)
    denominator = sum((sample["fileCount"] - mean_files) ** 2 for sample in populated)
    if denominator == 0:
        return {}
    fitted_per_file_us = (
        sum(
            (sample["fileCount"] - mean_files) * (sample["durationUs"] - mean_duration)
            for sample in populated
        )
        / denominator
    )
    per_file_us = round(fitted_per_file_us)
    intercept_us = round(mean_duration - fitted_per_file_us * mean_files)
    if intercept_us <= 0 or per_file_us <= 0:
        return {}
    errors = [
        abs(intercept_us + per_file_us * sample["fileCount"] - sample["durationUs"])
        / sample["durationUs"]
        * 100
        for sample in populated
    ]
    return {
        "kind": "linear-file-count-v1",
        "noCacheUs": _stats(no_cache),
        "populated": {
            "sampleCount": len(populated),
            "interceptUs": intercept_us,
            "perFileUs": per_file_us,
            "maxAbsoluteRelativeErrorPercent": round(max(errors), 3),
            "directoryCounts": sorted({sample["directoryCount"] for sample in populated}),
        },
        "samples": sorted(
            samples,
            key=lambda sample: (
                sample["fileCount"],
                sample["directoryCount"],
                sample["fileBytes"],
                sample["durationUs"],
            ),
        ),
    }


def _has_cache_population_samples(value: object) -> bool:
    mapping = _object_dict(value)
    if mapping is not None:
        if isinstance(mapping.get("cacheClearSamples"), list):
            return True
        return any(_has_cache_population_samples(child) for child in mapping.values())
    if isinstance(value, list):
        return any(_has_cache_population_samples(child) for child in cast(list[object], value))
    return False


def _exact_image_decode(workloads: dict[str, object]) -> list[dict[str, int]]:
    records: dict[tuple[int, int, int], dict[str, int]] = {}

    def collect(value: object) -> None:
        mapping = _object_dict(value)
        if mapping is None:
            if isinstance(value, list):
                for child in cast(list[object], value):
                    collect(child)
            return
        raw_samples = mapping.get("decodeAndCacheSamples")
        if isinstance(raw_samples, list):
            for raw_sample in cast(list[object], raw_samples):
                sample = _object_dict(raw_sample)
                if sample is None:
                    continue
                fields = (
                    sample.get("imageBytes"),
                    sample.get("width"),
                    sample.get("height"),
                    sample.get("generationMs"),
                )
                if all(isinstance(field, int) for field in fields):
                    image_bytes, width, height, duration = cast(tuple[int, int, int, int], fields)
                    key = (image_bytes, width, height)
                    record = {
                        "sourceBytes": image_bytes,
                        "width": width,
                        "height": height,
                        "durationMs": duration,
                    }
                    previous = records.get(key)
                    if previous is not None and previous != record:
                        raise HardwareError(f"conflicting exact image decode measurements for {key}")
                    records[key] = record
        for child in mapping.values():
            collect(child)

    collect(workloads)
    return [records[key] for key in sorted(records)]


def _exact_image_preparation(workloads: dict[str, object]) -> list[dict[str, int]]:
    records: dict[int, dict[str, int]] = {}

    def collect(value: object) -> None:
        mapping = _object_dict(value)
        if mapping is None:
            if isinstance(value, list):
                for child in cast(list[object], value):
                    collect(child)
            return
        raw_samples = mapping.get("prepareSamples")
        if isinstance(raw_samples, list):
            for raw_sample in cast(list[object], raw_samples):
                sample = _object_dict(raw_sample)
                if sample is None:
                    continue
                source_bytes = sample.get("imageBytes")
                duration = sample.get("generationMs")
                if isinstance(source_bytes, int) and isinstance(duration, int):
                    record = {"sourceBytes": source_bytes, "durationMs": duration}
                    previous = records.get(source_bytes)
                    if previous is not None and previous != record:
                        raise HardwareError(f"conflicting exact image preparation measurements for {source_bytes}")
                    records[source_bytes] = record
        for child in mapping.values():
            collect(child)

    collect(workloads)
    return [records[key] for key in sorted(records)]


def _duration_model(workloads: dict[str, object], sample_key: str) -> dict[str, int | float]:
    durations: list[int] = []

    def collect(value: object) -> None:
        mapping = _object_dict(value)
        if mapping is None:
            if isinstance(value, list):
                for child in cast(list[object], value):
                    collect(child)
            return
        samples = mapping.get(sample_key)
        if isinstance(samples, list):
            durations.extend(sample for sample in cast(list[object], samples) if isinstance(sample, int))
        for child in mapping.values():
            collect(child)

    collect(workloads)
    return _stats(durations) if durations else {}


def _exact_indexing_by_path(workloads: dict[str, object]) -> list[dict[str, object]]:
    observations: dict[str, list[dict[str, int]]] = {}
    fields = ("opfPassMs", "tocPassMs", "bookBinMs", "totalIndexingMs", "postIndexLoadMs")
    count_fields = ("manifestItems", "spineItems", "tocItems")

    def collect(value: object) -> None:
        mapping = _object_dict(value)
        if mapping is None:
            if isinstance(value, list):
                for child in cast(list[object], value):
                    collect(child)
            return
        path = mapping.get("epubPath")
        indexing = _object_dict(mapping.get("indexing"))
        if (
            isinstance(path, str)
            and indexing is not None
            and all(isinstance(indexing.get(field), int) for field in fields)
        ):
            observations.setdefault(path, []).append(
                {
                    field: cast(int, indexing[field])
                    for field in (*fields, *count_fields)
                    if isinstance(indexing.get(field), int)
                }
            )
        repeats = _object_dict(mapping.get("indexingRepeats"))
        raw_groups = repeats.get("groups") if repeats is not None else None
        groups = cast(list[object], raw_groups) if isinstance(raw_groups, list) else ([repeats] if repeats else [])
        for raw_group in groups:
            group = _object_dict(raw_group)
            repeat_path = group.get("epubPath") if group is not None else None
            repeat_samples = group.get("samples") if group is not None else None
            if not isinstance(repeat_path, str) or not isinstance(repeat_samples, list):
                raise HardwareError("repeated indexing group is incomplete")
            for raw_sample in cast(list[object], repeat_samples):
                sample = _object_dict(raw_sample)
                if sample is None or not all(isinstance(sample.get(field), int) for field in fields):
                    raise HardwareError(f"incomplete repeated indexing measurement for {repeat_path}")
                observations.setdefault(repeat_path, []).append(
                    {
                        field: cast(int, sample[field])
                        for field in (*fields, *count_fields, "coldOpenToReadyMs")
                        if isinstance(sample.get(field), int)
                    }
                )
        for child in mapping.values():
            collect(child)

    collect(workloads)
    records: list[dict[str, object]] = []
    for path in sorted(observations):
        samples = observations[path]
        phase_statistics = {field: _stats([sample[field] for sample in samples]) for field in fields}
        record: dict[str, object] = {
            "path": path,
            "sampleCount": len(samples),
            "phaseStatistics": phase_statistics,
            **{field: int(cast(float, phase_statistics[field]["p50"]) + 0.5) for field in fields},
        }
        phase_sum = sum(cast(int, record[field]) for field in ("opfPassMs", "tocPassMs", "bookBinMs"))
        record["totalIndexingMs"] = max(cast(int, record["totalIndexingMs"]), phase_sum)
        record["runtimePhaseSumMs"] = phase_sum
        for field in count_fields:
            counts = {sample[field] for sample in samples if field in sample}
            if len(counts) > 1:
                raise HardwareError(f"indexing item count changed across measurements for {path}: {field}")
            if counts:
                record[field] = next(iter(counts))
        cold_open_samples = [sample["coldOpenToReadyMs"] for sample in samples if "coldOpenToReadyMs" in sample]
        if cold_open_samples:
            record["coldOpenToReadyMs"] = _stats(cold_open_samples)
        records.append(record)
    return records


def _exact_warm_by_path(workloads: dict[str, object]) -> list[dict[str, object]]:
    records: dict[str, dict[str, object]] = {}

    def collect(value: object) -> None:
        mapping = _object_dict(value)
        if mapping is None:
            if isinstance(value, list):
                for child in cast(list[object], value):
                    collect(child)
            return
        path = mapping.get("epubPath")
        warm = _object_dict(mapping.get("warm"))
        if isinstance(path, str) and warm is not None:
            cached = _object_dict(warm.get("cachedMetadataLoadMs"))
            page_load = _object_dict(warm.get("metadataStartToPageLoadMs"))
            if (
                cached is None
                or page_load is None
                or not isinstance(cached.get("p50"), (int, float))
                or not isinstance(page_load.get("p50"), (int, float))
            ):
                raise HardwareError(f"incomplete exact warm-open measurements for {path}")
            record: dict[str, object] = {
                "path": path,
                "cachedMetadataLoadMs": cached,
                "metadataStartToPageLoadMs": page_load,
            }
            previous = records.get(path)
            if previous is not None and previous != record:
                raise HardwareError(f"conflicting exact warm-open measurements for {path}")
            records[path] = record
        for child in mapping.values():
            collect(child)

    collect(workloads)
    return [records[path] for path in sorted(records)]


def _section_stream_samples(serial: str) -> list[int]:
    started_ms: int | None = None
    durations: list[int] = []
    for line in serial.splitlines():
        timestamp = _TIMED_LOG_RE.match(line)
        if timestamp is None:
            continue
        time_ms = int(timestamp.group(1))
        if "[ERS] Loading file:" in line:
            started_ms = time_ms
        elif "Streamed temp HTML" in line and started_ms is not None:
            durations.append(time_ms - started_ms)
            started_ms = None
    return durations


def _file_browser_activity_samples(serial: str) -> list[int]:
    started_ms: int | None = None
    durations: list[int] = []
    for line in serial.splitlines():
        timestamp = _TIMED_LOG_RE.match(line)
        if timestamp is None:
            continue
        time_ms = int(timestamp.group(1))
        if "[ACT] Entering activity: FileBrowser" in line:
            started_ms = time_ms
        elif started_ms is not None and "from clearScreen to displayBuffer" in line:
            durations.append(time_ms - started_ms)
            started_ms = None
    return durations


def _file_browser_render_samples(serial: str) -> list[int]:
    in_file_browser = False
    durations: list[int] = []
    for line in serial.splitlines():
        if "[ACT] Entering activity: FileBrowser" in line:
            in_file_browser = True
            continue
        if "[ACT] Exiting activity: FileBrowser" in line:
            in_file_browser = False
            continue
        render = _GFX_RENDER_MS_RE.search(line) if in_file_browser else None
        if render is not None:
            durations.append(int(render.group(1)))
    return durations


def _home_render_samples(serial: str) -> tuple[list[int], list[int]]:
    in_home = False
    render_index = 0
    first: list[int] = []
    subsequent: list[int] = []
    for line in serial.splitlines():
        if "[ACT] Entering activity: Home" in line:
            in_home = True
            render_index = 0
            continue
        if "[ACT] Exiting activity: Home" in line:
            in_home = False
            continue
        render = _GFX_RENDER_MS_RE.search(line) if in_home else None
        if render is None:
            continue
        (first if render_index == 0 else subsequent).append(int(render.group(1)))
        render_index += 1
    return first, subsequent


def _activity_render_samples(
    serial: str, activity_name: str, *, duration_limit_ms: int | None = None
) -> tuple[list[int], list[int]]:
    in_activity = False
    full_render_index = 0
    first: list[int] = []
    subsequent: list[int] = []
    for line in serial.splitlines():
        if f"[ACT] Entering activity: {activity_name}" in line:
            in_activity = True
            full_render_index = 0
            continue
        if f"[ACT] Exiting activity: {activity_name}" in line:
            in_activity = False
            continue
        render = _GFX_RENDER_MS_RE.search(line) if in_activity else None
        if render is None:
            continue
        duration_ms = int(render.group(1))
        if duration_limit_ms is not None and duration_ms >= duration_limit_ms:
            continue
        (first if full_render_index == 0 else subsequent).append(duration_ms)
        full_render_index += 1
    return first, subsequent


def _settings_render_samples(serial: str) -> tuple[list[int], list[int]]:
    return _activity_render_samples(serial, "Settings", duration_limit_ms=100)


def _calibration_activity_render_samples(segment: str, activity_name: str) -> list[int]:
    return [int(duration) for name, duration in _CAL_ACTIVITY_RENDER_RE.findall(segment) if name == activity_name]


def _home_control_metrics(serial: str) -> dict[str, object]:
    segment = serial.split("CAL:MARK:home-controls-start:", 1)[1].split("CAL:MARK:home-controls-ready:", 1)[0]
    pending: tuple[str, int] | None = None
    samples: list[dict[str, int | str]] = []
    for line in segment.splitlines():
        button = _BUTTON_DOWN_RE.fullmatch(line)
        if button is not None:
            if pending is not None:
                raise HardwareError("Home control sample has no following render")
            pending = (button.group(1), int(button.group(2)))
            continue
        render = _GFX_RENDER_MS_RE.search(line)
        timestamp = _TIMED_LOG_RE.match(line)
        if pending is None or render is None or timestamp is None:
            continue
        control, pressed_ms = pending
        render_ms = int(render.group(1))
        press_to_render_ms = int(timestamp.group(1)) - render_ms - pressed_ms
        if press_to_render_ms < 0:
            raise HardwareError("Home control render precedes its press")
        samples.append(
            {
                "control": control,
                "pressAckToRenderStartMs": press_to_render_ms,
                "renderMs": render_ms,
            }
        )
        pending = None
    if pending is not None or not samples:
        raise HardwareError("Home control run has incomplete samples")
    return {
        "homeControlPressAckToRenderStartMs": _stats(
            [int(sample["pressAckToRenderStartMs"]) for sample in samples]
        ),
        "homeControlRenderMs": _stats([int(sample["renderMs"]) for sample in samples]),
        "homeControlSamples": samples,
    }


def _confirmation_hold_metrics(serial: str) -> dict[str, object]:
    pending_ms: int | None = None
    samples: list[int] = []
    for line in serial.splitlines():
        down = _CONFIRM_DOWN_RE.fullmatch(line)
        if down is not None:
            pending_ms = int(down.group(1))
            continue
        entered = _CONFIRMATION_ENTER_RE.fullmatch(line)
        if entered is not None and pending_ms is not None:
            samples.append(int(entered.group(1)) - pending_ms)
            pending_ms = None
            continue
        if _CONFIRM_UP_RE.fullmatch(line) is not None:
            pending_ms = None
    if not samples or any(sample < 0 for sample in samples):
        raise HardwareError("recent-book hold run has no valid confirmation samples")
    return {
        "confirmHoldToConfirmationMs": _stats(samples),
        "confirmHoldToConfirmationSamples": samples,
    }


def _image_metrics(serial: str, device_profile: str) -> dict[str, object]:
    threshold = _LARGE_IMAGE_PIXELS.get(device_profile)
    if threshold is None:
        raise HardwareError(f"unknown device profile for cached-image calibration: {device_profile}")
    cache_bytes: dict[str, int] = {}
    render: tuple[int, str, int, int] | None = None
    active: tuple[int, str, int, int] | None = None
    active_decode: dict[str, int | str] | None = None
    active_prepare: dict[str, int] | None = None
    last_decompressed: tuple[int, int] | None = None
    cached_samples: list[dict[str, int]] = []
    decode_samples: list[dict[str, int]] = []
    prepare_samples: list[dict[str, int]] = []
    image_discovery_samples: list[int] = []
    streamed_html_ms: int | None = None
    for line in serial.splitlines():
        timestamp = _TIMED_LOG_RE.match(line)
        if timestamp is None:
            continue
        time_ms = int(timestamp.group(1))
        if "Streamed temp HTML" in line:
            streamed_html_ms = time_ms
        if decompressed := _DECOMPRESSED_RE.search(line):
            last_decompressed = (int(decompressed.group(1)), int(decompressed.group(2)))
            if active_prepare is not None:
                active_prepare["archiveCompressedBytes"] = last_decompressed[0]
                active_prepare["imageBytes"] = last_decompressed[1]
        if _IMAGE_FOUND_RE.search(line):
            if streamed_html_ms is not None:
                image_discovery_samples.append(time_ms - streamed_html_ms)
                streamed_html_ms = None
            active_prepare = {"startedMs": time_ms}
        if dimensions := _EHP_IMAGE_DIMENSIONS_RE.search(line):
            if active_prepare is not None:
                active_prepare["sourceWidth"] = int(dimensions.group(1))
                active_prepare["sourceHeight"] = int(dimensions.group(2))
                active_prepare["generationMs"] = time_ms - active_prepare["startedMs"]
                prepare_samples.append({key: value for key, value in active_prepare.items() if key != "startedMs"})
                active_prepare = None
        if written := _CACHE_WRITTEN_RE.search(line):
            cache_bytes[written.group(1)] = int(written.group(4))
            if active_decode is not None:
                active_decode["cacheBytes"] = int(written.group(4))
        if rendered := _IMAGE_RENDER_RE.search(line):
            render = (time_ms, rendered.group(1), int(rendered.group(2)), int(rendered.group(3)))
        if decoding := _IMAGE_DECODE_RE.search(line):
            width = render[2] if render is not None and render[1] == decoding.group(1) else 0
            height = render[3] if render is not None and render[1] == decoding.group(1) else 0
            active_decode = {
                "startedMs": render[0] if render is not None and render[1] == decoding.group(1) else time_ms,
                "width": width,
                "height": height,
            }
            if last_decompressed is not None:
                active_decode["archiveCompressedBytes"] = last_decompressed[0]
                active_decode["imageBytes"] = last_decompressed[1]
        if scaled := _JPEG_SCALE_RE.search(line):
            if active_decode is not None:
                active_decode["sourceWidth"] = int(scaled.group(1))
                active_decode["sourceHeight"] = int(scaled.group(2))
                active_decode["width"] = int(scaled.group(3))
                active_decode["height"] = int(scaled.group(4))
        if loaded := _CACHE_LOAD_RE.search(line):
            width = int(loaded.group(2))
            height = int(loaded.group(3))
            started_ms = render[0] if render is not None and render[2:] == (width, height) else time_ms
            active = (started_ms, loaded.group(1), width, height)
        elif "Cache render complete" in line and active is not None:
            started_ms, path, width, height = active
            bytes_value = cache_bytes.get(path, 4 + ((width + 3) // 4) * height)
            cached_samples.append(
                {
                    "generationMs": time_ms - started_ms,
                    "width": width,
                    "height": height,
                    "cacheBytes": bytes_value,
                }
            )
            active = None
        if "Decode successful" in line and active_decode is not None:
            active_decode["generationMs"] = time_ms - cast(int, active_decode["startedMs"])
            decode_samples.append(
                {key: cast(int, value) for key, value in active_decode.items() if key != "startedMs"}
            )
            active_decode = None
    if not cached_samples and not decode_samples and not prepare_samples and not image_discovery_samples:
        return {}
    small = [sample["generationMs"] for sample in cached_samples if sample["width"] * sample["height"] < threshold]
    large = [sample["generationMs"] for sample in cached_samples if sample["width"] * sample["height"] >= threshold]
    grouped: dict[tuple[int, int, int], list[int]] = {}
    for sample in cached_samples:
        key = (sample["width"], sample["height"], sample["cacheBytes"])
        grouped.setdefault(key, []).append(sample["generationMs"])
    decode_small = [
        sample["generationMs"] for sample in decode_samples if sample.get("imageBytes", 0) <= _SMALL_IMAGE_MAX_BYTES
    ]
    decode_medium = [
        sample["generationMs"]
        for sample in decode_samples
        if _SMALL_IMAGE_MAX_BYTES < sample.get("imageBytes", 0) <= _MEDIUM_IMAGE_MAX_BYTES
    ]
    decode_large = [
        sample["generationMs"] for sample in decode_samples if sample.get("imageBytes", 0) > _MEDIUM_IMAGE_MAX_BYTES
    ]
    prepare_small = [
        sample["generationMs"] for sample in prepare_samples if sample.get("imageBytes", 0) <= _SMALL_IMAGE_MAX_BYTES
    ]
    prepare_medium = [
        sample["generationMs"]
        for sample in prepare_samples
        if _SMALL_IMAGE_MAX_BYTES < sample.get("imageBytes", 0) <= _MEDIUM_IMAGE_MAX_BYTES
    ]
    prepare_large = [
        sample["generationMs"] for sample in prepare_samples if sample.get("imageBytes", 0) > _MEDIUM_IMAGE_MAX_BYTES
    ]
    return {
        **({"htmlToImageMs": _stats(image_discovery_samples)} if image_discovery_samples else {}),
        **({"htmlToImageSamples": image_discovery_samples} if image_discovery_samples else {}),
        **({"prepareSmallMs": _stats(prepare_small)} if prepare_small else {}),
        **({"prepareMediumMs": _stats(prepare_medium)} if prepare_medium else {}),
        **({"prepareLargeMs": _stats(prepare_large)} if prepare_large else {}),
        **({"prepareSamples": prepare_samples} if prepare_samples else {}),
        **(
            {"decodeAndCacheMs": _stats([sample["generationMs"] for sample in decode_samples])}
            if decode_samples
            else {}
        ),
        **({"decodeAndCacheSmallMs": _stats(decode_small)} if decode_small else {}),
        **({"decodeAndCacheMediumMs": _stats(decode_medium)} if decode_medium else {}),
        **({"decodeAndCacheLargeMs": _stats(decode_large)} if decode_large else {}),
        "decodeSourceBytes": {
            "smallMax": _SMALL_IMAGE_MAX_BYTES,
            "mediumMax": _MEDIUM_IMAGE_MAX_BYTES,
        },
        **({"decodeAndCacheSamples": decode_samples} if decode_samples else {}),
        **(
            {"cachedRenderMs": _stats([sample["generationMs"] for sample in cached_samples])}
            if cached_samples
            else {}
        ),
        **({"cachedRenderSmallMs": _stats(small)} if small else {}),
        **({"cachedRenderLargeMs": _stats(large)} if large else {}),
        **(
            {
                "cachedRenderSamples": [
                    {"width": width, "height": height, "cacheBytes": bytes_value, "renderMs": _stats(durations)}
                    for (width, height, bytes_value), durations in sorted(grouped.items(), key=lambda item: item[0][2])
                ]
            }
            if grouped
            else {}
        ),
    }


def _indexing_metrics(serial: str) -> dict[str, int]:
    phases = {
        "opfPassMs": r"OPF pass completed in (\d+) ms",
        "tocPassMs": r"TOC pass completed in (\d+) ms",
        "bookBinMs": r"buildBookBin completed in (\d+) ms",
        "totalIndexingMs": r"Total indexing completed in (\d+) ms",
    }
    result = {
        name: int(match.group(1))
        for name, pattern in phases.items()
        if (match := re.search(pattern, serial)) is not None
    }
    if not result:
        return result
    manifest = re.search(r"Using fast index for (\d+) manifest items", serial)
    metadata = re.search(r"Loaded cache data: (\d+) spine, (\d+) TOC entries", serial)
    if manifest is not None:
        result["manifestItems"] = int(manifest.group(1))
    if metadata is not None:
        result["spineItems"] = int(metadata.group(1))
        result["tocItems"] = int(metadata.group(2))
    post_index = _timed_pairs(serial, "Total indexing completed", "Loaded cache data:")
    if post_index:
        result["postIndexLoadMs"] = post_index[0]
    return result


def _corpus_metrics(serial: str, markers: dict[str, int], device_profile: str) -> dict[str, object]:
    names = sorted(
        name[: -len("-cold-open")]
        for name in markers
        if name.endswith("-cold-open") and name != "cold-open"
    )
    result: dict[str, object] = {}
    for name in names:
        cold_start = f"CAL:MARK:{name}-cold-open:"
        cold_end = f"CAL:MARK:{name}-cold-ready:"
        page_start = f"CAL:MARK:{name}-page-00:"
        page_end = f"CAL:MARK:{name}-page-ready:"
        home_start = f"CAL:MARK:{name}-home:"
        home_end = f"CAL:MARK:{name}-home-ready:"
        if not all(marker in serial for marker in (cold_start, cold_end, page_start, page_end, home_start, home_end)):
            continue
        cold_serial = serial.split(cold_start, 1)[1].split(cold_end, 1)[0]
        page_serial = serial.split(page_start, 1)[1].split(page_end, 1)[0]
        home_serial = serial.split(home_start, 1)[1].split(home_end, 1)[0]
        cache_start = f"CAL:MARK:{name}-cache-clear:"
        cache_serial = serial.split(cache_start, 1)[1].split(cold_start, 1)[0] if cache_start in serial else ""
        cache_clear = _cache_clear_metrics(cache_serial)
        images = _image_metrics(cold_serial, device_profile)
        page_images = _image_metrics(page_serial, device_profile)
        page: dict[str, object] = {
            "inputToReadyMs": markers[f"{name}-page-ready"] - markers[f"{name}-page-00"]
        }
        if _PAGE_RE.search(page_serial):
            page["render"] = _page_metrics(page_serial)
        if page_images:
            page["images"] = page_images
        cached_metadata = _timed_pairs(home_serial, "[DBG] [EBP] Loading ePub:", "[DBG] [EBP] Loaded ePub:")
        thumbnail = _thumbnail_metrics(home_serial)
        warm = _warm_repeat_metrics(serial, markers, name)
        result[name] = {
            **({"epubPath": match.group(1)} if (match := _EPUB_LOAD_RE.search(cold_serial)) is not None else {}),
            "coldOpenToReadyMs": markers[f"{name}-cold-ready"] - markers[f"{name}-cold-open"],
            "indexing": _indexing_metrics(cold_serial),
            "page": page,
            **cache_clear,
            **({"cachedMetadataLoadMs": _stats(cached_metadata)} if cached_metadata else {}),
            **({"warm": warm} if warm else {}),
            **(
                {
                    "images": {
                        **images,
                    }
                }
                if images
                else {}
            ),
            **thumbnail,
        }
    return result


def _png_series_metrics(serial: str, markers: dict[str, int]) -> dict[str, object]:
    names = sorted(name[: -len("-png-cold-open")] for name in markers if name.endswith("-png-cold-open"))
    result: dict[str, object] = {}
    for name in names:
        cold_start = f"CAL:MARK:{name}-png-cold-open:"
        cold_end = f"CAL:MARK:{name}-png-cold-ready:"
        home_start = f"CAL:MARK:{name}-png-home:"
        home_end = f"CAL:MARK:{name}-png-home-ready:"
        if not all(marker in serial for marker in (cold_start, cold_end, home_start, home_end)):
            continue
        cold_serial = serial.split(cold_start, 1)[1].split(cold_end, 1)[0]
        home_serial = serial.split(home_start, 1)[1].split(home_end, 1)[0]
        path_match = _EPUB_LOAD_RE.search(cold_serial)
        thumbnail = _thumbnail_metrics(home_serial)
        if path_match is None or not thumbnail:
            continue
        cache_start = f"CAL:MARK:{name}-cache-clear:"
        cache_serial = serial.split(cache_start, 1)[1].split(cold_start, 1)[0] if cache_start in serial else ""
        result[name] = {
            "epubPath": path_match.group(1),
            "coldOpenToReadyMs": markers[f"{name}-png-cold-ready"] - markers[f"{name}-png-cold-open"],
            **_cache_clear_metrics(cache_serial),
            "images": thumbnail,
        }
    return result


def _indexing_repeat_metrics(serial: str, markers: dict[str, int]) -> dict[str, object]:
    starts = sorted(
        marker
        for marker in markers
        if re.fullmatch(r".+-index-\d+-cold-open", marker) is not None
    )
    if not starts:
        return {}
    fields = ("opfPassMs", "tocPassMs", "bookBinMs", "totalIndexingMs", "postIndexLoadMs")
    samples_by_path: dict[str, list[dict[str, int]]] = {}
    for start in starts:
        ready = start.removesuffix("-cold-open") + "-cold-ready"
        if ready not in markers:
            raise HardwareError(f"repeated indexing run is missing marker: {ready}")
        start_text = f"CAL:MARK:{start}:"
        end_text = f"CAL:MARK:{ready}:"
        segment = serial.split(start_text, 1)[1].split(end_text, 1)[0]
        path_match = _EPUB_LOAD_RE.search(segment)
        indexing = _indexing_metrics(segment)
        if path_match is None or not all(isinstance(indexing.get(field), int) for field in fields):
            raise HardwareError(f"repeated indexing sample is incomplete: {start}")
        samples_by_path.setdefault(path_match.group(1), []).append(
            {
                **indexing,
                "coldOpenToReadyMs": markers[ready] - markers[start],
            }
        )
    if any(len(samples) < 2 for samples in samples_by_path.values()):
        raise HardwareError("each repeated indexing EPUB must contain at least two samples")
    return {
        "groups": [
            {"epubPath": path, "samples": samples_by_path[path]}
            for path in sorted(samples_by_path)
        ]
    }


def _workload_metrics(path: Path, serial: str, device_profile: str) -> dict[str, object]:
    markers = {name: int(time_ms) for name, time_ms in _MARK_RE.findall(serial)}
    result: dict[str, object] = {"sourceRun": path.name}
    indexing_repeats = _indexing_repeat_metrics(serial, markers)
    if indexing_repeats:
        result["indexingRepeats"] = indexing_repeats
        return result
    if any(marker.startswith("home-recents-empty-") for marker in markers):
        result.update(_confirmation_hold_metrics(serial))
        return result
    if "home-controls-start" in markers and "home-controls-ready" in markers:
        result.update(_home_control_metrics(serial))
        return result
    if "file-browser-controls-start" in markers and "file-browser-controls-ready" in markers:
        file_browser_samples = _file_browser_activity_samples(serial)
        if file_browser_samples:
            result["fileBrowserControlActivityToDisplayMs"] = _stats(file_browser_samples)
            result["fileBrowserControlActivityToDisplaySamples"] = file_browser_samples
        file_browser_render_samples = _file_browser_render_samples(serial)
        if file_browser_render_samples:
            result["fileBrowserControlRenderMs"] = _stats(file_browser_render_samples)
            result["fileBrowserControlRenderSamples"] = file_browser_render_samples
        return result
    if "settings-popup-variants-open" in markers and "settings-popup-variants-home-ready" in markers:
        variants = (
            ("sleep-screen", 7),
            ("refresh-frequency", 5),
            ("line-spacing", 3),
            ("paragraph-alignment", 5),
        )
        variant_results: list[dict[str, object]] = []
        all_samples: list[int] = []
        for name, option_count in variants:
            start = f"settings-popup-variant-{name}-open"
            end = f"settings-popup-variant-{name}-samples-ready"
            missing = [marker for marker in (start, end, f"settings-popup-variant-{name}-cancelled") if marker not in markers]
            if missing:
                raise HardwareError(f"Settings popup-variant run is missing markers: {', '.join(missing)}")
            segment = serial.split(f"CAL:MARK:{start}:", 1)[1].split(f"CAL:MARK:{end}:", 1)[0]
            samples = _calibration_activity_render_samples(segment, "Settings")
            if not samples:
                raise HardwareError(f"Settings popup variant has no render samples: {name}")
            variant_results.append(
                {
                    "name": name,
                    "optionCount": option_count,
                    "renderMs": _stats(samples),
                    "renderSamples": samples,
                }
            )
            all_samples.extend(samples)
        result["settingsPopupVariants"] = variant_results
        result["settingsPopupRenderMs"] = _stats(all_samples)
        result["settingsPopupRenderSamples"] = all_samples
        return result
    if "settings-open" in markers and "settings-home-ready" in markers:
        if "settings-popup-down-00" in markers:
            required = ("settings-popup-open", "settings-popup-move-ready")
            missing = [marker for marker in required if marker not in markers]
            if missing:
                raise HardwareError(f"Settings popup run is missing markers: {', '.join(missing)}")
            segment = serial.split("CAL:MARK:settings-popup-open:", 1)[1].split(
                "CAL:MARK:settings-popup-move-ready:", 1
            )[0]
            samples = _calibration_activity_render_samples(segment, "Settings")
            if samples:
                result["settingsPopupRenderMs"] = _stats(samples)
                result["settingsPopupRenderSamples"] = samples
            return result
        settings_first_samples, settings_subsequent_samples = _settings_render_samples(serial)
        if settings_first_samples and settings_subsequent_samples:
            result["settingsFirstRenderMs"] = _stats(settings_first_samples)
            result["settingsFirstRenderSamples"] = settings_first_samples
            result["settingsSubsequentRenderMs"] = _stats(settings_subsequent_samples)
            result["settingsSubsequentRenderSamples"] = settings_subsequent_samples
        return result
    if "reader-percent-open" in markers and "reader-percent-home-ready" in markers:
        percent_first, percent_subsequent = _activity_render_samples(serial, "EpubReaderPercentSelection")
        if percent_first and percent_subsequent:
            result["readerPercentFirstRenderMs"] = _stats(percent_first)
            result["readerPercentFirstRenderSamples"] = percent_first
            result["readerPercentSubsequentRenderMs"] = _stats(percent_subsequent)
            result["readerPercentSubsequentRenderSamples"] = percent_subsequent
        return result
    if "reader-popups-navigation-open" in markers and "reader-popups-home-ready" in markers:
        popup_markers = (
            "reader-orientation-popup-open",
            "reader-orientation-popup-cancel",
            "reader-auto-turn-popup-open",
            "reader-auto-turn-popup-cancel",
        )
        missing = [marker for marker in popup_markers if marker not in markers]
        if missing:
            raise HardwareError(f"Reader popup run is missing markers: {', '.join(missing)}")
        orientation_segment = serial.split("CAL:MARK:reader-orientation-popup-open:", 1)[1].split(
            "CAL:MARK:reader-orientation-popup-cancel:", 1
        )[0]
        auto_turn_segment = serial.split("CAL:MARK:reader-auto-turn-popup-open:", 1)[1].split(
            "CAL:MARK:reader-auto-turn-popup-cancel:", 1
        )[0]
        orientation_samples = _calibration_activity_render_samples(orientation_segment, "EpubReaderMenu")
        auto_turn_samples = _calibration_activity_render_samples(auto_turn_segment, "EpubReaderMenu")
        if orientation_samples and auto_turn_samples:
            result["readerMenuOrientationPopupRenderMs"] = _stats(orientation_samples)
            result["readerMenuOrientationPopupRenderSamples"] = orientation_samples
            result["readerMenuAutoTurnPopupRenderMs"] = _stats(auto_turn_samples)
            result["readerMenuAutoTurnPopupRenderSamples"] = auto_turn_samples
            result["readerMenuPopupRenderSamples"] = orientation_samples + auto_turn_samples
        return result
    if "reader-navigation-open" in markers and "reader-navigation-home-ready" in markers:
        menu_first, menu_subsequent = _activity_render_samples(serial, "EpubReaderMenu")
        chapters_first, chapters_subsequent = _activity_render_samples(serial, "EpubReaderChapterSelection")
        if menu_first and menu_subsequent and chapters_first and chapters_subsequent:
            result["readerMenuFirstRenderMs"] = _stats(menu_first)
            result["readerMenuFirstRenderSamples"] = menu_first
            result["readerMenuSubsequentRenderMs"] = _stats(menu_subsequent)
            result["readerMenuSubsequentRenderSamples"] = menu_subsequent
            result["readerChapterSelectionFirstRenderMs"] = _stats(chapters_first)
            result["readerChapterSelectionFirstRenderSamples"] = chapters_first
            result["readerChapterSelectionSubsequentRenderMs"] = _stats(chapters_subsequent)
            result["readerChapterSelectionSubsequentRenderSamples"] = chapters_subsequent
        return result
    section_stream_samples = _section_stream_samples(serial)
    if section_stream_samples:
        result["sectionStreamMs"] = _stats(section_stream_samples)
        result["sectionStreamSamples"] = section_stream_samples
    if "library-navigation-ready" in markers:
        file_browser_samples = _file_browser_activity_samples(serial)
        if file_browser_samples:
            result["fileBrowserActivityToDisplayMs"] = _stats(file_browser_samples)
            result["fileBrowserActivityToDisplaySamples"] = file_browser_samples
        file_browser_render_samples = _file_browser_render_samples(serial)
        if file_browser_render_samples:
            result["fileBrowserRenderMs"] = _stats(file_browser_render_samples)
            result["fileBrowserRenderSamples"] = file_browser_render_samples
        home_first_samples, home_subsequent_samples = _home_render_samples(serial)
        if home_first_samples and home_subsequent_samples:
            result["homeFirstRenderMs"] = _stats(home_first_samples)
            result["homeFirstRenderSamples"] = home_first_samples
            result["homeSubsequentRenderMs"] = _stats(home_subsequent_samples)
            result["homeSubsequentRenderSamples"] = home_subsequent_samples
    corpus = _corpus_metrics(serial, markers, device_profile)
    png_series = _png_series_metrics(serial, markers)
    indexing = _indexing_metrics(serial)
    if indexing and not corpus and not png_series:
        if match := _EPUB_LOAD_RE.search(serial):
            result["epubPath"] = match.group(1)
        result["indexing"] = indexing
    if corpus:
        result["corpus"] = corpus
    if png_series:
        result["pngSeries"] = png_series
    warm_names = sorted(
        {
            match.group(1)
            for marker in markers
            if (match := re.fullmatch(r"(.+)-warm-open-\d+", marker)) is not None
        }
    )
    if not corpus and len(warm_names) == 1:
        warm = _warm_repeat_metrics(serial, markers, warm_names[0])
        if warm:
            result["warm"] = warm
            if match := _EPUB_LOAD_RE.search(serial):
                result["epubPath"] = match.group(1)
    cache_clear = _cache_clear_metrics(serial)
    if cache_clear and not corpus and not png_series:
        result.update(cache_clear)
    load_pairs = (
        ("coldOpenToReadyMs", "cold-open", "cold-ready"),
        ("warmOpenToReadyMs", "warm-open", "warm-ready"),
        ("warmQuiescentOpenToReadyMs", "warm-quiescent-open", "warm-quiescent-ready"),
        ("xlOpenToReadyMs", "xl-open", "xl-ready"),
    )
    loads = {
        name: markers[end] - markers[start]
        for name, start, end in load_pairs
        if start in markers and end in markers
    }
    if loads:
        result["loads"] = loads
    sleep_input = re.search(r"CAL:BUTTON:power:(?:DOWN|PULSE:\d+):(\d+)", serial)
    if sleep_input is not None:
        input_ms = int(sleep_input.group(1))
        activity_ms = _first_timed_after(serial, "[DBG] [ACT] Entering activity: Sleep", input_ms)
        deep_sleep_ms = _first_timed_after(serial, "[DBG] [MAIN] Entering deep sleep", input_ms)
        if activity_ms is not None and deep_sleep_ms is not None:
            segment = serial[serial.find(sleep_input.group(0)) :]
            waits = [int(value) for value in _WAIT_RE.findall(segment)]
            result["sleep"] = {
                "powerInputToActivityMs": activity_ms - input_ms,
                "powerInputToDeepSleepMs": deep_sleep_ms - input_ms,
                "activityToDeepSleepMs": deep_sleep_ms - activity_ms,
                **(
                    {"controllerBusyMs": {"fast": waits[0], "full": waits[1]}}
                    if len(waits) >= 2
                    else {}
                ),
            }
    cached_metadata = _timed_pairs(serial, "[DBG] [EBP] Loading ePub:", "[DBG] [EBP] Loaded ePub:")
    if cached_metadata and "warmQuiescentOpenToReadyMs" in loads:
        result["cachedMetadataLoadMs"] = _stats(cached_metadata)
        warm_start = markers["warm-quiescent-open"]
        metadata_start = _first_timed_after(serial, "[DBG] [EBP] Loading ePub:", warm_start)
        page_start = _first_timed_after(serial, "[DBG] [ERS] Loading file:", warm_start)
        if metadata_start is not None and page_start is not None:
            loads["warmMetadataStartToReadyMs"] = page_start - metadata_start
    images = _image_metrics(serial, device_profile)
    thumbnail = _thumbnail_metrics(serial)
    if corpus and images:
        result["corpusImages"] = {
            key: images[key]
            for key in ("prepareSamples", "decodeAndCacheSamples")
            if key in images
        }
    elif (images or thumbnail) and not png_series:
        result["images"] = {
            **images,
            **thumbnail,
        }
    for workload, start, end in (
        ("coldPages", "CAL:MARK:cold-page-00:", "CAL:MARK:cold-pages-ready:"),
        ("xlPages", "CAL:MARK:xl-page-00:", "CAL:MARK:xl-pages-ready:"),
        ("halfPages", "CAL:MARK:half-page-00:", "CAL:MARK:half-pages-ready:"),
    ):
        if start in serial and end in serial:
            result[workload] = _page_metrics(serial.split(start, 1)[1].split(end, 1)[0])
    if len(result) == 1:
        raise HardwareError(f"workload run contains no recognized timing markers: {path}")
    return result


def build_profile(
    profile_id: str,
    page_run: Path,
    panel_run: Path,
    sd_runs: list[Path],
    workload_runs: list[Path],
    optical_analysis: Path | None = None,
    grayscale_analysis: Path | None = None,
    half_render_run: Path | None = None,
    half_waveform_analysis: Path | None = None,
    fast_waveform_analysis: Path | None = None,
    image_fast_waveform_analysis: Path | None = None,
    directory_run: Path | None = None,
    supplemental_workload_runs: list[Path] | None = None,
    warm_workload_runs: list[Path] | None = None,
    evidence_workload_runs: list[Path] | None = None,
) -> dict[str, object]:
    paths = [page_run, panel_run, *sd_runs, *workload_runs]
    loaded = [(path, *_load_run(path)) for path in paths]
    optical: dict[str, object] | None = None
    grayscale: dict[str, object] | None = None
    reader_half: dict[str, object] | None = None
    reader_fast: dict[str, object] | None = None
    reader_image_fast: dict[str, object] | None = None
    source_runs = loaded.copy()
    if optical_analysis is not None:
        optical, optical_run, optical_manifest = _optical_waveform(optical_analysis)
        source_runs.append((optical_run, optical_manifest, ""))
    if grayscale_analysis is not None:
        grayscale, grayscale_run, grayscale_manifest = _grayscale_waveform(grayscale_analysis)
        source_runs.append((grayscale_run, grayscale_manifest, ""))
    half_render: tuple[Path, dict[str, object], str] | None = None
    if half_render_run is not None:
        half_render_manifest, half_render_serial = _load_run(half_render_run)
        half_render = (half_render_run, half_render_manifest, half_render_serial)
        source_runs.append(half_render)
    if half_waveform_analysis is not None:
        reader_half, reader_half_run, reader_half_manifest = _reader_half_waveform(half_waveform_analysis)
        if not any(path.resolve() == reader_half_run.resolve() for path, _, _ in source_runs):
            source_runs.append((reader_half_run, reader_half_manifest, ""))
    if fast_waveform_analysis is not None:
        reader_fast, reader_fast_run, reader_fast_manifest = _reader_fast_waveform(fast_waveform_analysis)
        if not any(path.resolve() == reader_fast_run.resolve() for path, _, _ in source_runs):
            source_runs.append((reader_fast_run, reader_fast_manifest, ""))
    if image_fast_waveform_analysis is not None:
        reader_image_fast, image_fast_run, image_fast_manifest = _reader_image_fast_waveform(
            image_fast_waveform_analysis
        )
        if not any(path.resolve() == image_fast_run.resolve() for path, _, _ in source_runs):
            source_runs.append((image_fast_run, image_fast_manifest, ""))
    directory: dict[str, object] | None = None
    if directory_run is not None:
        directory_manifest, directory_serial = _load_run(directory_run)
        source_runs.append((directory_run, directory_manifest, directory_serial))
        directory = _directory_metrics(directory_run, directory_serial)
    supplemental_workloads: list[tuple[Path, dict[str, object], str]] = []
    for supplemental_run in supplemental_workload_runs or []:
        supplemental_manifest, supplemental_serial = _load_run(supplemental_run)
        supplemental_workloads.append((supplemental_run, supplemental_manifest, supplemental_serial))
        source_runs.append((supplemental_run, supplemental_manifest, supplemental_serial))
    warm_workloads: list[tuple[Path, dict[str, object], str]] = []
    for warm_run in warm_workload_runs or []:
        warm_manifest, warm_serial = _load_run(warm_run)
        warm_workloads.append((warm_run, warm_manifest, warm_serial))
        source_runs.append((warm_run, warm_manifest, warm_serial))
    evidence_workloads: list[tuple[Path, dict[str, object], str]] = []
    for evidence_run in evidence_workload_runs or []:
        evidence_manifest, evidence_serial = _load_run(evidence_run)
        evidence_workloads.append((evidence_run, evidence_manifest, evidence_serial))
        source_runs.append((evidence_run, evidence_manifest, evidence_serial))
    device_profiles = {_manifest_string(manifest, "deviceProfile", path) for path, manifest, _ in source_runs}
    firmware_versions = {_manifest_string(manifest, "firmwareVersion", path) for path, manifest, _ in loaded}
    source_firmware_versions = {
        _manifest_string(manifest, "firmwareVersion", path) for path, manifest, _ in source_runs
    }
    if len(device_profiles) != 1 or len(firmware_versions) != 1:
        raise HardwareError("base calibration runs do not share one device profile and firmware version")
    if any(manifest.get("status") != "complete" for _, manifest, _ in source_runs):
        raise HardwareError("only complete calibration runs can produce a timing profile")
    device_profile = next(iter(device_profiles))
    page_serial = loaded[0][2]
    panel_serial = loaded[1][2]
    storage_end = 2 + len(sd_runs)
    storage_runs = [(path, serial) for path, _, serial in loaded[2:storage_end]]
    workload_loaded = loaded[storage_end:]
    workloads: dict[str, object] = {
        path.name: _workload_metrics(path, serial, device_profile) for path, _, serial in workload_loaded
    }
    workloads.update(
        {path.name: _workload_metrics(path, serial, device_profile) for path, _, serial in supplemental_workloads}
    )
    model_workloads = {
        name: workload for name, workload in workloads.items() if not _has_cache_population_samples(workload)
    }
    parsed_warm_workloads: dict[str, object] = {
        path.name: _workload_metrics(path, serial, device_profile) for path, _, serial in warm_workloads
    }
    parsed_evidence_workloads: dict[str, object] = {
        path.name: _workload_metrics(path, serial, device_profile) for path, _, serial in evidence_workloads
    }
    jpeg_thumbnail_model = _jpeg_thumbnail_model(model_workloads)
    png_thumbnail_model = _png_thumbnail_model(workloads)
    cache_clear_model = _cache_clear_model(workloads)
    section_streaming = _duration_model(model_workloads, "sectionStreamSamples")
    section_image_discovery = _duration_model(model_workloads, "htmlToImageSamples")
    file_browser_activity = _duration_model(
        model_workloads, "fileBrowserControlActivityToDisplaySamples"
    ) or _duration_model(model_workloads, "fileBrowserActivityToDisplaySamples")
    file_browser_render = _duration_model(model_workloads, "fileBrowserControlRenderSamples") or _duration_model(
        model_workloads, "fileBrowserRenderSamples"
    )
    home_first_render = _duration_model(model_workloads, "homeFirstRenderSamples")
    home_subsequent_render = _duration_model(model_workloads, "homeSubsequentRenderSamples")
    settings_first_render = _duration_model(model_workloads, "settingsFirstRenderSamples")
    settings_subsequent_render = _duration_model(model_workloads, "settingsSubsequentRenderSamples")
    settings_popup_render = _duration_model(model_workloads, "settingsPopupRenderSamples")
    reader_menu_first_render = _duration_model(model_workloads, "readerMenuFirstRenderSamples")
    reader_menu_subsequent_render = _duration_model(model_workloads, "readerMenuSubsequentRenderSamples")
    reader_menu_popup_render = _duration_model(model_workloads, "readerMenuPopupRenderSamples")
    reader_percent_first_render = _duration_model(model_workloads, "readerPercentFirstRenderSamples")
    reader_percent_subsequent_render = _duration_model(model_workloads, "readerPercentSubsequentRenderSamples")
    exact_indexing = _exact_indexing_by_path(model_workloads)
    exact_warm = _exact_warm_by_path(parsed_warm_workloads)
    render = _page_metrics(page_serial)
    if half_render is not None:
        half_path, _, half_serial = half_render
        half_workload = _workload_metrics(half_path, half_serial, device_profile)
        half_pages = _object_dict(half_workload.get("halfPages"))
        half_primary = _object_dict(half_pages.get("halfPrimary")) if half_pages is not None else None
        if half_primary is None:
            raise HardwareError("supplemental half-render run contains no half-primary page timings")
        render["halfPrimary"] = half_primary
        workloads[half_path.name] = half_workload
        model_workloads[half_path.name] = half_workload
    exact_image_decode = _exact_image_decode(model_workloads)
    exact_image_preparation = _exact_image_preparation(model_workloads)
    panel_manifest = loaded[1][1]
    camera = _object_dict(panel_manifest.get("camera"))
    fps_value = camera.get("fps") if camera is not None else None
    fps = fps_value if isinstance(fps_value, int) and fps_value > 0 else None
    video_evidence: dict[str, object] = {
        "captureFps": fps,
        "frameQuantizationMs": round(1000 / fps, 3) if fps else None,
        "reviewRotationDegrees": camera.get("reviewRotationDegrees") if camera is not None else None,
    }
    if optical is not None:
        video_evidence.update(_cadence_evidence(optical))
    return {
        "schemaVersion": 1,
        "id": profile_id,
        "deviceProfile": device_profile,
        "firmwareVersion": next(iter(firmware_versions)),
        "status": "provisional-single-device",
        "statistics": {"nominal": "p50", "tolerance": "p90", "dispersion": "MAD"},
        "renderMs": render,
        "panel": _panel_metrics(panel_serial),
        "opticalWaveform": {
            **(optical or {}),
            **({"grayscale": grayscale} if grayscale is not None else {}),
            **({"readerHalf": reader_half} if reader_half is not None else {}),
            **({"readerFast": reader_fast} if reader_fast is not None else {}),
            **({"readerImageFast": reader_image_fast} if reader_image_fast is not None else {}),
        },
        "storage": _storage_metrics(storage_runs),
        "directory": directory,
        "workloads": workloads,
        "warmWorkloads": parsed_warm_workloads,
        "evidenceWorkloads": parsed_evidence_workloads,
        "models": {
            "jpegThumbnail": jpeg_thumbnail_model,
            "pngThumbnail": png_thumbnail_model,
            "exactImageDecode": exact_image_decode,
            "exactImagePreparation": exact_image_preparation,
            "exactIndexingByPath": exact_indexing,
            "exactWarmByPath": exact_warm,
            "sectionStreamingMs": section_streaming,
            "sectionImageDiscoveryMs": section_image_discovery,
            "fileBrowserActivityToDisplayMs": file_browser_activity,
            "fileBrowserRenderMs": file_browser_render,
            "homeFirstRenderMs": home_first_render,
            "homeSubsequentRenderMs": home_subsequent_render,
            "settingsFirstRenderMs": settings_first_render,
            "settingsSubsequentRenderMs": settings_subsequent_render,
            "settingsPopupRenderMs": settings_popup_render,
            "readerMenuFirstRenderMs": reader_menu_first_render,
            "readerMenuSubsequentRenderMs": reader_menu_subsequent_render,
            "readerMenuPopupRenderMs": reader_menu_popup_render,
            "readerPercentFirstRenderMs": reader_percent_first_render,
            "readerPercentSubsequentRenderMs": reader_percent_subsequent_render,
            "cacheClear": cache_clear_model,
        },
        "videoEvidence": video_evidence,
        "limitations": [
            "One X4 and one SD card were measured; this is not a population or temperature profile.",
            "Render phases are content-specific and retain the observed fast/half refresh split.",
            "All five exact cold-index workloads and the XL-font variant have same-device repeat samples.",
            "Half-primary render phases use the controlled every-page-refresh workload; image-heavy fast-primary pages remain workload evidence.",
            "Reader half-refresh video uses the repeated inverted-target and target-settling peaks from natural page content.",
            "Reader fast-refresh video uses the repeated direct-transition window from natural page content.",
            "Image-heavy reader fast-refresh video uses a separate direct-transition window for majority-dark targets.",
            "Sources span calibration-only command revisions; each source entry records its exact firmware label.",
            "Exact indexing workloads are device-path-specific; renamed and unmeasured EPUBs use fallback timing.",
            "Exact warm-open workloads use per-path p50 phases; p90 and MAD remain tolerance evidence.",
            "Exact image preparation and decode workloads match measured source bytes and output dimensions before "
            "tier fallback.",
            "Settings paint timing distinguishes full clear-and-redraw renders from a provisional shared popup floor; "
            "only the current English Reader Font Size layout was calibrated.",
            "Reader menu paint timing is the current Stormlight clear-and-redraw menu without optional footnote or "
            "saved-bookmark rows; those variants and book-specific chapter lists remain deferred evidence.",
            "Reader menu popup paint timing pools the current English Orientation and Auto Turn overlays; translated "
            "labels remain deferred evidence.",
            "Reader percent-selector paint timing covers the current English fixed layout at 0, 1, and 10 percent; "
            "translated layouts remain deferred evidence.",
            "PNG thumbnail preparation and conversion pacing cover measured RGB8 source byte and area ranges at the X4 Home target; other formats, targets, and extrapolated inputs retain fallback timing.",
            "Directory timing is measured over one populated root and scales per openNextFile call.",
            "Populated cache-clear timing is a per-file fit over three-directory caches; byte and directory costs are not independently identified.",
            "Panel operation includes data transfer and controller busy time; controllerBusyMs isolates the busy wait.",
            "Webcam optical phases are camera-observed, target-dependent evidence; serial/device timings remain canonical.",
        ],
        "sources": [_source(path, manifest) for path, manifest, _ in source_runs],
        "sourceFirmwareVersions": sorted(source_firmware_versions),
    }


def write_profile(profile: dict[str, object], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(profile, indent=2, sort_keys=True) + "\n", encoding="utf-8")
