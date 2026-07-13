from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
from collections.abc import Mapping
from pathlib import Path
from types import TracebackType
from typing import IO, Literal, Self, cast

DeviceProfile = Literal["x3", "x4"]
PhysicalControl = Literal["back", "confirm", "left", "right", "up", "down", "power"]
Action = Literal["back", "confirm", "left", "right", "up", "down", "power", "page_back", "page_forward"]


class EmulatorError(RuntimeError):
    pass


class ProtocolError(EmulatorError):
    def __init__(self, code: int, message: str) -> None:
        super().__init__(f"emulator protocol error {code}: {message}")
        self.code = code
        self.message = message


class WaitTimeout(ProtocolError):
    pass


class Emulator:
    """A synchronous, process-owning CrossPoint emulator session."""

    def __init__(self) -> None:
        self._process: subprocess.Popen[bytes] | None = None
        self._stderr: IO[bytes] | None = None
        self._request_id = 0
        self._device: DeviceProfile = "x3"
        self._executable = Path()
        self._timing_profile: Path | None = None
        self._rtc_start = "2000-01-01T00:00:00Z"
        self._seed = 0
        self._keep_artifacts = False
        self._temporary_artifacts = False
        self._artifact_root = Path()
        self._reset_count = 0
        self.artifact_directory = Path()
        self.run_directories: list[Path] = []
        self.handshake: dict[str, object] = {}

    @classmethod
    def launch(
        cls,
        device: DeviceProfile,
        *,
        sd: str | Path | None = None,
        artifacts: str | Path | None = None,
        executable: str | Path | None = None,
        rtc_start: str = "2000-01-01T00:00:00Z",
        seed: int = 0,
        initial_panel: Literal["white", "black"] = "white",
        initial_panel_png: str | Path | None = None,
        timing_profile: str | Path | None = None,
        keep_artifacts: bool = False,
    ) -> Self:
        emulator = cls()
        emulator._device = device
        emulator._executable = Path(executable) if executable is not None else emulator._default_executable()
        emulator._rtc_start = rtc_start
        emulator._seed = seed
        emulator._keep_artifacts = keep_artifacts
        if timing_profile is not None:
            emulator._timing_profile = Path(timing_profile)
        elif device == "x4":
            emulator._timing_profile = (
                Path(__file__).resolve().parents[4] / "emulator" / "profiles" / "x4-hardware-2026-07-12-v1.json"
            )
        else:
            emulator._timing_profile = None
        if artifacts is None:
            emulator._artifact_root = Path(tempfile.mkdtemp(prefix=f"crosspoint-{device}-"))
            emulator._temporary_artifacts = True
        else:
            emulator._artifact_root = Path(artifacts)
            emulator._artifact_root.mkdir(parents=True, exist_ok=True)
        try:
            emulator._start_process(
                emulator._artifact_root,
                sd=Path(sd) if sd is not None else None,
                initial_panel=initial_panel,
                initial_panel_png=Path(initial_panel_png) if initial_panel_png is not None else None,
            )
        except BaseException:
            emulator._finish_process()
            raise
        return emulator

    @staticmethod
    def _default_executable() -> Path:
        return Path(__file__).resolve().parents[4] / ".pio" / "build" / "emulator" / "program"

    def _start_process(
        self,
        artifacts: Path,
        *,
        sd: Path | None,
        initial_panel: Literal["white", "black"] = "white",
        initial_panel_png: Path | None = None,
    ) -> None:
        if not self._executable.is_file():
            raise EmulatorError(f"emulator executable not found: {self._executable}")
        artifacts.mkdir(parents=True, exist_ok=True)
        arguments = [
            str(self._executable),
            "--device",
            self._device,
            "--artifacts",
            str(artifacts),
            "--rtc-start",
            self._rtc_start,
            "--seed",
            str(self._seed),
        ]
        if sd is not None:
            arguments.extend(("--sd", str(sd)))
        if self._timing_profile is not None:
            arguments.extend(("--timing-profile", str(self._timing_profile)))
        if initial_panel_png is not None:
            arguments.extend(("--panel-initial-png", str(initial_panel_png)))
        else:
            arguments.extend(("--panel-initial", initial_panel))

        self.artifact_directory = artifacts
        self.run_directories.append(artifacts)
        self._stderr = (artifacts / "stderr.log").open("wb")
        self._process = subprocess.Popen(arguments, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._stderr)
        self._request_id = 0
        self.handshake = self._request("initialize", {"protocolVersion": 1})
        if self.handshake.get("protocolVersion") != 1:
            raise EmulatorError(f"unsupported emulator handshake: {self.handshake!r}")

    def _request(self, method: str, params: Mapping[str, object] | None = None) -> dict[str, object]:
        process = self._require_process()
        if process.stdin is None or process.stdout is None:
            raise EmulatorError("emulator protocol pipes are unavailable")
        self._request_id += 1
        request: dict[str, object] = {
            "jsonrpc": "2.0",
            "id": self._request_id,
            "method": method,
            "params": dict(params or {}),
        }
        body = json.dumps(request, separators=(",", ":")).encode("utf-8")
        try:
            process.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body)
            process.stdin.flush()
            response = self._read_response(process.stdout)
        except (BrokenPipeError, EOFError) as error:
            raise EmulatorError(self._process_failure_message()) from error

        response_id = response.get("id")
        if response_id != self._request_id:
            raise EmulatorError(f"unexpected response id {response_id!r}, expected {self._request_id}")
        error_value = response.get("error")
        if isinstance(error_value, dict):
            error_object = cast(dict[str, object], error_value)
            code = error_object.get("code")
            message = error_object.get("message")
            if not isinstance(code, int) or not isinstance(message, str):
                raise EmulatorError(f"malformed protocol error: {error_object!r}")
            exception = WaitTimeout if code in (-32020, -32021) else ProtocolError
            raise exception(code, message)
        result = response.get("result")
        if not isinstance(result, dict):
            raise EmulatorError(f"malformed protocol result: {response!r}")
        return cast(dict[str, object], result)

    @staticmethod
    def _read_response(stream: IO[bytes]) -> dict[str, object]:
        content_length: int | None = None
        while True:
            line = stream.readline()
            if line == b"":
                raise EOFError("emulator closed its protocol stream")
            if line in (b"\n", b"\r\n"):
                break
            name, separator, value = line.partition(b":")
            if separator and name.lower() == b"content-length":
                content_length = int(value.strip())
        if content_length is None:
            raise EmulatorError("protocol response omitted Content-Length")
        body = stream.read(content_length)
        if len(body) != content_length:
            raise EOFError("emulator truncated its protocol response")
        decoded: object = json.loads(body)
        if not isinstance(decoded, dict):
            raise EmulatorError("protocol response is not a JSON object")
        return cast(dict[str, object], decoded)

    def _require_process(self) -> subprocess.Popen[bytes]:
        if self._process is None:
            raise EmulatorError("emulator session is closed")
        return self._process

    def _process_failure_message(self) -> str:
        process = self._process
        code = process.poll() if process is not None else None
        return f"emulator process exited unexpectedly (code {code}); artifacts: {self.artifact_directory}"

    def advance(self, milliseconds: int) -> dict[str, object]:
        if milliseconds < 0:
            raise ValueError("milliseconds must be non-negative")
        return self._request("clock.advance", {"microseconds": milliseconds * 1000})

    def state(self) -> dict[str, object]:
        return self._request("emulator.state")

    def button_down(self, control: PhysicalControl) -> dict[str, object]:
        return self._request("input.press", {"control": control})

    def button_up(self, control: PhysicalControl) -> dict[str, object]:
        return self._request("input.release", {"control": control})

    def action_down(self, action: Action) -> dict[str, object]:
        return self._request("input.pressAction", {"action": action})

    def action_up(self, action: Action) -> dict[str, object]:
        return self._request("input.releaseAction", {"action": action})

    def press(self, control: PhysicalControl, *, hold_ms: int = 20) -> None:
        self.button_down(control)
        self.advance(max(100, hold_ms))
        self.button_up(control)
        self.advance(20)

    def press_action(self, action: Action, *, hold_ms: int = 20) -> None:
        self.action_down(action)
        self.advance(max(100, hold_ms))
        self.action_up(action)
        self.advance(20)

    def wait_for_activity(
        self,
        activity_id: Literal[
            "boot",
            "home",
            "file_browser",
            "settings",
            "sleep",
            "reader.epub",
            "reader.epub.menu",
            "reader.epub.chapters",
            "reader.epub.percent",
        ],
        *,
        timeout_ms: int = 10_000,
        wall_timeout_ms: int = 5_000,
    ) -> dict[str, object]:
        return self._request(
            "wait.activity",
            {"activityId": activity_id, "timeoutUs": timeout_ms * 1000, "wallTimeoutMs": wall_timeout_ms},
        )

    def wait_for_render(
        self, *, after: int, timeout_ms: int = 10_000, wall_timeout_ms: int = 5_000,
    ) -> dict[str, object]:
        return self._request(
            "wait.render", {"after": after, "timeoutUs": timeout_ms * 1000, "wallTimeoutMs": wall_timeout_ms}
        )

    def wait_for_panel_idle(self, *, timeout_ms: int = 10_000, wall_timeout_ms: int = 5_000) -> dict[str, object]:
        return self._request(
            "wait.panelIdle", {"timeoutUs": timeout_ms * 1000, "wallTimeoutMs": wall_timeout_ms}
        )

    def clear_epub_cache(self, path: str) -> dict[str, object]:
        """Clear one EPUB's production cache while File Browser is idle."""
        return self._request("storage.clearEpubCache", {"path": path})

    def capture_panel(self, name: str = "panel.png") -> Path:
        return self._capture("capture.panel", name)

    def capture_framebuffer(self, name: str = "framebuffer.png") -> Path:
        return self._capture("capture.framebuffer", name)

    def screenshot(self, name: str = "screenshot.png") -> Path:
        return self._capture("capture.screenshot", name)

    def _capture(self, method: str, name: str) -> Path:
        result = self._request(method, {"name": name})
        path = result.get("path")
        if not isinstance(path, str):
            raise EmulatorError(f"capture response omitted path: {result!r}")
        return Path(path)

    def reset(self) -> dict[str, object]:
        result = self._request("input.reset")
        storage = result.get("storageDirectory")
        panel = result.get("panelPath")
        if not isinstance(storage, str) or not isinstance(panel, str):
            raise EmulatorError(f"reset response omitted retained state: {result!r}")
        self._finish_process()
        self._reset_count += 1
        next_artifacts = self._artifact_root / "resets" / str(self._reset_count)
        self._start_process(next_artifacts, sd=Path(storage), initial_panel_png=Path(panel))
        return self.handshake

    def export_video(self, output: str | Path | None = None, *, fps: int = 10) -> Path:
        from .video import export_video

        if self._process is not None:
            raise EmulatorError("close the emulator before exporting its completed trace")
        return export_video(self.artifact_directory, output, fps=fps)

    def close(self) -> None:
        process = self._process
        if process is None:
            return
        if process.poll() is None:
            try:
                self._request("shutdown")
            except EmulatorError:
                pass
        self._finish_process()

    def _finish_process(self) -> None:
        process = self._process
        self._process = None
        if process is not None:
            if process.stdin is not None:
                process.stdin.close()
            if process.stdout is not None:
                process.stdout.close()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        if self._stderr is not None:
            self._stderr.close()
            self._stderr = None

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        del exc_value, traceback
        self.close()
        if self._temporary_artifacts and exc_type is None and not self._keep_artifacts:
            shutil.rmtree(self._artifact_root)
