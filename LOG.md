# Emulator Implementation Log

## 2026-07-12

- Started implementation from `fdd1ad30` on branch `emulator`; worktree was clean and matched `origin/emulator`.
- Confirmed `NOTES.md` does not yet exist.
- Confirmed `uv 0.9.5` and FFmpeg 6.1.1 are installed; `pio` is not currently on `PATH`.
- Began phase 1 from `docs/emulator-plan.md`: native execution spine, framed protocol, deterministic time, and scheduler compatibility.
- Added the PlatformIO `emulator` native target and a small host executable with explicit `--device` and artifact-directory configuration.
- Added Content-Length framed JSON-RPC over stdio, protocol/version negotiation, an uncalibrated timing marker, deterministic clock controls, and canonical manifest/event artifacts.
- Installed PlatformIO's native platform and native ArduinoJson dependency through the existing PlatformIO cache. Fixed the resulting missing `<optional>` include.
- Verified an X3 protocol smoke run: four ordered responses, a 125 ms simulated-time advance, and matching manifest/event files.
- Verified the existing `default` ESP32-C3 firmware still builds successfully (`5267981` bytes flash, `49532` bytes RAM reported).
- Added a deterministic, logically single-core scheduler with serialized host task dispatch, simulated delays/timeouts, notifications, priority/creation-order scheduling, task diagnostics, and a 5 s wall-time watchdog.
- Added native PlatformIO tests for deterministic task order, notification timeouts, and watchdog termination. All 3 tests pass; the watchdog exits a stalled child with code 70.
- Verified X3 and X4 both handshake and that two identical initialize/state/shutdown runs produce byte-identical event traces for each profile.
- Read the phase-1 checkpoint in `NOTES.md`; its X4, determinism, scheduler, and watchdog reminders are now addressed.
- Added the narrow Arduino/FreeRTOS compatibility surface used by the app today: simulated `millis`/`micros`/delay/yield, pinned task creation, notifications, mutexes, recursive mutexes, mutex ownership/peek, and critical sections.
- Added a weak-linked shared `setup()`/`loop()` lifecycle runner. Setup may auto-advance through deterministic delays; normal loop execution is bounded by explicit caller time advances so periodic tasks cannot run forever.
- Changed protocol/trace sequencing so recorded events and responses consume one shared monotonically increasing sequence. Reverified byte-identical X3/X4 runs; response sequences are `2,4,5,7` around three trace events.
- Native emulator suite now has 7 passing tests covering scheduling, priority/creation-order ties, bounded time, lifecycle, FreeRTOS primitives, and watchdog behavior.
- Applied the phase-1 reviewer follow-up before commit: FreeRTOS task priority is now preserved and runnable tasks resolve by priority then creation order, matching the plan.
