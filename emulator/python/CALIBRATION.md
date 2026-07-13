# X4 calibration and fidelity notes

This is the maintainer record for physical-device measurement, profile generation, and the current X4 model's evidence
and limits. User installation and automation live in [README.md](README.md).

Physical serial capture requires the calibration extra. Run the commands below through
`uv run --extra calibration crosspoint-calibrate ...`; profile and waveform analysis use the same form for consistency.

## Timing profiles

`Emulator.launch("x4")` automatically loads the bundled `x4-hardware-2026-07-12-v1.json`. The handshake and run
manifest record its ID, source, and calibrated status. The profile supplies deterministic p50 panel BUSY/driver timing,
separate fast/half reader render phases, SD open/transfer/close costs, cold indexing, cached/uncached image costs, and
JPEG and PNG thumbnail generation. Dedicated current-SD navigation splits File Browser's `38` ms entry-to-display total
into a `12` ms setup remainder and `26` ms paint, while Home's first and later paints use `72` and `12` ms p50 targets.
Reversible empty-to-nine-recent evidence shows that populated Home cost varies with the displayed top cover (`68-81` ms)
rather than scaling with list length; `72/12` ms remains the provisional central populated-screen compromise.
Full Settings paints use `27/36` ms first/later targets, the current English Reader Font Size popup uses `7` ms, and the
current Stormlight Reader menu uses `20.5/19` ms. The English Reader Orientation and Auto Turn overlays share a `7.5` ms
target. The Settings value is a provisional shared floor for non-clearing Settings overlays; only the current English
Reader Font Size layout is calibrated. Other/translated popup layouts, optional Footnotes/Bookmarks rows, and
book-specific chapter lists remain deferred.
Paint scopes pad only time not already consumed by native rendering or storage; older schema-version-1 profiles that omit
these optional fields retain their previous behavior. X3 deliberately keeps the `development-uncalibrated-v0` timing marker until equivalent
hardware captures exist. X4 PNG pacing is bounded to the measured RGB8 byte/area ranges and Home target; unsupported
formats, targets, and extrapolated sizes retain ordinary fallback timing.
Panel timings distinguish whole display intervals from controller BUSY. Natural FAST/HALF Reader calls use the measured
render-phase intervals; direct FULL `displayBuffer()` calls in Boot and Sleep consume the controlled
`operationUs - busyUs` transfer/setup remainder. The X4 single-buffer driver charges those remainders on its real plane
sequence: one plane before FAST BUSY and two after, or two before and two after HALF/FULL BUSY. Blocking-call duration stays
unchanged, while the BUSY trace marks the controller boundary and async refreshes omit the blocking path's post-refresh
re-seed. Active display intervals shorter than BUSY are rejected at startup. Controlled FAST/HALF `operationUs` values
remain profile evidence; current production firmware calls `HalDisplay::refreshDisplay()` only from calibration builds.
Cold indexing consumes the measured OPF, TOC, book-bin, total, and post-index boundaries separately. The five measured
files (`stormlight`, WOT, Sun Eater, Mistborn, and LOTR) use exact device-path records because their phases are
non-monotonic; this is deliberately not a size/count curve. Other EPUBs at or above the firmware's 400-spine threshold
retain the provisional Stormlight fallback, while unmeasured smaller books retain ordinary storage pacing. TOC replaces
generic bulk-transfer pacing only when one of those whole-phase profiles is active.
Warm cached opens likewise check exact Stormlight, WOT, Sun Eater, Mistborn, and LOTR path records before the large-spine
fallback. The records pad cached metadata and metadata-start-to-page-load separately; open-to-ready p90/MAD remain
tolerance evidence rather than a third delay. Renaming a book therefore preserves fallback behavior instead of silently
borrowing another path's content timing.

Pass `timing_profile=Path(...)` to select another versioned profile. The native executable exposes the same choice as
`--timing-profile FILE`; without it, native X3 and X4 runs use the old uncalibrated development timing. Profiles are
validated against the selected device and required measurement fields before the application starts.

