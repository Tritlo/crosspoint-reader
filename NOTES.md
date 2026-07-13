# Reviewer Notes

Advice from the tracking agent, newest entry first. Written against `docs/emulator-plan.md` and `LOG.md`.

## 2026-07-12 04:55 — All reviewer threads closed

The commit-granularity question is settled the right way: the deviation is recorded in the plan, published history stays, and the cleanup landed as its own reviewable commit (`f42919ca`). Dropping the misleading `wait.storageIdle` and updating the plan to state the synchronous-storage contract is also the kind of plan-faithfulness this review was pushing for. Nothing further from me — the remaining items (CI observation via `workflow_dispatch`/draft PR, device SD hash cross-check, phase-7 calibration) are all owner-gated and correctly tracked in the log.

## 2026-07-12 04:25 — Post-milestone cleanup looks good; commit granularity and CI trigger

The cleanup pass is quality work — pulling the emulator-only declaration out of shared `MappedInputManager`, compiling render-generation tracking only for the emulator (both tighten the plan's "no emulator conditionals in application code" boundary), and recording the cppcheck failure honestly instead of weakening the gate. Two items:

- **The milestone landed as one commit (`440efd3d`), not phase-sized commits.** The plan calls for independently reviewable phases, and the reviewer note asked for a split before committing. Since the branch has been pushed, splitting now means a soft reset + re-commit in slices + force-push to the fork branch — mechanically safe on a personal branch but strictly the owner's call now. If the owner prefers to keep `440efd3d`, record the deviation in the plan.
- **CI never ran because the workflow only triggers on `master` pushes or PRs.** Cheapest observable options: add `workflow_dispatch:` (manual trigger, no behavior change for others) or open a draft PR from the fork branch. Either needs owner authorization; the milestone's CI criterion stays "tracked, not claimed" until one happens.
- Hash cross-check remains blocked on a real device `.crosspoint` SD dump — nothing local can unblock it.

## 2026-07-12 03:55 — Milestone 1 locally complete; commit the phases

All local milestone-1 acceptance criteria check out from my reading, the pixel-plane golden switch resolves my zlib concern, and the honest "tracked, not claimed" handling of the CI run and device hash check is exactly right. One process item before anything else touches this tree:

- **Phases 2–6 are a single 92-file uncommitted diff.** Only phase 1 has a commit (`fd6287eb`). The plan requires each phase to be independently buildable and reviewable — that means phase-sized commits (storage/boot, display/panel, input/signals, Python/vertical slice, recording/presentation), with the `freeink-sdk` patch staying separate for its own review. Splitting now, while the phase boundaries are fresh, is much cheaper than reconstructing them later — and an accidental `git checkout`/tool mishap currently has a very large blast radius.
- After commits: the GitHub CI run and the push itself remain owner-authorized decisions.

## 2026-07-12 03:41 — Phase 5 nearly done; golden-comparison robustness

The vertical slice landing with two panel-model bugs caught by *visual review before goldens* is the process working as designed. Phase 4's outstanding criterion (externally driven boot → Home → refresh waits) is now demonstrated through the Python client. Three notes:

- **Don't let goldens depend on system zlib's compressed bytes.** Level-9 DEFLATE output is deterministic for a fixed zlib build, but not across zlib versions/implementations (zlib-ng and historical 1.2.x changes produce different streams for identical input). If golden assertions compare encoded PNG bytes, a CI or distro zlib bump breaks every golden with no visual change. Compare decoded pixel content (or a stored hash of the raw pixel planes) instead, and keep the encoded PNGs as artifacts only.
- **CI green is a claim that needs a real run.** The workflow exists locally; the milestone's "Ubuntu CI verifies the headless scenario" criterion is only met once it has actually executed on GitHub. That requires a push, which needs the owner's go-ahead — flag it rather than pushing.
- Still open from earlier: one `devicePathHash32` cross-check against a real device SD when one is available.

## 2026-07-12 03:27 — Phase 4 input-path check satisfied

The check I pre-announced is satisfied: injection feeds ADC-ladder/power-pin samples through the SDK's real `InputManager`, the 5 ms two-sample debounce, native `HalGPIO`, and `MappedInputManager` — and the swapped-mapping fixture plus `pressAction` resolution prove the remap path end-to-end. Queue support closes the last FreeRTOS scheduling point from the plan. No concerns. Remaining for phase-4 exit: the boot → Home → panel-refresh wait sequence driven externally (fine if that lands with the phase-5 Python client, but note it when claiming phase 4 done).

## 2026-07-12 03:13 — Phase 3 confirmed closed

Both gaps from the previous note are resolved (byte-identical captures demonstrated on both profiles; PNG initial-state seeding landed, with a nice `istreambuf_iterator`/`eofbit` catch). No reservations — phase 3 exit criteria are met from my reading too. Nothing to add for phase 4 beyond the plan itself; the one thing I'll be checking at its exit is that injected input flows through the real native GPIO → debounce → `MappedInputManager` path rather than any direct-injection shortcut.

## 2026-07-12 03:06 — Phase 3 nearly complete; two gaps against the plan

Strong execution — real drivers on a native bus with the SDK diff kept unpinned for independent review, exactly per plan. Two items before calling phase 3 done:

- **The third exit criterion isn't demonstrated yet**: "repeated runs produce byte-identical canonical captures." The PNGs are verified valid and profile-distinct, but the log doesn't show two identical runs producing byte-identical capture files (the PNG encoder itself must be deterministic — no timestamps/ancillary chunks). Cheap to add alongside the existing trace-identity checks.
- **Initial panel state from a supplied PNG** is in the plan's panel-behavior section (white/black/PNG); white/black landed. If PNG-seeding is deliberately deferred, note it in the plan's deferred list so the plan stays faithful.

## 2026-07-12 02:52 — Phase 2 exit criteria met; phase 3 reminders

From my reading, all three phase-2 exit criteria are now demonstrated (fixture immutability, reset/fresh-run semantics, firmware round-trip of `book.bin`/`progress.bin` through shared readers) plus X4 boot parity and storage tracing. The recursive `HalFile::write` catch is exactly the kind of bug the round-trip criterion exists for — good. The real-device hash cross-check from the previous note remains open; fine to defer until a device SD is available, but track it.

For phase 3 (panel model), two plan constraints worth front-loading:

- **`freeink-sdk` changes land independently first.** The native `EpdBus` work crosses the submodule boundary; the plan requires SDK changes to be verified on their own before this repo's submodule pin moves. Keep emulator-repo commits and SDK commits cleanly separated from the start.
- **Reuse the real driver state machines.** Resist reimplementing refresh promotion/grayscale logic in the panel model — the plan's panel-drift risk. The bus should interpret the real `Uc8253X3Driver`/`Ssd1677Driver` traffic.

## 2026-07-12 02:45 — Phase 2 progress: hash fix looks right, one verification suggestion

Excellent batch — fixtures with manifest identity hashing, directory-backed `HalStorage` with confinement tests, the full app graph compiling natively, and a real X3 boot to Home. The `devicePathHash32` approach is exactly the migration-free option. Two notes:

- **Cross-verify the hash against a real device, not just host reasoning.** The known-vector tests prove self-consistency, but the contract is "matches what the ESP32-C3 toolchain's `std::hash` produced." The cheapest ground truth: take a real device SD card (or any existing `.crosspoint/` dump) and check that `devicePathHash32(<book path>)` reproduces an actual existing `epub_<N>` directory name. One matching pair from real hardware turns the assumption into evidence; worth doing before cache-dependent goldens are built on top.
- **Remaining phase-2 exit criteria** for the checklist: reset preserves run storage / fresh run starts from fixture, emulator-created persisted files round-trip through firmware readers (settings, progress.bin, book.bin), and an X4 boot-to-Home to match the X3 one.

## 2026-07-12 02:15 — Phase 2 ABI audit: one confirmed hazard, core looks clean

Both notes from the last entry are resolved (mutex/recursive-mutex support, priority + creation-order ties). Ahead of the phase-2 "audit persistence for host-ABI dependencies" item, I did a scan of the shared serialization paths. Findings:

- **Confirmed hazard — `std::hash<std::string>` names the cache dirs.** `Epub.h:43`, `Xtc.h:32`, `Txt.cpp:10`, and `EpubReaderActivity.cpp:133` build `.crosspoint/epub_<std::hash(filepath)>` paths. libstdc++'s `_Hash_bytes` is a different algorithm for 4-byte vs 8-byte `size_t`, so the 64-bit host computes different directory names than the ESP32-C3 for the same book path. Consequences: a fixture prepared from a real device SD has caches the emulator can't find (silent full re-parse), and emulator-created cache dirs don't round-trip to device — which is a phase-2 exit criterion. Fix belongs in shared code per the plan, but note the migration trap: changing the algorithm orphans every existing device cache, and `progress.bin` (reading progress) lives inside those dirs. The migration-free option is a project-owned hash function that replicates libstdc++'s 32-bit `_Hash_bytes` output exactly, so device names stay unchanged and the host matches. Whichever way, make it an explicit decision, not an accident of `std::hash`.
- **Serialization core is clean.** `lib/Serialization/Serialization.h` uses fixed-width `uint32_t` string lengths; `book.bin` writes are all `uint32_t`-typed expressions (checked `BookMetadataCache.cpp` header/LUT/spine paths, incl. `cumulativeSize` being `uint32_t` in the struct); settings are `uint8_t` fields via `readPod`. Remaining audit focus: grep `writePod`/`readPod` call sites for `size_t`/`long`/`time_t`-typed *expressions* (integer promotion in sums is silent) and any whole-struct writes (padding differs).

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
