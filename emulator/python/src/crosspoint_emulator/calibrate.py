from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import platform
import subprocess
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Literal, cast

from .calibration_profile import build_profile, write_profile
from .hardware import CameraInput, HardwareDevice, HardwareError, WebcamRecorder
from .optical_waveform import (
    analyze_boot_waveform,
    analyze_grayscale_waveform,
    analyze_panel_waveform,
    analyze_reader_fast_waveform,
    analyze_reader_half_waveform,
    analyze_reader_image_fast_waveform,
    parse_crop,
    write_waveform,
)


def _load_scenario(path: Path) -> Callable[[HardwareDevice], None]:
    spec = importlib.util.spec_from_file_location("crosspoint_calibration_scenario", path)
    if spec is None or spec.loader is None:
        raise HardwareError(f"cannot load scenario: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    run = getattr(module, "run", None)
    if not callable(run):
        raise HardwareError(f"scenario must define run(device): {path}")
    return cast(Callable[[HardwareDevice], None], run)


def _ffmpeg_version(executable: str) -> str:
    completed = subprocess.run((executable, "-version"), capture_output=True, check=False, text=True)
    return completed.stdout.splitlines()[0] if completed.returncode == 0 and completed.stdout else "unknown"


def _webcam_recorder(
    arguments: argparse.Namespace, output: Path
) -> tuple[WebcamRecorder, CameraInput, Literal[0, 90, 180, 270]]:
    rotation = cast(Literal[0, 90, 180, 270], arguments.rotate)
    input_format = cast(CameraInput, arguments.input_format)
    return (
        WebcamRecorder(
            cast(str, arguments.camera),
            output / "capture.mp4",
            input_format=input_format,
            fps=cast(int, arguments.fps),
            video_size=cast(str, arguments.video_size),
            rotation=rotation,
            input_codec=cast(str | None, arguments.input_codec),
            ffmpeg=cast(str, arguments.ffmpeg),
        ),
        input_format,
        rotation,
    )


def _camera_manifest(
    recorder: WebcamRecorder,
    arguments: argparse.Namespace,
    input_format: CameraInput,
    rotation: Literal[0, 90, 180, 270],
) -> dict[str, object]:
    return {
        "input": arguments.camera,
        "inputFormat": input_format,
        "fps": arguments.fps,
        "videoSize": arguments.video_size,
        "reviewRotationDegrees": rotation,
        "startedHostTimeNs": recorder.started_ns,
        "firstFrameDecodedHostTimeNs": recorder.first_frame_decoded_ns,
        "firstFrameObservedHostTimeNs": recorder.first_frame_ns,
        "stoppedHostTimeNs": recorder.stopped_ns,
        **recorder.source_cadence(),
    }


def _record(arguments: argparse.Namespace) -> int:
    output = cast(Path, arguments.output).resolve()
    if output.exists() and any(output.iterdir()):
        raise HardwareError(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    scenario_path = cast(Path, arguments.script).resolve()
    scenario = _load_scenario(scenario_path)
    recorder, input_format, rotation = _webcam_recorder(arguments, output)
    started_utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    status = "failed"
    error_message: str | None = None
    profile = "unknown"
    firmware_version = "unknown"
    terminal_device_disconnect = False
    terminal_scenario: str | None = None
    device_disconnected_host_time_ns: int | None = None
    try:
        with HardwareDevice.open(
            cast(str, arguments.port),
            output,
            baudrate=cast(int, arguments.baudrate),
            boot_timeout_s=cast(float, arguments.boot_timeout),
        ) as device:
            profile = device.profile
            firmware_version = device.firmware_version
            recorder.start()
            device.sleep(cast(float, arguments.camera_warmup))
            device.mark("capture-start")
            scenario(device)
            terminal_scenario = device.terminal_reason
            terminal_device_disconnect = device.expected_disconnect
            device_disconnected_host_time_ns = device.disconnected_host_time_ns
            if terminal_scenario is None:
                device.mark("capture-end")
            device.sleep(cast(float, arguments.camera_tail))
            status = "complete"
    except BaseException as error:
        error_message = f"{type(error).__name__}: {error}"
        raise
    finally:
        try:
            recorder.stop()
        except HardwareError as error:
            if error_message is None:
                error_message = f"{type(error).__name__}: {error}"
            if status == "complete":
                status = "failed"
                raise
        finally:
            manifest: dict[str, object] = {
                "schemaVersion": 1,
                "status": status,
                "error": error_message,
                "deviceProfile": profile,
                "firmwareVersion": firmware_version,
                "terminalDeviceDisconnect": terminal_device_disconnect,
                "terminalScenario": terminal_scenario,
                "deviceDisconnectedHostTimeNs": device_disconnected_host_time_ns,
                "serialPort": arguments.port,
                "baudrate": arguments.baudrate,
                "scenario": str(scenario_path),
                "scenarioSha256": hashlib.sha256(scenario_path.read_bytes()).hexdigest(),
                "startedUtc": started_utc,
                "host": {"platform": platform.platform(), "python": sys.version.split()[0]},
                "camera": _camera_manifest(recorder, arguments, input_format, rotation),
                "ffmpegVersion": _ffmpeg_version(cast(str, arguments.ffmpeg)),
                "ffmpegArguments": recorder.arguments,
            }
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(output)
    return 0


def _record_boot(arguments: argparse.Namespace) -> int:
    output = cast(Path, arguments.output).resolve()
    if output.exists() and any(output.iterdir()):
        raise HardwareError(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    recorder, input_format, rotation = _webcam_recorder(arguments, output)
    # Preserve the PlatformIO virtualenv symlink: resolving it would invoke the
    # underlying interpreter without the virtualenv's site-packages.
    esptool_python = cast(Path, arguments.esptool_python).expanduser().absolute()
    esptool = cast(Path, arguments.esptool).resolve()
    if not esptool_python.is_file() or not esptool.is_file():
        raise HardwareError("record-boot requires valid --esptool-python and --esptool paths")
    reset_command = (
        str(esptool_python),
        str(esptool),
        "--chip",
        "esp32c3",
        "--port",
        cast(str, arguments.port),
        "--before",
        "usb-reset",
        "--after",
        "hard-reset",
        "run",
    )
    started_utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    status = "failed"
    error_message: str | None = None
    profile = "unknown"
    firmware_version = "unknown"
    reset_started_ns: int | None = None
    reset_finished_ns: int | None = None
    serial_open_started_ns: int | None = None
    try:
        recorder.start()
        time.sleep(cast(float, arguments.camera_warmup))
        reset_started_ns = time.monotonic_ns()
        reset = subprocess.run(reset_command, capture_output=True, check=False, text=True)
        reset_finished_ns = time.monotonic_ns()
        (output / "esptool.log").write_text(reset.stdout + reset.stderr, encoding="utf-8")
        if reset.returncode != 0:
            raise HardwareError(f"esptool reset failed with code {reset.returncode}")
        serial_open_started_ns = time.monotonic_ns()
        with HardwareDevice.open(
            cast(str, arguments.port),
            output,
            baudrate=cast(int, arguments.baudrate),
            boot_timeout_s=cast(float, arguments.boot_timeout),
        ) as device:
            profile = device.profile
            firmware_version = device.firmware_version
            device.wait_for_log("[DBG] [PWR] Going to low-power mode", timeout_s=30)
            device.press("back", hold_ms=100, settle_ms=300)
            device.sleep(cast(float, arguments.camera_tail))
        status = "complete"
    except BaseException as error:
        error_message = f"{type(error).__name__}: {error}"
        raise
    finally:
        try:
            recorder.stop()
        except HardwareError as error:
            if error_message is None:
                error_message = f"{type(error).__name__}: {error}"
            if status == "complete":
                status = "failed"
                raise
        finally:
            scenario_path = Path(__file__).resolve()
            manifest: dict[str, object] = {
                "schemaVersion": 1,
                "status": status,
                "error": error_message,
                "deviceProfile": profile,
                "firmwareVersion": firmware_version,
                "serialPort": arguments.port,
                "baudrate": arguments.baudrate,
                "scenario": "record-boot-v1",
                "scenarioSha256": hashlib.sha256(scenario_path.read_bytes()).hexdigest(),
                "startedUtc": started_utc,
                "host": {"platform": platform.platform(), "python": sys.version.split()[0]},
                "reset": {
                    "command": list(reset_command),
                    "startedHostTimeNs": reset_started_ns,
                    "finishedHostTimeNs": reset_finished_ns,
                    "serialOpenStartedHostTimeNs": serial_open_started_ns,
                },
                "camera": _camera_manifest(recorder, arguments, input_format, rotation),
                "ffmpegVersion": _ffmpeg_version(cast(str, arguments.ffmpeg)),
                "ffmpegArguments": recorder.arguments,
            }
            (output / "manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
    print(output)
    return 0


def _analyze(arguments: argparse.Namespace) -> int:
    output = cast(Path, arguments.output).resolve()
    profile = build_profile(
        profile_id=cast(str, arguments.profile_id),
        page_run=cast(Path, arguments.page_run).resolve(),
        panel_run=cast(Path, arguments.panel_run).resolve(),
        sd_runs=[path.resolve() for path in cast(list[Path], arguments.sd_run)],
        workload_runs=[path.resolve() for path in cast(list[Path], arguments.workload_run)],
        optical_analysis=(
            cast(Path, arguments.optical_analysis).resolve() if arguments.optical_analysis is not None else None
        ),
        grayscale_analysis=(
            cast(Path, arguments.grayscale_analysis).resolve() if arguments.grayscale_analysis is not None else None
        ),
        half_render_run=(
            cast(Path, arguments.half_render_run).resolve() if arguments.half_render_run is not None else None
        ),
        half_waveform_analysis=cast(Path, arguments.half_waveform_analysis).resolve()
        if arguments.half_waveform_analysis is not None
        else None,
        fast_waveform_analysis=cast(Path, arguments.fast_waveform_analysis).resolve()
        if arguments.fast_waveform_analysis is not None
        else None,
        image_fast_waveform_analysis=cast(Path, arguments.image_fast_waveform_analysis).resolve()
        if arguments.image_fast_waveform_analysis is not None
        else None,
        directory_run=cast(Path, arguments.directory_run).resolve() if arguments.directory_run is not None else None,
        supplemental_workload_runs=[
            path.resolve() for path in cast(list[Path], arguments.supplemental_workload_run)
        ],
        warm_workload_runs=[path.resolve() for path in cast(list[Path], arguments.warm_workload_run)],
        evidence_workload_runs=[path.resolve() for path in cast(list[Path], arguments.evidence_workload_run)],
    )
    write_profile(profile, output)
    print(output)
    return 0


def _waveform(arguments: argparse.Namespace) -> int:
    output = cast(Path, arguments.output).resolve()
    analysis = analyze_panel_waveform(
        cast(Path, arguments.run).resolve(),
        parse_crop(cast(str, arguments.crop)),
        threshold=cast(float, arguments.threshold),
        boundary_ms=cast(float, arguments.boundary_ms),
        ffmpeg=cast(str, arguments.ffmpeg),
    )
    write_waveform(analysis, output)
    print(output)
    return 0


def _boot_waveform(arguments: argparse.Namespace) -> int:
    output = cast(Path, arguments.output).resolve()
    analysis = analyze_boot_waveform(
        cast(Path, arguments.run).resolve(),
        parse_crop(cast(str, arguments.crop)),
        threshold=cast(float, arguments.threshold),
        boundary_ms=cast(float, arguments.boundary_ms),
        ffmpeg=cast(str, arguments.ffmpeg),
    )
    write_waveform(analysis, output)
    print(output)
    return 0


def _grayscale_waveform(arguments: argparse.Namespace) -> int:
    output = cast(Path, arguments.output).resolve()
    analysis = analyze_grayscale_waveform(
        cast(Path, arguments.run).resolve(),
        parse_crop(cast(str, arguments.crop)),
        threshold=cast(float, arguments.threshold),
        window_ms=cast(float, arguments.window_ms),
        ffmpeg=cast(str, arguments.ffmpeg),
    )
    write_waveform(analysis, output)
    print(output)
    return 0


def _reader_half_waveform(arguments: argparse.Namespace) -> int:
    output = cast(Path, arguments.output).resolve()
    analysis = analyze_reader_half_waveform(
        cast(Path, arguments.run).resolve(),
        parse_crop(cast(str, arguments.crop)),
        threshold=cast(float, arguments.threshold),
        window_ms=cast(float, arguments.window_ms),
        ffmpeg=cast(str, arguments.ffmpeg),
    )
    write_waveform(analysis, output)
    print(output)
    return 0


def _reader_fast_waveform(arguments: argparse.Namespace) -> int:
    output = cast(Path, arguments.output).resolve()
    analysis = analyze_reader_fast_waveform(
        cast(Path, arguments.run).resolve(),
        parse_crop(cast(str, arguments.crop)),
        threshold=cast(float, arguments.threshold),
        window_ms=cast(float, arguments.window_ms),
        ffmpeg=cast(str, arguments.ffmpeg),
    )
    write_waveform(analysis, output)
    print(output)
    return 0


def _reader_image_fast_waveform(arguments: argparse.Namespace) -> int:
    output = cast(Path, arguments.output).resolve()
    analysis = analyze_reader_image_fast_waveform(
        cast(Path, arguments.run).resolve(),
        parse_crop(cast(str, arguments.crop)),
        threshold=cast(float, arguments.threshold),
        window_ms=cast(float, arguments.window_ms),
        minimum_image_area_fraction=cast(float, arguments.minimum_image_area_fraction),
        target_dark_pixel_percent_min=cast(int, arguments.target_dark_pixel_percent_min),
        ffmpeg=cast(str, arguments.ffmpeg),
    )
    write_waveform(analysis, output)
    print(output)
    return 0


def _add_capture_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--port", required=True, help="serial device, e.g. /dev/ttyACM0 or COM4")
    parser.add_argument("--camera", required=True, help="camera device path, DirectShow name, or AVFoundation index")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--input-format", choices=("v4l2", "dshow", "avfoundation"), default="v4l2")
    parser.add_argument("--input-codec", help="camera input codec, commonly mjpeg")
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--video-size", default="1920x1080")
    parser.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=0)
    parser.add_argument("--baudrate", type=int, default=115_200)
    parser.add_argument("--boot-timeout", type=float, default=30.0)
    parser.add_argument("--camera-warmup", type=float, default=2.0)
    parser.add_argument("--camera-tail", type=float, default=2.0)
    parser.add_argument("--ffmpeg", default="ffmpeg")


def main() -> None:
    parser = argparse.ArgumentParser(description="Drive CrossPoint calibration firmware while recording a webcam")
    subparsers = parser.add_subparsers(dest="command", required=True)
    record = subparsers.add_parser("record", help="record one scripted physical-device run")
    _add_capture_arguments(record)
    record.add_argument("--script", required=True, type=Path, help="Python file defining run(device)")
    record.set_defaults(handler=_record)

    record_boot = subparsers.add_parser("record-boot", help="hard-reset and record one physical boot to Home")
    _add_capture_arguments(record_boot)
    record_boot.add_argument(
        "--esptool-python",
        type=Path,
        default=Path.home() / ".platformio/penv/bin/python",
        help="Python executable for esptool",
    )
    record_boot.add_argument(
        "--esptool",
        type=Path,
        default=Path.home() / ".platformio/packages/tool-esptoolpy/esptool.py",
        help="esptool.py path",
    )
    record_boot.set_defaults(handler=_record_boot)

    analyze = subparsers.add_parser("analyze", help="build a timing profile from completed calibration runs")
    analyze.add_argument("--profile-id", required=True)
    analyze.add_argument("--page-run", required=True, type=Path)
    analyze.add_argument("--panel-run", required=True, type=Path)
    analyze.add_argument("--sd-run", required=True, action="append", type=Path)
    analyze.add_argument("--workload-run", action="append", type=Path, default=[])
    analyze.add_argument("--optical-analysis", type=Path)
    analyze.add_argument("--grayscale-analysis", type=Path)
    analyze.add_argument("--half-render-run", type=Path)
    analyze.add_argument("--half-waveform-analysis", type=Path)
    analyze.add_argument("--fast-waveform-analysis", type=Path)
    analyze.add_argument("--image-fast-waveform-analysis", type=Path)
    analyze.add_argument("--directory-run", type=Path)
    analyze.add_argument("--supplemental-workload-run", action="append", type=Path, default=[])
    analyze.add_argument("--warm-workload-run", action="append", type=Path, default=[])
    analyze.add_argument("--evidence-workload-run", action="append", type=Path, default=[])
    analyze.add_argument("--output", required=True, type=Path)
    analyze.set_defaults(handler=_analyze)

    waveform = subparsers.add_parser("waveform", help="extract repeated optical panel phases from a capture")
    waveform.add_argument("--run", required=True, type=Path)
    waveform.add_argument("--crop", required=True, help="inner screen crop as x,y,width,height")
    waveform.add_argument("--threshold", type=float, default=10.0, help="minimum mean absolute luma change")
    waveform.add_argument("--boundary-ms", type=float, default=200.0, help="exclude operation-boundary changes")
    waveform.add_argument("--ffmpeg", default="ffmpeg")
    waveform.add_argument("--output", required=True, type=Path)
    waveform.set_defaults(handler=_waveform)

    boot_waveform = subparsers.add_parser("waveform-boot", help="extract a natural X4 Boot full-refresh pulse train")
    boot_waveform.add_argument("--run", required=True, type=Path)
    boot_waveform.add_argument("--crop", required=True, help="inner screen crop as x,y,width,height")
    boot_waveform.add_argument("--threshold", type=float, default=10.0, help="minimum mean absolute luma change")
    boot_waveform.add_argument("--boundary-ms", type=float, default=200.0, help="exclude operation-boundary changes")
    boot_waveform.add_argument("--ffmpeg", default="ffmpeg")
    boot_waveform.add_argument("--output", required=True, type=Path)
    boot_waveform.set_defaults(handler=_boot_waveform)

    grayscale = subparsers.add_parser("waveform-gray", help="extract controlled grayscale optical phases")
    grayscale.add_argument("--run", required=True, type=Path)
    grayscale.add_argument("--crop", required=True, help="inner screen crop as x,y,width,height")
    grayscale.add_argument("--threshold", type=float, default=3.0, help="minimum mean absolute luma change")
    grayscale.add_argument("--window-ms", type=float, default=500.0, help="analysis window after grayscale activation")
    grayscale.add_argument("--ffmpeg", default="ffmpeg")
    grayscale.add_argument("--output", required=True, type=Path)
    grayscale.set_defaults(handler=_grayscale_waveform)

    reader_half = subparsers.add_parser("waveform-half", help="extract natural reader half-refresh inversion phases")
    reader_half.add_argument("--run", required=True, type=Path)
    reader_half.add_argument("--crop", required=True, help="inner screen crop as x,y,width,height")
    reader_half.add_argument("--threshold", type=float, default=5.0, help="minimum mean absolute luma change")
    reader_half.add_argument("--window-ms", type=float, default=2200.0, help="analysis window after displayBuffer")
    reader_half.add_argument("--ffmpeg", default="ffmpeg")
    reader_half.add_argument("--output", required=True, type=Path)
    reader_half.set_defaults(handler=_reader_half_waveform)

    reader_fast = subparsers.add_parser("waveform-fast", help="extract natural reader fast-refresh transition phases")
    reader_fast.add_argument("--run", required=True, type=Path)
    reader_fast.add_argument("--crop", required=True, help="inner screen crop as x,y,width,height")
    reader_fast.add_argument("--threshold", type=float, default=5.0, help="minimum mean absolute luma change")
    reader_fast.add_argument("--window-ms", type=float, default=800.0, help="analysis window after displayBuffer")
    reader_fast.add_argument("--ffmpeg", default="ffmpeg")
    reader_fast.add_argument("--output", required=True, type=Path)
    reader_fast.set_defaults(handler=_reader_fast_waveform)

    reader_image_fast = subparsers.add_parser(
        "waveform-image-fast", help="extract image-heavy reader fast-refresh transition phases"
    )
    reader_image_fast.add_argument("--run", required=True, type=Path)
    reader_image_fast.add_argument("--crop", required=True, help="inner screen crop as x,y,width,height")
    reader_image_fast.add_argument("--threshold", type=float, default=5.0, help="minimum mean absolute luma change")
    reader_image_fast.add_argument("--window-ms", type=float, default=800.0, help="analysis window after displayBuffer")
    reader_image_fast.add_argument(
        "--minimum-image-area-fraction",
        type=float,
        default=0.5,
        help="minimum rendered image fraction of the X4 panel",
    )
    reader_image_fast.add_argument(
        "--target-dark-pixel-percent-min",
        type=int,
        default=50,
        help="minimum canonical target dark-pixel percentage for selecting this waveform",
    )
    reader_image_fast.add_argument("--ffmpeg", default="ffmpeg")
    reader_image_fast.add_argument("--output", required=True, type=Path)
    reader_image_fast.set_defaults(handler=_reader_image_fast_waveform)
    arguments = parser.parse_args()
    handler = cast(Callable[[argparse.Namespace], int], arguments.handler)
    try:
        raise SystemExit(handler(arguments))
    except HardwareError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