The SD benchmark rate applies to ordinary files. Derived EPUB images use the workload's image timings because the
physical X4 renders its row-batched `.pxc` cache much faster than the calibration benchmark's generic buffer path. This
distinction reproduces the measured Stormlight cover instead of multiplying the raw 4 KiB cost across every grayscale
strip. Image scopes match the physical `Rendering image` through completion logs, including source/cache open. Cached
renders select the measured small/large displayed-area class (`20.5`/`147` ms p50). First decode/cache checks ten exact
source-byte/output-dimension observations (`73-1,978` ms) before falling back to the measured source-byte tiers: at most
16 KiB (`77` ms), at most 128 KiB (`1,079` ms), or larger (`1,871` ms). This captures WOT's 12,502-byte source displayed
at `464x587` (`650` ms), the three corpus covers, and six Stormlight images without pretending they form a throughput
curve. Unmeasured dimensions and sizes retain the coarse tiers.
The earlier ZIP-extraction-through-dimension-probing boundary likewise checks ten exact source-byte observations
(`158-1,845` ms) before its `159.5`/`443`/`1,736` ms tier fallback. Cold large-book loading separately retains the
measured `1,098` ms post-index CSS/cache-reload phase; generic storage timing is suppressed only inside these measured
whole-workload scopes.
Cold section construction uses two narrower boundaries: `Loading file` through newly streamed HTML is `327` ms p50 over
12 physical sections, and streamed HTML through the first image is `124` ms p50 over five image sections. Cached or reused
HTML bypasses both scopes. With WOT's exact preparation/decode phases applied, section load through first display is
`1,467` ms versus `1,425` ms physically.
Five physical JPEG thumbnails from 43,333 to 305,810 decoded bytes support a linear whole-workload model. Its fitted
nominal duration is `435,868 us + 10,590 ns * imageBytes`, with at most `9.38%` error on the five inputs; the scope covers
EPUB extraction, decode/scale, and BMP output without double-charging generic transfer pacing.
Four controlled RGB8 PNG covers at the fixed `135x226` Home target span `321x482` through `1284x1927` and
`551-5,987` ms whole-workload timings. ZIP extraction/preparation fits
`48,615 us + 6,975 ns * imageBytes` within `0.973%`; scanline conversion fits
`120,586 us + 2,168 ns * sourcePixels` within `4.359%`. Runtime applies the two phases only inside their measured
byte/area ranges, with RGB8 and target checks on conversion. The four profiled emulator replays are within `3.45%` of
their physical whole boundaries, while a `160x241` control remains on fallback rather than extrapolating.
Physical Power receipt to deep-sleep log is `5,371` ms, including `562` ms before Sleep activity entry. The runtime pads
Sleep-entry-to-deep-sleep to the measured `4,809` ms while retaining the existing fast/full panel phases.

For warm-load calibration, use `examples/calibrate_warm_epub.py`. It performs an untimed open, waits for the page and
Home thumbnail work to settle, then marks a second open. This avoids folding pending Home work into the warm EPUB
number. The native `wait_for_activity()` reports lifecycle entry; use `wait_for_render()` or `wait_for_panel_idle()` when
the assertion includes the first page paint. The X4 driver resumes at the controller BUSY boundary, re-seeds both panel
planes, and then returns to the application. `wait_for_panel_idle()` includes that whole blocking call and also advances
through any later camera-measured optical transition so a following screenshot is the visible state at that simulated
time.

After closing a completed run, derive a naturally rotated MP4 without rerunning the emulator:

```python
device.close()
device.export_video("artifacts/x3/presentation/page-turn.mp4", fps=10)
```

The exporter samples deduplicated canonical `panel.frame` events at fixed FPS, records the FFmpeg version/arguments and
sampled frame identities beside the MP4, and verifies encoded duration and frame count with FFprobe.

For visual review against a webcam capture, rectify the four physical screen corners and align both inputs at the same
scenario boundary:

```sh
uv run python examples/compare_physical_emulator.py \
  --physical ../../artifacts/calibration/x4-run/capture.mp4 \
  --physical-start 12.300 \
  --emulator ../../artifacts/emulator/run/replay.mp4 \
  --emulator-start 8.100 \
  --duration 3.0 \
  --corners 68,57,407,43,66,577,418,574 \
  --output ../../artifacts/comparison/review.mp4
```

