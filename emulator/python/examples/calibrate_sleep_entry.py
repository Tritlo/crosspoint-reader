from crosspoint_emulator.hardware import HardwareDevice


def run(device: HardwareDevice) -> None:
    """Hold Power until the X4 enters deep sleep and intentionally disconnects USB."""
    device.mark("sleep-power-down")
    device.button_pulse("power", hold_ms=800)
    device.wait_for_terminal_log("[DBG] [MAIN] Entering deep sleep", timeout_s=20)
