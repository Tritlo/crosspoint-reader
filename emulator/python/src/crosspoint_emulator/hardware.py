from __future__ import annotations

import importlib
import json
import re
import shutil
import signal
import statistics
import subprocess
import threading
import time
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from types import TracebackType
from typing import IO, Literal, Protocol, Self, cast

from .client import EmulatorError, PhysicalControl

CameraInput = Literal["v4l2", "dshow", "avfoundation"]
_FNV_OFFSET_BASIS = 14695981039346656037
_FNV_PRIME = 1099511628211
_SHOWINFO_FRAME_RE = re.compile(rb"\bn:\s*(\d+)\b.*?\bpts_time:([+-]?(?:\d+(?:\.\d*)?|\.\d+))")


class HardwareError(EmulatorError):
    pass


def summarize_source_cadence(frame_pts_seconds: list[float]) -> dict[str, object]:
    intervals_ms = [
        (current - previous) * 1000
        for previous, current in zip(frame_pts_seconds, frame_pts_seconds[1:])
        if current > previous
    ]
    if not intervals_ms:
        return {"decodedSourceFrames": len(frame_pts_seconds)}
    ordered = sorted(intervals_ms)
    p90_index = (len(ordered) - 1) * 0.9
    lower = int(p90_index)
    upper = min(lower + 1, len(ordered) - 1)
    p90_interval_ms = ordered[lower] + (ordered[upper] - ordered[lower]) * (p90_index - lower)
    median_interval_ms = statistics.median(intervals_ms)
    return {
        "decodedSourceFrames": len(frame_pts_seconds),
        "observedSourceFps": round(1000 / median_interval_ms, 3),
        "sourceFrameIntervalMs": {
            "min": round(min(intervals_ms), 3),
            "p50": round(median_interval_ms, 3),
            "p90": round(p90_interval_ms, 3),
            "max": round(max(intervals_ms), 3),
        },
    }


def read_source_cadence(ffmpeg_log: Path) -> dict[str, object]:
    frame_pts_seconds: list[float] = []
    try:
        with ffmpeg_log.open("rb") as log:
            for raw in log:
                frame = _SHOWINFO_FRAME_RE.search(raw) if b"showinfo" in raw else None
                if frame is not None:
                    frame_pts_seconds.append(float(frame.group(2)))
    except OSError:
        return {}
    return summarize_source_cadence(frame_pts_seconds)


class _SerialPort(Protocol):
    def write(self, data: bytes) -> int: ...

    def flush(self) -> None: ...

    def readline(self) -> bytes: ...

    def close(self) -> None: ...


@dataclass(frozen=True)
class HardwareReply:
    line: str
    host_time_ns: int
    device_time_ms: int


