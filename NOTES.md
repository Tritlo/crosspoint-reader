# Reviewer Notes

Advice from the tracking agent, newest entry first. Written against `docs/emulator-plan.md` and `LOG.md`.

## 2026-07-12 02:10 — Phase 1 exit criteria met

All three open items from the previous note are now done (X4 handshake, two-run byte-identical traces, scheduler + watchdog with tests). Phase 1 looks complete; good to move to phase 2. Two things to have on the radar:

- **Semaphore/mutex support before boot**: `DeterministicScheduler` currently exposes tasks/notifications/delay/yield only. Booting the real app in phase 2 pulls in `HalStorage`, which takes `storageMutex` on every file operation, and the plan lists semaphore and queue as required scheduling points. Add those primitives (host-side) before wiring the real boot path, or the first `Storage.` call will have nowhere to go.
- **Tie-breaking vs plan wording**: the plan says runnable ties resolve by *priority and creation order*; the log and the API (no priority parameter on `createTask`) suggest round-robin without priorities. Both are deterministic, but CrossPoint creates tasks at explicit priorities, so either add a priority argument when the FreeRTOS compat wrapper lands or amend the plan to say round-robin — keep the plan faithful to the implementation.

## 2026-07-12 01:55 — Phase 1 checkpoint

Direction looks right: runner/protocol/clock live under `emulator/native/` with only a guarded 5-line `main()` shim in `src/platform/`, ArduinoJson matches the firmware pin (7.4.2), no wall-clock usage in emulator sources, and the manifest already records seed/RTC/timing-profile versions. No corrections needed.

Reminders before calling phase 1 done (plan exit criteria):

- **X4 handshake**: only an X3 smoke run is logged so far; verify both profiles launch and handshake.
- **Determinism check**: run two identical no-op runs and diff the normalized event sequences.
- **Scheduler + watchdog**: the deterministic single-core FreeRTOS compatibility layer and the wall-time deadlock watchdog (with scheduler diagnostics) are phase-1 scope and not yet in the tree.
- Minor, can wait for phase 2: plan wants fixture identity and environment (locale, timezone, filesystem ordering) in `manifest.json` alongside the seed/RTC already there.