Corner order is top-left, top-right, bottom-left, bottom-right in the naturally rotated camera video. The helper converts
both inputs to explicit output-frame ranges before stacking them; this avoids multi-input timestamp negotiation replaying
the wrong source frames. It does not infer the semantic alignment boundary—use recorder markers, device logs, and emulator
events for that. A JSON sidecar records both source hashes, requested and rounded frame boundaries, corners, FFmpeg version
and arguments, output hash, and FFprobe results. The command fails if either source cannot supply the requested frame count
instead of silently emitting a shorter review.

Validation:

```sh
uv run --extra dev pyright
uv run --extra dev pytest -q
```

## Physical-device calibration capture

Flash the calibration-only firmware, put the device in the scenario's starting state, and run the recorder on a host
that can see both USB serial and the webcam:

```sh
pio run -e calibration -t upload
cd emulator/python
uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --input-codec mjpeg \
  --fps 60 \
  --rotate 90 \
  --script examples/calibrate_page_turns.py \
  --output ../../artifacts/calibration/x3-page-turns-01
```

Record a reset-to-Home boot without writing another scenario:

```sh
uv run --extra calibration crosspoint-calibrate record-boot \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --input-codec mjpeg \
  --fps 30 \
  --video-size 640x480 \
  --rotate 90 \
  --output ../../artifacts/calibration/x4-boot-reset-01
```

`record-boot` starts the camera before invoking PlatformIO's esptool Python, opens serial immediately after the hard reset,
waits through Home's low-power log, and sends a harmless Back input so the attached reader remains awake. The manifest
records the exact reset command and host timestamps. Its defaults use `~/.platformio/penv/bin/python` and
`~/.platformio/packages/tool-esptoolpy/esptool.py`; override them with `--esptool-python` and `--esptool` when PlatformIO
is installed elsewhere. A reset failure still produces a failed manifest, FFmpeg log, reset log, and partial video.
`camera.fps` is the requested/encoded CFR, not a claim about real camera sampling. The recorder also reports pre-encoder
`decodedSourceFrames`, `observedSourceFps`, and source-frame interval min/p50/p90/max from FFmpeg's decoded timestamps.
This matters when FFmpeg duplicates frames: the current webcam produces 25 real FPS at 640x480 even when 30 or 60 FPS is
requested, so its optical quantization is 40 ms rather than the encoded frame interval.

Extract the natural Boot full-refresh pulse train with the same serial/video alignment used by the other analyzers:

```sh
uv run --extra calibration crosspoint-calibrate waveform-boot \
  --run ../../artifacts/calibration/x4-boot-reset-01 \
  --crop 90,70,270,450 \
  --output ../../artifacts/calibration/x4-boot-reset-01/boot-waveform.json
```

The analyzer derives a host/device clock origin only from pre-refresh serial lines because `displayBuffer()` blocks later
log delivery. Natural Boot evidence is a conformance check on the controlled full-refresh waveform, not another fitted
timing input.

The script is normal Python and defines `run(device)`. `HardwareDevice.press()`, `button_down()`, `button_up()`,
`button_pulse()`, `mark()`, `sleep()`, `wait_for_log()`, `wait_for_terminal_log()`, `wait_for_disconnect()`,
`benchmark_panel()`, `benchmark_sd()`, and
`benchmark_directory()` provide the reproducible control and calibration surface. `wait_for_log()` can be bounded by a
preceding marker's `host_time_ns`, so background firmware work is awaited without confusing an earlier matching line for
the current operation.
For workload calibration, `clear_epub_cache()` removes one named EPUB's complete derived cache, and `open_epub()`
bypasses only file-browser selection before entering the production reader/index/render path. Activity-changing direct
commands reset the production inactivity timer, matching the CPU-speed restoration of a real input transition. Calibration media must be
replaceable: clearing the cache also removes that book's saved progress. Settings navigation, rows, and option popups are
driven with physical buttons. `select_settings_on_home()` removes Home-selector and long-Back ambiguity by returning to
Home with Settings selected; Confirm still opens the real Settings activity. `select_files_on_home()` similarly gives
File Browser calibration a deterministic starting selection while Confirm still enters the production activity.
Synchronous cache, SD, directory, panel, and grayscale commands restore normal CPU frequency before entering their
measured boundary, so an idle device cannot scale the SPI or CPU portions of those samples.
Each run keeps `capture.mp4`, raw `serial.log`,
timestamped `events.jsonl`, FFmpeg diagnostics, and `manifest.json`. Set `--rotate` so the recorded device is upright for
natural review; canonical timing is still read from the unmodified video frame cadence and serial timestamps. The
manifest records the firmware version and scenario hash so a run remains attributable after the script changes.