class HardwareDevice:
    """Synchronous control of calibration firmware over USB serial."""

    def __init__(self, serial_port: _SerialPort, artifact_directory: Path) -> None:
        self._serial = serial_port
        self._artifact_directory = artifact_directory
        self._serial_log: IO[bytes] = (artifact_directory / "serial.log").open("wb")
        self._events: IO[str] = (artifact_directory / "events.jsonl").open("w", encoding="utf-8")
        self._condition = threading.Condition()
        self._replies: deque[tuple[int, str]] = deque()
        self._recent_device_lines: deque[tuple[int, str]] = deque(maxlen=4096)
        self._closed = False
        self._reader_stopped = threading.Event()
        self._terminal_reason: str | None = None
        self._disconnected_host_time_ns: int | None = None
        self._reader = threading.Thread(target=self._read_serial, name="crosspoint-serial", daemon=True)
        self._reader.start()
        self.profile = "unknown"
        self.firmware_version = "unknown"

    @classmethod
    def open(
        cls,
        port: str,
        artifact_directory: str | Path,
        *,
        baudrate: int = 115_200,
        boot_timeout_s: float = 30.0,
    ) -> Self:
        artifacts = Path(artifact_directory)
        artifacts.mkdir(parents=True, exist_ok=True)
        try:
            serial_module = importlib.import_module("serial")
            serial_constructor = cast(Callable[..., _SerialPort], getattr(serial_module, "Serial"))
            serial_port = serial_constructor(port=port, baudrate=baudrate, timeout=0.1, write_timeout=2.0)
        except ImportError as error:
            raise HardwareError(
                "physical calibration requires the 'calibration' package extra: crosspoint-emulator[calibration]"
            ) from error
        except (OSError, ValueError) as error:
            raise HardwareError(f"cannot open serial port {port}: {error}") from error

        device = cls(serial_port, artifacts)
        deadline = time.monotonic() + boot_timeout_s
        try:
            while True:
                try:
                    reply = device._command("CAL:HELLO", "CAL:HELLO:", timeout_s=0.75)
                    fields = reply.line.split(":")
                    if len(fields) != 6 or fields[2] != "1" or fields[3] not in ("x3", "x4"):
                        raise HardwareError(f"unsupported calibration handshake: {reply.line}")
                    device.profile = fields[3]
                    device.firmware_version = fields[4]
                    return device
                except TimeoutError:
                    if time.monotonic() >= deadline:
                        raise HardwareError(
                            f"calibration firmware did not answer on {port}; flash the PlatformIO calibration environment"
                        )
        except BaseException:
            device.close()
            raise

    @property
    def artifact_directory(self) -> Path:
        return self._artifact_directory

    def _record(self, direction: str, line: str, host_time_ns: int) -> None:
        event = {"direction": direction, "hostTimeNs": host_time_ns, "line": line}
        with self._condition:
            self._events.write(json.dumps(event, separators=(",", ":")) + "\n")
            self._events.flush()

    def _read_serial(self) -> None:
        pending = bytearray()
        try:
            while not self._closed:
                try:
                    raw = self._serial.readline()
                except (OSError, TypeError):
                    break
                if not raw:
                    continue
                self._serial_log.write(raw)
                self._serial_log.flush()
                pending.extend(raw)
                while (newline := pending.find(b"\n")) >= 0:
                    host_time_ns = time.monotonic_ns()
                    raw_line = bytes(pending[:newline]).rstrip(b"\r")
                    del pending[: newline + 1]
                    line = raw_line.decode("utf-8", errors="replace")
                    self._record("device", line, host_time_ns)
                    with self._condition:
                        self._recent_device_lines.append((host_time_ns, line))
                        if line.startswith("CAL:") and not line.startswith("CAL:BENCH:"):
                            self._replies.append((host_time_ns, line))
                        self._condition.notify_all()
        finally:
            self._disconnected_host_time_ns = time.monotonic_ns()
            self._reader_stopped.set()
            with self._condition:
                self._condition.notify_all()

    def _wait_for_reply(self, command: str, expected_prefix: str, *, timeout_s: float) -> HardwareReply:
        deadline = time.monotonic() + timeout_s
        with self._condition:
            while True:
                for index, (host_time_ns, line) in enumerate(self._replies):
                    if line.startswith("CAL:ERROR:"):
                        del self._replies[index]
                        raise HardwareError(f"device rejected {command!r}: {line}")
                    if line.startswith(expected_prefix):
                        del self._replies[index]
                        try:
                            device_time_ms = int(line.rsplit(":", 1)[1])
                        except (IndexError, ValueError) as error:
                            raise HardwareError(f"malformed calibration reply: {line}") from error
                        return HardwareReply(line, host_time_ns, device_time_ms)
                if self._reader_stopped.is_set():
                    raise HardwareError("serial device disconnected")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"timed out waiting for {expected_prefix}")
                self._condition.wait(remaining)

    def _command(
        self,
        command: str,
        expected_prefix: str,
        *,
        timeout_s: float = 5.0,
        event_command: str | None = None,
    ) -> HardwareReply:
        if self._closed:
            raise HardwareError("hardware device is closed")
        wire = f"CMD:{command}\n".encode("ascii")
        sent_ns = time.monotonic_ns()
        with self._condition:
            self._replies.clear()
        displayed_command = command if event_command is None else event_command
        self._record("host", f"CMD:{displayed_command}", sent_ns)
        try:
            self._serial.write(wire)
            self._serial.flush()
        except OSError as error:
            raise HardwareError(f"serial write failed: {error}") from error
        return self._wait_for_reply(displayed_command, expected_prefix, timeout_s=timeout_s)

    def button_down(self, control: PhysicalControl) -> HardwareReply:
        return self._command(f"CAL:BUTTON:{control}:DOWN", f"CAL:BUTTON:{control}:DOWN:", timeout_s=300)

    def button_up(self, control: PhysicalControl) -> HardwareReply:
        return self._command(f"CAL:BUTTON:{control}:UP", f"CAL:BUTTON:{control}:UP:", timeout_s=300)

    def button_pulse(self, control: PhysicalControl, *, hold_ms: int) -> HardwareReply:
        if hold_ms < 10 or hold_ms > 10_000:
            raise ValueError("hold_ms must be between 10 and 10000")
        return self._command(
            f"CAL:BUTTON:{control}:PULSE:{hold_ms}",
            f"CAL:BUTTON:{control}:PULSE:{hold_ms}:",
            timeout_s=300,
        )

    def press(self, control: PhysicalControl, *, hold_ms: int = 80, settle_ms: int = 80) -> None:
        if hold_ms < 10 or settle_ms < 0:
            raise ValueError("hold_ms must be at least 10 and settle_ms must be non-negative")
        self.button_down(control)
        time.sleep(hold_ms / 1000)
        self.button_up(control)
        time.sleep(settle_ms / 1000)

    def mark(self, name: str) -> HardwareReply:
        return self._command(f"CAL:MARK:{name}", f"CAL:MARK:{name}:", timeout_s=900)

    def wait_for_log(
        self,
        text: str,
        *,
        after_host_time_ns: int = 0,
        timeout_s: float = 300.0,
    ) -> HardwareReply:
        """Wait for a firmware log line without issuing another device command."""
        if not text:
            raise ValueError("log text must not be empty")
        if after_host_time_ns < 0 or timeout_s <= 0:
            raise ValueError("after_host_time_ns must be non-negative and timeout_s must be positive")
        deadline = time.monotonic() + timeout_s
        with self._condition:
            while True:
                for host_time_ns, line in self._recent_device_lines:
                    if host_time_ns >= after_host_time_ns and text in line:
                        match = re.match(r"\[(\d+)]", line)
                        device_time_ms = int(match.group(1)) if match is not None else -1
                        return HardwareReply(line, host_time_ns, device_time_ms)
                if self._reader_stopped.is_set():
                    raise HardwareError("serial device disconnected")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"timed out waiting for device log containing {text!r}")
                self._condition.wait(remaining)

    def clear_epub_cache(self, path: str) -> HardwareReply:
        if not path.startswith("/") or ".." in path or not path.lower().endswith(".epub"):
            raise ValueError("EPUB path must be an absolute device path without parent traversal")
        return self._command(f"CAL:CACHE:CLEAR:{path}", f"CAL:CACHE:CLEARED:{path}:", timeout_s=300)

    def upload_file(self, source: str | Path, path: str) -> HardwareReply:
        """Provision one EPUB on the calibration SD through a verified temporary file."""
        if not path.startswith("/") or ".." in path or ":" in path or not path.lower().endswith(".epub"):
            raise ValueError("upload path must be an absolute EPUB path without parent traversal or ':'")
        source_path = Path(source)
        byte_count = source_path.stat().st_size
        if not 1 <= byte_count <= 64 * 1024 * 1024:
            raise ValueError("calibration uploads must contain 1..67108864 bytes")
        checksum = _FNV_OFFSET_BASIS
        with source_path.open("rb") as input_file:
            while chunk := input_file.read(64 * 1024):
                for value in chunk:
                    checksum ^= value
                    checksum = checksum * _FNV_PRIME & 0xFFFFFFFFFFFFFFFF
        checksum_text = f"{checksum:016x}"
        command = f"CAL:UPLOAD:BEGIN:{path}:{byte_count}:{checksum_text}"
        self._command(command, f"CAL:UPLOAD:READY:{path}:{byte_count}:{checksum_text}:", timeout_s=30)
        offset = 0
        final_reply: HardwareReply | None = None
        with source_path.open("rb") as input_file:
            while chunk := input_file.read(96):
                chunk_command = f"CAL:UPLOAD:CHUNK:{offset}:{chunk.hex()}"
                offset += len(chunk)
                if offset == byte_count:
                    final_reply = self._command(
                        chunk_command,
                        f"CAL:UPLOADED:{path}:{byte_count}:{checksum_text}:",
                        timeout_s=30,
                        event_command=f"CAL:UPLOAD:CHUNK:{offset - len(chunk)}:{len(chunk)}-bytes",
                    )
                else:
                    self._command(
                        chunk_command,
                        f"CAL:UPLOAD:CHUNK:ACK:{offset}:",
                        timeout_s=30,
                        event_command=f"CAL:UPLOAD:CHUNK:{offset - len(chunk)}:{len(chunk)}-bytes",
                    )
        if final_reply is None:
            raise HardwareError("calibration upload produced no final reply")
        return final_reply

    def open_epub(self, path: str) -> HardwareReply:
        if not path.startswith("/") or ".." in path or not path.lower().endswith(".epub"):
            raise ValueError("EPUB path must be an absolute device path without parent traversal")
        return self._command(f"CAL:OPEN:{path}", f"CAL:OPEN:{path}:", timeout_s=900)

    def select_settings_on_home(self) -> HardwareReply:
        return self._command("CAL:HOME:SETTINGS", "CAL:HOME:SETTINGS:", timeout_s=300)

    def select_files_on_home(self) -> HardwareReply:
        return self._command("CAL:HOME:FILES", "CAL:HOME:FILES:", timeout_s=300)

    def benchmark_sd(self, byte_count: int, *, iterations: int = 10) -> HardwareReply:
        if not 4096 <= byte_count <= 4 * 1024 * 1024 or not 1 <= iterations <= 32:
            raise ValueError("SD benchmark requires 4096..4194304 bytes and 1..32 iterations")
        timeout_s = max(180.0, byte_count * iterations * 2 / 48_000 + 60)
        return self._command(
            f"CAL:BENCH:SD:{byte_count}:{iterations}",
            f"CAL:BENCH-END:SD:{byte_count}:{iterations}:",
            timeout_s=timeout_s,
        )

    def benchmark_directory(self, *, iterations: int = 10) -> HardwareReply:
        if not 1 <= iterations <= 10:
            raise ValueError("directory benchmark requires 1..10 iterations")
        return self._command(
            f"CAL:BENCH:DIR:{iterations}",
            f"CAL:BENCH-END:DIR:{iterations}:",
            timeout_s=60,
        )

    def benchmark_panel(self, mode: Literal["fast", "half", "full"], *, iterations: int = 5) -> HardwareReply:
        if not 1 <= iterations <= 10:
            raise ValueError("panel benchmark requires 1..10 iterations")
        return self._command(
            f"CAL:BENCH:PANEL:{mode}:{iterations}",
            f"CAL:BENCH-END:PANEL:{mode}:{iterations}:",
            timeout_s=180,
        )

    def benchmark_grayscale(self, primary: Literal["fast", "half"], *, iterations: int = 5) -> HardwareReply:
        if not 1 <= iterations <= 10:
            raise ValueError("grayscale benchmark requires 1..10 iterations")
        return self._command(
            f"CAL:BENCH:GRAY:{primary}:{iterations}",
            f"CAL:BENCH-END:GRAY:{primary}:{iterations}:",
            timeout_s=180,
        )

    def sleep(self, seconds: float) -> None:
        if seconds < 0:
            raise ValueError("seconds must be non-negative")
        time.sleep(seconds)

    @property
    def expected_disconnect(self) -> bool:
        return self._terminal_reason == "device-disconnect"

    @property
    def terminal_reason(self) -> str | None:
        return self._terminal_reason

    @property
    def disconnected_host_time_ns(self) -> int | None:
        return self._disconnected_host_time_ns

    def wait_for_disconnect(self, *, timeout_s: float = 15.0) -> None:
        """Accept one intentional terminal USB disconnect, such as hardware deep sleep."""
        if timeout_s <= 0:
            raise ValueError("timeout_s must be positive")
        self._terminal_reason = "device-disconnect"
        if not self._reader_stopped.wait(timeout_s):
            self._terminal_reason = None
            raise TimeoutError("timed out waiting for the serial device to disconnect")
        disconnected_ns = self._disconnected_host_time_ns
        if disconnected_ns is None:
            raise HardwareError("serial reader stopped without a disconnect timestamp")
        self._record("host", "DEVICE:DISCONNECTED", disconnected_ns)

    def wait_for_terminal_log(
        self, text: str, *, timeout_s: float = 15.0, disconnect_grace_s: float = 1.0
    ) -> HardwareReply:
        """Accept a terminal firmware boundary when USB remains enumerated."""
        if disconnect_grace_s < 0:
            raise ValueError("disconnect_grace_s must be non-negative")
        reply = self.wait_for_log(text, timeout_s=timeout_s)
        if self._reader_stopped.wait(disconnect_grace_s):
            self._terminal_reason = "device-disconnect"
            disconnected_ns = self._disconnected_host_time_ns
            if disconnected_ns is not None:
                self._record("host", "DEVICE:DISCONNECTED", disconnected_ns)
        else:
            self._terminal_reason = "device-log"
            self._record("host", f"DEVICE:TERMINAL:{text}", time.monotonic_ns())
        return reply

    def close(self) -> None:
        if self._closed:
            return
        if self._terminal_reason is None and not self._reader_stopped.is_set():
            for control in cast(
                tuple[PhysicalControl, ...], ("back", "confirm", "left", "right", "up", "down", "power")
            ):
                try:
                    self._command(
                        f"CAL:BUTTON:{control}:UP",
                        f"CAL:BUTTON:{control}:UP:",
                        timeout_s=1,
                    )
                except (HardwareError, TimeoutError):
                    break
        self._closed = True
        self._serial.close()
        self._reader.join(timeout=1)
        self._serial_log.close()
        self._events.close()

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        del exc_type, exc_value, traceback
        self.close()


