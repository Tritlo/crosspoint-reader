# CrossPoint emulator

Run the real CrossPoint application on Linux with deterministic X3/X4 hardware, storage, input, clock, and e-ink panel
models. Automation is ordinary Python: a script launches a device, drives controls, waits for observable state, and asks
for PNGs or an MP4.

## Build the native runner

From the repository root:

```sh
pio run -e emulator
```

The Python client finds `.pio/build/emulator/program` when the current directory is inside this checkout. From elsewhere,
pass `executable=...`, set `CROSSPOINT_EMULATOR_EXECUTABLE`, or use the command's `--executable` option.

## Write a script

```python
from pathlib import Path

from crosspoint_emulator import Emulator

artifacts = Path("artifacts/review")

with Emulator.launch("x4", sd="books", artifacts=artifacts) as device:
    device.wait_for_activity("home")
    device.wait_for_panel_idle()
    device.press_action("confirm")
    device.wait_for_activity("file_browser")
    device.wait_for_panel_idle()
    device.screenshot("file-browser.png")

device.export_video(artifacts / "run.mp4", fps=10)
```

Every run writes a deterministic event/frame trace. `screenshot()` writes a naturally oriented review PNG at that point;
`capture_panel()` and `capture_framebuffer()` retain controller-native geometry. After the session closes,
`export_video()` samples the trace into a deterministic MP4. A script can request PNGs, video, both, or neither.

Use `press_action()` for firmware-mapped intent such as `page_forward`. Use `button_down()` and `button_up()` when a test
needs an exact physical hold or chord. Waits use simulated time and a separate wall-time deadlock limit.

## Run it

Inside this checkout:

```sh
uv run --project emulator/python crosspoint-emulator emulator/python/examples/review_home.py \
  --device x4 --artifacts artifacts/review --video
```

An isolated GitHub-source install works through `uvx` while the native runner remains the locally built platform artifact:

```sh
uvx --from 'git+https://github.com/Tritlo/crosspoint-reader.git@emulator#subdirectory=emulator/python' \
  crosspoint-emulator --executable .pio/build/emulator/program \
  emulator/python/examples/review_home.py --device x4 --artifacts artifacts/review --video
```

Arguments after the script path belong to the script. The command itself only makes the installed package available and
sets optional runner/profile overrides; it does not impose a second scenario language.

For a persistent local install, use `uv tool install ./emulator/python`. Wheels and source distributions include the
calibrated X4 timing JSON. X3 intentionally remains marked uncalibrated until an X3 profile is measured.

## Reproducibility and fixtures

Each launch copies the supplied SD fixture into isolated run storage; the source directory is never mutated. The default
RTC is `2000-01-01T00:00:00Z` and the default seed is `0`. Pass `rtc_start=...` and `seed=...` explicitly when a scenario
needs other deterministic values.

`reset()` restarts the native process from the current writable SD snapshot and visible panel. Temporary successful runs
are removed unless `keep_artifacts=True`; failed runs are retained.

## Extension points

- Add user workflows as normal Python scripts. The public surface is `Emulator` plus capture/video helpers.
- Add a device profile in native `Configuration`, then expose the profile name in `DeviceProfile`.
- Add hardware behavior at the existing native HAL boundaries under `emulator/native`; keep application code shared with
  firmware.
- Add calibrated timing as versioned package data. Runtime models consume nominal p50 boundaries and retain p90/MAD as
  evidence rather than hidden delays.
- Extend the framed JSON-RPC protocol only for behavior that cannot be expressed through controls, time, storage fixtures,
  captures, or existing read-only waits.

The physical-device capture harness and the detailed X4 timing provenance are maintainer surfaces documented in
[CALIBRATION.md](CALIBRATION.md). The architecture and deferred fidelity limits live in
[`docs/emulator-plan.md`](../../docs/emulator-plan.md).

## Validate changes

```sh
pio test -e emulator
pio run -e emulator
uv run --project emulator/python --extra dev pyright
uv run --project emulator/python --extra dev pytest -q
uv build --project emulator/python
```