Deep sleep is a terminal physical scenario. A script must opt in with `wait_for_terminal_log()` when USB remains
enumerated after the firmware stops, or `wait_for_disconnect()` when the board detaches. The recorder then omits the
unreachable `capture-end` marker and records the explicit terminal reason; unexpected disconnects remain failures. The
camera tail continues so the e-ink waveform can finish. Wake or reset the device before the next run.

Use `--input-format dshow --camera "Camera Name"` on Windows, or `--input-format avfoundation --camera 0` on macOS.
The normal `default` and release firmware environments do not include the calibration command surface.

Build a versioned profile only from completed runs. Repeat `--sd-run` when sizes are split across captures:

```sh
uv run --extra calibration crosspoint-calibrate waveform \
  --run ../../artifacts/calibration/x4-panel-optical-normal-v5 \
  --crop 90,70,270,450 \
  --threshold 10 \
  --boundary-ms 200 \
  --output ../../artifacts/calibration/x4-panel-optical-normal-v5/optical-waveform.json

uv run --extra calibration crosspoint-calibrate waveform-gray \
  --run ../../artifacts/calibration/x4-grayscale-optical-normal-v4 \
  --crop 90,70,270,450 \
  --threshold 3 \
  --window-ms 500 \
  --output ../../artifacts/calibration/x4-grayscale-optical-normal-v4/grayscale-waveform.json

uv run --extra calibration crosspoint-calibrate waveform-half \
  --run ../../artifacts/calibration/x4-stormlight-half-pages-v2 \
  --crop 90,70,270,450 \
  --threshold 5 \
  --window-ms 2200 \
  --output ../../artifacts/calibration/x4-stormlight-half-pages-v2/half-waveform.json

uv run --extra calibration crosspoint-calibrate waveform-fast \
  --run ../../artifacts/calibration/x4-stormlight-fast-pages-v2 \
  --crop 90,70,270,450 \
  --threshold 5 \
  --window-ms 800 \
  --output ../../artifacts/calibration/x4-stormlight-fast-pages-v2/fast-waveform.json

uv run --extra calibration crosspoint-calibrate waveform-image-fast \
  --run ../../artifacts/calibration/x4-stormlight-image-optical-normal-v1 \
  --crop 90,70,270,450 \
  --threshold 5 \
  --window-ms 800 \
  --minimum-image-area-fraction 0.5 \
  --target-dark-pixel-percent-min 50 \
  --output ../../artifacts/calibration/x4-stormlight-image-optical-normal-v1/image-fast-waveform.json

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_half_image_optical.py \
  --output ../../artifacts/calibration/x4-stormlight-half-image-optical-normal-v1 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_reader_settings.py \
  --output ../../artifacts/calibration/x4-stormlight-xl-font-normal-v6-repeat \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_directory.py \
  --output ../../artifacts/calibration/x4-directory-benchmark-normal-v3 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_cache_clear.py \
  --output ../../artifacts/calibration/x4-cache-clear-v2 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_epub_corpus.py \
  --output ../../artifacts/calibration/x4-epub-corpus-v2 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_png_thumbnail.py \
  --output ../../artifacts/calibration/x4-png-thumbnail-v1 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_png_thumbnail_series.py \
  --output ../../artifacts/calibration/x4-png-thumbnail-series-normal-v2 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_cold_index_repeats.py \
  --output ../../artifacts/calibration/x4-stormlight-index-repeats-normal-v1 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_cold_index_corpus_repeats.py \
  --output ../../artifacts/calibration/x4-corpus-index-repeats-normal-v1 \
  --fps 30 --video-size 640x480 --rotate 90

CROSSPOINT_WARM_EPUB=/sun-eater.epub uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_warm_epub_repeats.py \
  --output ../../artifacts/calibration/x4-sun-eater-warm-normal-v3 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_reader_navigation.py \
  --output ../../artifacts/calibration/x4-reader-navigation-controls-normal-v1 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_reader_percent_navigation.py \
  --output ../../artifacts/calibration/x4-reader-percent-controls-normal-v3 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_reader_popups.py \
  --output ../../artifacts/calibration/x4-reader-popups-normal-v2 \
  --fps 30 --video-size 640x480 --rotate 90

uv run --extra calibration crosspoint-calibrate record \
  --port /dev/ttyACM0 \
  --camera /dev/video0 \
  --script examples/calibrate_sleep_entry.py \
  --output ../../artifacts/calibration/x4-sleep-entry-v1 \
  --fps 60 --video-size 640x480 --rotate 90 --camera-tail 3

uv run --extra calibration crosspoint-calibrate analyze \
  --profile-id x4-hardware-2026-07-12-v1 \
  --page-run ../../artifacts/calibration/x4-page-turns-30-normal-v2 \
  --panel-run ../../artifacts/calibration/x4-panel-modes-normal-v2 \
  --sd-run ../../artifacts/calibration/x4-sd-small-normal-v2 \
  --sd-run ../../artifacts/calibration/x4-sd-512k-normal-v3 \
  --workload-run ../../artifacts/calibration/x4-stormlight-cold-normal-v3 \
  --workload-run ../../artifacts/calibration/x4-stormlight-xl-font-normal-v5 \
  --workload-run ../../artifacts/calibration/x4-stormlight-warm-quiescent-normal-v2 \
  --optical-analysis ../../artifacts/calibration/x4-panel-optical-normal-v5/optical-waveform.json \
  --grayscale-analysis ../../artifacts/calibration/x4-grayscale-optical-normal-v4/grayscale-waveform.json \
  --half-render-run ../../artifacts/calibration/x4-stormlight-half-pages-v2 \
  --half-waveform-analysis ../../artifacts/calibration/x4-stormlight-half-pages-v2/half-waveform.json \
  --fast-waveform-analysis ../../artifacts/calibration/x4-stormlight-fast-pages-v2/fast-waveform.json \
  --image-fast-waveform-analysis ../../artifacts/calibration/x4-stormlight-image-optical-normal-v1/image-fast-waveform.json \
  --directory-run ../../artifacts/calibration/x4-directory-benchmark-normal-v3 \
  --supplemental-workload-run ../../artifacts/calibration/x4-epub-corpus-v2 \
  --supplemental-workload-run ../../artifacts/calibration/x4-cache-clear-v2 \
  --supplemental-workload-run ../../artifacts/calibration/x4-sleep-entry-v3 \
  --supplemental-workload-run ../../artifacts/calibration/x4-library-navigation-v3 \
  --supplemental-workload-run ../../artifacts/calibration/x4-cache-population-v1 \
  --supplemental-workload-run ../../artifacts/calibration/x4-cache-population-v2-repeat \
  --supplemental-workload-run ../../artifacts/calibration/x4-cache-population-corpus-v1 \
  --supplemental-workload-run ../../artifacts/calibration/x4-cache-population-corpus-v2-noop-repeat \
  --supplemental-workload-run ../../artifacts/calibration/x4-png-thumbnail-normal-v9 \
  --supplemental-workload-run ../../artifacts/calibration/x4-png-thumbnail-series-normal-v2 \
  --supplemental-workload-run ../../artifacts/calibration/x4-stormlight-index-repeats-normal-v1 \
  --supplemental-workload-run ../../artifacts/calibration/x4-corpus-index-repeats-normal-v1 \
  --supplemental-workload-run ../../artifacts/calibration/x4-file-browser-controls-normal-v1 \
  --supplemental-workload-run ../../artifacts/calibration/x4-file-browser-controls-normal-v2 \
  --supplemental-workload-run ../../artifacts/calibration/x4-settings-controls-normal-v1 \
  --supplemental-workload-run ../../artifacts/calibration/x4-settings-controls-normal-v2 \
  --supplemental-workload-run ../../artifacts/calibration/x4-settings-popup-controls-normal-v1 \
  --supplemental-workload-run ../../artifacts/calibration/x4-settings-popup-controls-normal-v2 \
  --supplemental-workload-run ../../artifacts/calibration/x4-reader-navigation-controls-normal-v1 \
  --supplemental-workload-run ../../artifacts/calibration/x4-reader-navigation-controls-normal-v2 \
  --supplemental-workload-run ../../artifacts/calibration/x4-reader-percent-controls-normal-v3 \
  --supplemental-workload-run ../../artifacts/calibration/x4-reader-percent-controls-normal-v4 \
  --supplemental-workload-run ../../artifacts/calibration/x4-reader-popups-normal-v2 \
  --supplemental-workload-run ../../artifacts/calibration/x4-reader-popups-normal-v3 \
  --warm-workload-run ../../artifacts/calibration/x4-stormlight-warm-normal-v3 \
  --warm-workload-run ../../artifacts/calibration/x4-wot-warm-normal-v3 \
  --warm-workload-run ../../artifacts/calibration/x4-sun-eater-warm-normal-v3 \
  --warm-workload-run ../../artifacts/calibration/x4-mistborn-warm-normal-v2 \
  --warm-workload-run ../../artifacts/calibration/x4-lotr-warm-normal-v2 \
  --evidence-workload-run ../../artifacts/calibration/x4-stormlight-xl-font-normal-v6-repeat \
  --evidence-workload-run ../../artifacts/calibration/x4-home-controls-normal-v3 \
  --evidence-workload-run ../../artifacts/calibration/x4-home-recents-variants-normal-v1 \
  --output src/crosspoint_emulator/profiles/x4-hardware-2026-07-12-v1.json
```