class WebcamRecorder:
    """Process-owning FFmpeg webcam recorder."""

    def __init__(
        self,
        camera: str,
        output: str | Path,
        *,
        input_format: CameraInput,
        fps: int = 60,
        video_size: str = "1920x1080",
        rotation: Literal[0, 90, 180, 270] = 0,
        input_codec: str | None = None,
        ffmpeg: str = "ffmpeg",
    ) -> None:
        if fps <= 0:
            raise ValueError("fps must be positive")
        ffmpeg_path = shutil.which(ffmpeg)
        if ffmpeg_path is None:
            raise HardwareError("webcam recording requires ffmpeg on PATH")
        self.output = Path(output)
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self._stderr_path = self.output.with_suffix(".ffmpeg.log")
        self._stderr: IO[bytes] | None = None
        self._process: subprocess.Popen[bytes] | None = None
        self._progress_thread: threading.Thread | None = None
        self._stderr_thread: threading.Thread | None = None
        self.started_ns: int | None = None
        self.first_frame_decoded_ns: int | None = None
        self.first_frame_ns: int | None = None
        self.stopped_ns: int | None = None
        self.decoded_frame_pts_seconds: list[float] = []
        source = camera
        if input_format == "dshow":
            source = f"video={camera}"
        elif input_format == "avfoundation" and ":" not in camera:
            source = f"{camera}:none"
        arguments = [ffmpeg_path, "-hide_banner", "-loglevel", "info", "-y", "-f", input_format]
        if input_codec is not None:
            arguments.extend(("-input_format", input_codec))
        arguments.extend(("-framerate", str(fps), "-video_size", video_size, "-i", source, "-an"))
        rotation_filter = {0: None, 90: "transpose=clock", 180: "hflip,vflip", 270: "transpose=cclock"}[rotation]
        filters = [value for value in (rotation_filter, "showinfo") if value is not None]
        arguments.extend(("-vf", ",".join(filters)))
        arguments.extend(("-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p"))
        arguments.extend(("-progress", "pipe:1", "-stats_period", "0.05", "-nostats"))
        arguments.extend(("-movflags", "+faststart", str(self.output)))
        self.arguments = arguments
        self.input_format = input_format
        self.camera = camera
        self.fps = fps
        self.video_size = video_size
        self.rotation = rotation

    def start(self) -> None:
        if self._process is not None:
            raise HardwareError("webcam recorder is already running")
        self._stderr = self._stderr_path.open("wb")
        self.started_ns = time.monotonic_ns()
        self._process = subprocess.Popen(self.arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if self._process.stdout is None or self._process.stderr is None:
            raise HardwareError("FFmpeg diagnostic pipes are unavailable")
        self._progress_thread = threading.Thread(
            target=self._read_progress, args=(self._process.stdout,), name="crosspoint-camera-progress", daemon=True
        )
        self._stderr_thread = threading.Thread(
            target=self._read_stderr, args=(self._process.stderr,), name="crosspoint-camera-stderr", daemon=True
        )
        self._progress_thread.start()
        self._stderr_thread.start()
        time.sleep(0.5)
        if self._process.poll() is not None:
            self.stop()
            message = self._stderr_path.read_text(encoding="utf-8", errors="replace").strip()
            raise HardwareError(f"ffmpeg could not start camera {self.camera!r}: {message}")

    def _read_progress(self, stream: IO[bytes]) -> None:
        for raw in iter(stream.readline, b""):
            if self.first_frame_ns is not None or not raw.startswith(b"frame="):
                continue
            try:
                frame = int(raw.partition(b"=")[2].strip())
            except ValueError:
                continue
            if frame > 0:
                self.first_frame_ns = time.monotonic_ns()

    def _read_stderr(self, stream: IO[bytes]) -> None:
        for raw in iter(stream.readline, b""):
            frame = _SHOWINFO_FRAME_RE.search(raw) if b"showinfo" in raw else None
            if b"showinfo" in raw and frame is not None:
                if self.first_frame_decoded_ns is None and int(frame.group(1)) == 0:
                    self.first_frame_decoded_ns = time.monotonic_ns()
                self.decoded_frame_pts_seconds.append(float(frame.group(2)))
            if self._stderr is not None:
                self._stderr.write(raw)
                self._stderr.flush()

    def source_cadence(self) -> dict[str, object]:
        return summarize_source_cadence(self.decoded_frame_pts_seconds)

    def stop(self) -> None:
        process = self._process
        self._process = None
        if process is not None and process.poll() is None:
            try:
                if process.stdin is not None:
                    process.stdin.write(b"q\n")
                    process.stdin.flush()
                process.wait(timeout=10)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                process.send_signal(signal.SIGINT)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            finally:
                if process.stdin is not None:
                    process.stdin.close()
        if self._progress_thread is not None:
            self._progress_thread.join(timeout=2)
            self._progress_thread = None
        if self._stderr_thread is not None:
            self._stderr_thread.join(timeout=2)
            self._stderr_thread = None
        if process is not None and process.stdout is not None:
            process.stdout.close()
        if process is not None and process.stderr is not None:
            process.stderr.close()
        self.stopped_ns = time.monotonic_ns()
        if self._stderr is not None:
            self._stderr.close()
            self._stderr = None
        if process is not None and process.returncode not in (0, 255):
            message = self._stderr_path.read_text(encoding="utf-8", errors="replace").strip()
            raise HardwareError(f"ffmpeg recording failed (code {process.returncode}): {message}")

    def __enter__(self) -> Self:
        self.start()
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        del exc_type, exc_value, traceback
        self.stop()
