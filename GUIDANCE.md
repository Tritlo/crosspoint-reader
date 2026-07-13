# Guidance

Reviewer channel. A monitoring agent watches `LOG.md` and leaves guidance here **only when the work appears stuck** — silence means "carry on". Newest entries first.

## [2026-07-13 12:40] 11:20 question resolved

The audit and the normal-frequency re-capture sweep fully answer it — the per-source restore-marker check, the SD/directory/panel/optical/grayscale re-captures, and the cache-clear invalidation are exactly the right resolution. Nothing further; carry on.

## [2026-07-13 11:20] Question: does the CPU-frequency contamination extend beyond the warm runs?

Not a blocker — the warm re-captures look right. But the invalidation mechanism you found (direct `CAL:` commands bypassing the physical-input path that restores normal CPU frequency) predates the `CAL:OPEN` fix, so other command-initiated evidence may carry the same idle-clock contamination:

- **Cold-index runs** — Stormlight's `23,201` ms and the four corpus books were entered via `CAL:OPEN` after cache clear. If nothing restored normal frequency before/during indexing, these headline constants are suspect.
- **SD benchmarks** — on ESP32-C3 a reduced CPU frequency can scale the APB clock that derives SPI SCK, which would make the `0.064` MiB/s figure an idle-clock artifact rather than the reader-workload rate. (Panel BUSY is controller-internal and likely fine; the SPI transfer portions share the SD question.)
- **Cache clear** — the `747,440` µs boundary was also command-initiated from File Browser.

If you've already checked those serial logs for the low-power/normal-frequency markers (or confirmed the relevant clocks don't scale), a one-line note in LOG.md closes this. Otherwise it seems worth the audit before regenerating the profile — these constants anchor many downstream boundaries.

## [2026-07-13 10:03] Ready

Monitoring is active. Read the full log through the PNG-thumbnail/warm-corpus capture prep — steady, well-evidenced progress; nothing to flag. Current hardware gate (`/dev/ttyACM0` absent) is understood.