The analyzer reports p50 as nominal, p90 as the tolerance boundary, and MAD as dispersion. It keeps whole panel-operation
time separate from the controller busy wait because e-ink transfer and visible waveform phases are not one delay.
The generic image tiers are copied into `models.imageFallback` with their source workload so native profile consumption
does not depend on raw workload key order. Older schema-version-1 profiles without that field retain the original lookup.
Evidence workloads are retained with the profile and its source manifest but excluded from every runtime model. The
independent XL-font repeat uses this path: its panel BUSY phases reproduce the fitted refresh modes while content-dependent
render and display totals remain scenario evidence rather than a second timing charge.
The settled Home-control run uses the same path: it records press-acknowledgment-to-render-start and paint distributions,
but does not add an input delay when the production GPIO/debounce path already reproduces the physical `20` ms nominal.
The reversible recents run contributes only its nine Confirm-hold-to-Confirmation samples; its population-dependent paint
costs remain outside the profile models as explicitly deferred evidence.
The two dedicated current-SD File Browser control runs supersede the older navigation run for File Browser entry and
paint timing. The older run remains the Home-render source; keeping the model keys separate prevents different captured
root contents and calibration revisions from silently producing an impossible whole-activity duration shorter than the
paint itself.
The two accepted Reader-percent runs exercise the fixed English selector at `0`, `1`, and `10` percent, restore the
initial value, cancel, and return Home. They drive only the selector's clear-and-redraw target; the failed v2 run and the
earlier pre-idle-fix v1 run are excluded, and translated layouts remain deferred evidence.
The two accepted Reader-popup runs use the calibration-only render-start timestamp because these overlays deliberately
reuse the existing framebuffer and never call `clearScreen`. Orientation paints at `8` ms nominal and Auto Turn at
exactly `7` ms; one pooled `7.5` ms Reader-menu popup target stays within `0.5` ms of either without adding popup-type
state. Back closes the popup on press and the menu on release, so the scenario waits for the resulting Reader repaint,
reopens the menu for the second popup, and changes neither value.
The settled Settings repeats contribute separate first and later full-screen paint targets. They deliberately exclude
option-popup draws, whose production path reuses the existing framebuffer without calling `clearScreen`; runtime applies
the targets only when the render actually cleared the screen, so popup queue timing is not charged twice.
Two later paired-control Settings captures use the calibration-only render boundary for the English Reader Font Size
overlay. Their 14 entry/Down/Up paints are `7` ms p50/p90 with `0` ms MAD, while their incidental full Settings paints
remain evidence-only so the accepted `27/36` ms parent-screen targets do not drift. The popup target is a floor: other or
translated option sets are uncalibrated but share that floor, retaining heavier native work instead of being shortened.
The reversible Reader-navigation repeats likewise contribute first and later paint targets for the current Stormlight
menu without optional Footnotes or Bookmarks rows. Those variants and the Stormlight chapter-list paints remain in the
workload evidence only rather than defining general menu-population or chapter-selector delays.
Cache-clear runs retain firmware-measured `Epub::clearCache()` durations and pre-deletion population counts. Missing
caches use their separate seven-sample p50; populated caches use the eleven-sample per-file fit at the synchronous production
mutation boundary. The evidence remains separate from ordinary SD-operation pacing.
The corpus scenario is grouped by its per-book markers so cache clear, indexing phases, cold-open readiness, cached Home
metadata, image work, one page render, and thumbnail generation remain attributable instead of being pooled across books.
It waits for the actual successful thumbnail log and then holds Home for two seconds, rather than assuming a fixed delay;
the resulting video contains a settled cover frame for physical/emulator review. It then performs three warm reopens per
book, returning to a settled Home screen between samples, and records open-to-ready, cached-metadata, and
metadata-to-page-load distributions separately from the thumbnail-overlapped Home metadata observation. Each warm sample
waits for the production `Progress saved` boundary before leaving Reader, preventing the next navigation from overlapping
an image-heavy page render.
Large cached books can also be measured independently with `calibrate_warm_epub_repeats.py`. Set
`CROSSPOINT_WARM_EPUB` to an SD-root path and reset the device between books; this avoids cross-book heap/lifecycle
accumulation while retaining three samples under one firmware/camera run. Completed isolated runs become exact per-path
cached-metadata and metadata-to-page-load records; renamed and unmeasured books retain the large-spine fallback.
`calibrate_cold_index_repeats.py` similarly isolates three cold Stormlight opens. Each sample enters File Browser through
physical input, clears the complete derived cache, waits for the first-page progress boundary, and returns Home before
the next sample. The analyzer combines those samples with the accepted core run, keeps p50/p90/MAD for every indexing
phase, and exposes rounded p50 scalars to the existing runtime scopes. If independently rounded phase p50s exceed the
measured total p50, their sum floors the runtime total instead of constructing an internally impossible workload.
`calibrate_cold_index_corpus_repeats.py` applies the same isolated boundary twice to WOT, Sun Eater, Mistborn, and LOTR
in one continuous run. Repeated-index groups are keyed by the measured device path, so the run cannot pool unlike books
or leak its incidental Home metadata, cache clearing, section, or image work into another profile model.
`books-for-testing/png-cover-calibration-small.epub` is a locally ignored, minimal one-chapter fixture with a `963x1445`
RGB PNG cover compressed to 51,237 bytes. Three locally ignored variants retain that RGB8/four-gray-level content at
`321x482`, `642x963`, and `1284x1927`. `calibrate_png_thumbnail.py` provisions the single fixture through the
calibration-only serial upload boundary, which
writes 96-byte hex-encoded commands to a temporary SD file, acknowledges every chunk, and verifies the complete size and
FNV-1a checksum before renaming it into place. The script then clears
its cache, opens it through the production reader, waits for the successful PNG-thumbnail log, and retains a settled
naturally oriented Home frame. `calibrate_png_thumbnail_series.py` repeats that production path for all three variants
under one naturally rotated capture, while marker grouping keeps each thumbnail boundary separate. The fixtures isolate
PNG thumbnail cost from a large EPUB index.
Successful thumbnail boundaries retain preparation and conversion durations, archive-compressed and decoded byte counts,
source dimensions, target dimensions, and image format already emitted by firmware. The five JPEG observations drive
the byte-scaled whole-workload model; the four PNG observations drive only the bounded two-phase model. Non-monotonic indexing phases are consumed as
exact device-path records only; they are not generalized to renamed or unmeasured books.
Root-directory open and `openNextFile` timing are measured separately from regular file opens so library navigation does
not inherit the much slower EPUB-open cost. Optical analysis requires first-decoded-frame camera alignment, verifies the capture hash, and retains separate black/white
summaries; the crop is the inner panel in the camera's natural 480x640 frame, excluding the bezel.
The grayscale scenario re-establishes a black production-style BW base before every timed custom-LUT activation, and
records device-measured start offsets so primary refresh and cleanup transfers stay outside each optical window. At
normal CPU speed all ten after-half samples stay below the camera-change threshold, matching the firmware constraint that
a half refresh sets particles too firmly for this grayscale LUT to adjust. The profile records that absence explicitly;
it does not lower the threshold into camera noise or invent light/dark transition phases.
The supplemental half-render run forces a half refresh on every page through Settings, collects a larger natural reader
sample, and restores the normal 15-page cadence before it exits. Its page-content waveform analysis measures the two
repeatable half-refresh peaks separately: the panel first drives an inverted new page, then settles to the target. The
fast-page capture starts from the verified 15-page baseline and records one short direct old-to-new transition.
The image-heavy fast analysis accepts only page operations that render an image over at least half of the X4 panel. Eight
independent full-cover samples move through a lighter drive at `248.283` ms p50 and settle at `342.846` ms p50. Canonical
cover targets are over 72% dark while observed text/control targets remain below 7%, so the runtime conservatively selects
this schedule only when at least 50% of target pixels are dark; all other fast refreshes retain the text schedule.
`calibrate_half_image_optical.py` sets the cleanup cadence to every page, waits for each full-cover render to finish, and
restores 15 pages afterward. The production reader still uses its documented double-FAST image blank/restore path: HALF
would set particles too firmly for the following grayscale LUT. Nine independent majority-dark operations repeat the
image schedule at `254.012/334.228` ms p50 start/end. Pooling both captures gives `253.734/337.073` ms across 17 samples;
the shifts from the applied values are under 9 ms and therefore below the observed 40 ms source-frame resolution, so this
validation run does not create an image-HALF schedule or retune the existing image-FAST model.
The five profile panel/grayscale/reader optical inputs all decode at 25.0 FPS (`40.0` ms p50, `40.1` ms p90 source
intervals), although their CFR MP4s and older manifests say 30 FPS. Regenerated waveform sidecars and the profile now
record both rates and use 40 ms as optical quantization; their transition p50s are unchanged. Three natural Boot captures
measure a `3,903-3,904` ms full operation. Their main pulse medians are `283.33/283.335/266.67` ms, within one real camera
frame of the controlled `266.7` ms value, so Boot does not introduce a second full-refresh schedule.

