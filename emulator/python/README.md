# CrossPoint emulator Python client

Build the native executable with `pio run -e emulator`, then run scripts through the locked `uv` project:

```python
from crosspoint_emulator import Emulator

with Emulator.launch("x3", sd="test/epubs", artifacts="artifacts/x3") as device:
    device.wait_for_activity("home")
    device.wait_for_panel_idle()
    device.press_action("confirm")
    device.wait_for_activity("file_browser")
    device.screenshot("file-browser.png")
```

`press_action()` resolves the current firmware mapping but still injects the resulting physical button through the real
ADC/debounce/HAL path. Use `button_down()`/`button_up()` for physical chords and `action_down()`/`action_up()` for explicit
holds. Every wait has independent simulated-time and wall-time limits.

`screenshot()` produces a naturally rotated review image. `capture_panel()` and `capture_framebuffer()` retain the native
controller orientation for deterministic regression checks. On failures, temporary run artifacts are retained; successful
temporary runs are removed unless `keep_artifacts=True`.

`reset()` ends the native process and relaunches it from a snapshot of the writable SD directory and visible panel PNG,
discarding volatile process state while retaining the same observable state a hardware reset preserves.

After closing a completed run, derive a naturally rotated MP4 without rerunning the emulator:

```python
device.close()
device.export_video("artifacts/x3/presentation/page-turn.mp4", fps=10)
```

The exporter samples deduplicated canonical `panel.frame` events at fixed FPS, records the FFmpeg version/arguments and
sampled frame identities beside the MP4, and verifies encoded duration and frame count with FFprobe.

Validation:

```sh
uv run --extra dev pyright
uv run --extra dev pytest -q
```
