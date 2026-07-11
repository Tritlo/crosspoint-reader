# CrossPoint Emulator

Vocabulary for reproducing Xteink X3 and X4 behavior on a development host.

## Language

**Emulator**:
A deterministic host-native execution of CrossPoint application behavior against a selected device profile. It does not execute the ESP32 firmware binary.
_Avoid_: Firmware emulator, ESP32 emulator

**Device profile**:
The X3 or X4 geometry, capabilities, timing, storage, input, and e-ink behavior selected for an emulator run.
_Avoid_: Board type, skin

**Framebuffer capture**:
The pixels composed for the display before e-ink refresh behavior is applied.
_Avoid_: Screenshot

**Panel capture**:
The visible e-ink state at a specific simulated time, including refresh transitions and retained artifacts.
_Avoid_: Screenshot, framebuffer capture

**Presentation capture**:
A derived human-facing rendering of a panel capture that may add device framing, scaling, orientation, inputs, or timing overlays. It is not test truth.
_Avoid_: Panel capture, golden image

**Panel recording**:
A time-resolved sequence of panel captures from one emulator run, exportable as video.
_Avoid_: Screen recording

**Run trace**:
A replayable record of one emulator run containing its versioned profiles, simulated-time events, device controls, automation signals, timing spans, storage operations, and panel transitions.
_Avoid_: Log, video

**SD fixture**:
An initial directory tree mounted as the emulated SD card. Each run receives isolated writable storage without modifying the fixture.
_Avoid_: SD image, mounted directory

**Device control**:
An emulator action available on the physical device boundary, such as button input, power, reset, time, or SD-card contents. Automated tests change behavior only through device controls.
_Avoid_: State injection, test mutation

**Automation signal**:
A read-only semantic observation used to synchronize or assert an automated run, such as the current activity, panel state, timing span, or storage operation.
_Avoid_: Test hook, backdoor

**Activity ID**:
A stable identity for the currently active screen, exposed as an automation signal independently of its C++ class or displayed title.
_Avoid_: Activity name, class name

**Simulated time**:
The canonical clock of an emulator run, advanced deterministically by execution and modeled device operations.
_Avoid_: Wall time, elapsed time

**Pacing**:
The rate at which simulated time is presented in wall time. Pacing may be unbounded for tests or real-time-scaled for interactive use without changing run results.
_Avoid_: Simulation speed, device speed

**Timing profile**:
A versioned, deterministic set of operation costs calibrated for one device profile from repeated hardware measurements.
_Avoid_: Benchmark, delay table

**Panel model**:
A calibrated behavioral representation of a device's visible e-ink state, refresh phases, timing, retention, and ghosting. It is not an electrochemical or cycle-accurate simulation of the panel.
_Avoid_: Framebuffer, physical panel simulation

**Initial panel state**:
The visible e-ink content retained before an emulator boot begins. It is explicit run input and survives reset or sleep unless a fresh run replaces it.
_Avoid_: Boot screen, initial framebuffer