To replay both Reader popups through production controls and export a naturally oriented review video:

```sh
uv run python examples/replay_reader_popups.py --sd FIXTURE --artifacts ARTIFACTS --fps 30
```

`examples/replay_settings_navigation.py --physical-cadence` reproduces the calibration script's overlapping 500 ms
Settings inputs instead of waiting for every e-ink update. This is useful for reviewing queued renders and option-popup
transitions against the physical capture; without the flag, each action uses explicit panel-idle waits.

`examples/replay_sleep_reset.py` holds Power through the production sleep path, verifies that the application task becomes
quiescent while protocol control remains available, and resets from the retained panel. It checks that the reset snapshot
is byte-identical to the visible sleep panel and exports separate sleep-entry and reset-to-Home MP4s.

`examples/replay_image_pages.py` cold-opens a one-book fixture and advances through the first N pages with panel-idle waits.
It covers the large, medium, and small image decode/cache classes and can export a naturally oriented MP4 for comparison
with `calibrate_large_epub.py`.

`examples/replay_cache_clear.py` cold-opens a one-book fixture, returns to an idle File Browser, clears that EPUB through
the production cache path, and verifies another cold open:

```sh
uv run python examples/replay_cache_clear.py \
  --sd FIXTURE --artifacts ARTIFACTS --book /book.epub
```

To replay one calibrated half-refresh page turn and export its natural-orientation MP4, prepare a one-book fixture with
`/.crosspoint/settings.json` containing `{"refreshFrequency":0}`, then run:

```sh
uv run python examples/replay_page_refresh.py --sd FIXTURE --artifacts ARTIFACTS --name page-turn --fps 30
```
